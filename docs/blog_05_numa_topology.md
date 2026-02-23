<!-- SPDX-License-Identifier: GPL-2.0 -->
<!-- Copyright (c) 2026 Graziano Labs Corp. -->

# NUMA Topology: The Silent Performance Killer on Multi-Socket Systems

*Part 5 of 9 in a series on building a host-side data path emulator for AI infrastructure*

---

On a multi-socket server, every page of memory belongs to a NUMA node. A thread on socket 0 accessing memory on socket 1 pays a latency and bandwidth penalty that ranges from 30% to over 100%, depending on the topology and the number of hops. There is no error. There is no warning in dmesg. The kernel does not log anything. The allocation succeeds, the data is correct, and the throughput is halved. This is NUMA disaggregation, and it is the single most common source of unexplained performance variance on multi-socket training nodes.

Phase 5 adds three capabilities to dmaplane: NUMA-aware buffer allocation via `alloc_pages_node`, a topology query ioctl that exposes the ACPI SLIT distance matrix, and an NxN cross-node bandwidth benchmark that quantifies the penalty. The goal is to make NUMA placement visible and measurable before Phase 8 introduces GPU memory, where a misplaced host staging buffer means every GPU-to-NIC transfer crosses the interconnect.

---

## The Allocation Change: alloc_pages to alloc_pages_node

**Phase 2 allocated pages with `alloc_pages(GFP_KERNEL, 0)`, which requests memory from the current CPU's local node.** This is fine when the caller does not care where pages land. But when a buffer will be DMA-read by a NIC on socket 1, allocating it on socket 0 means the NIC's DMA engine pays the cross-socket penalty on every read.

The change is a single function call:

```c
int alloc_node = (target_node == DMAPLANE_NUMA_ANY)
                  ? NUMA_NO_NODE : target_node;

buf->pages[i] = alloc_pages_node(alloc_node,
                                 GFP_KERNEL | __GFP_ZERO, 0);
```

`alloc_pages_node` takes a NUMA node ID and walks that node's zonelist first. Each NUMA node maintains independent free page pools per zone (ZONE_DMA, ZONE_NORMAL, ZONE_MOVABLE). The buddy allocator tries the target node's zones in preference order, then falls back to progressively more distant nodes if the target is exhausted.

The fallback is silent. `alloc_pages_node` is best-effort -- there is no `GFP_STRICT_NODE` flag, no `-ENOMEM` specific to "requested node was full." If node 1 has no free pages in any zone, the allocator walks the zonelist to node 0, allocates there, and returns success. The caller receives a valid `struct page *` with no indication that the page is on the wrong node. This design is intentional -- the kernel prefers a successful allocation on the wrong node over an allocation failure -- but it means the caller must verify placement after the fact.

When `target_node` is `DMAPLANE_NUMA_ANY`, the code passes `NUMA_NO_NODE`, which is equivalent to plain `alloc_pages` -- the allocator uses the current CPU's local node with standard fallback. This preserves backward compatibility with Phase 2's behavior.

---

## Post-Hoc Verification: page_to_nid Is the Only Detection Mechanism

**After allocation, `page_to_nid()` reads the node ID from the page's `struct page` flags -- the only way to detect silent fallback by the buddy allocator.**

```c
if (target_node != DMAPLANE_NUMA_ANY) {
    int actual = page_to_nid(buf->pages[i]);

    if (actual == target_node)
        local_count++;
    else
        remote_count++;
}
```

The code counts local vs. remote pages across the entire allocation. If any pages land on the wrong node, a `pr_warn` fires with the exact count of misplaced pages. The buffer's `actual_numa_node` field reports the majority node, so userspace knows where the bulk of the data lives.

Three counters track allocation outcomes across the device lifetime:

- `numa_local_allocs` -- allocation landed entirely on the requested node
- `numa_remote_allocs` -- at least one page fell back to a different node
- `numa_anon_allocs` -- allocation used `DMAPLANE_NUMA_ANY` (no target)

These are returned via the existing `IOCTL_GET_BUF_STATS`. On a healthy system with adequate per-node memory, `numa_remote_allocs` should be zero. A non-zero value means the target node is under memory pressure, and buffers are silently misplaced.

---

## Coherent Memory Cannot Be Steered

**`dma_alloc_coherent` has no NUMA node parameter.** It allocates physically contiguous, cache-coherent memory from the device's local node -- determined by the platform device's NUMA affinity, not by any caller-specified preference. The kernel API simply does not expose a node selector for coherent allocations.

This is why Phase 2's design split matters. Coherent allocations (`BUF_TYPE_COHERENT`) are for small control structures -- doorbell pages, completion queue entries -- where physical contiguity and hardware coherence are required. Large streaming buffers (`BUF_TYPE_PAGES`) use the page-backed path, which supports `alloc_pages_node` and therefore NUMA steering. The coherent path reports `actual_numa_node` via `page_to_nid(virt_to_page())` for informational purposes, but the caller cannot control where the memory lands.

---

## Topology Query: ACPI SLIT Distances and What They Mean

**The ACPI SLIT (System Locality Information Table) is firmware's description of the relative cost of cross-node memory access.** The kernel exposes it through `node_distance(a, b)`, which returns a dimensionless integer normalized so that local access equals 10.

Typical values on x86 server hardware:

| Distance | Meaning | Example |
|----------|---------|---------|
| 10 | Same node (local) | CPU and memory on the same socket |
| 20-21 | One QPI/UPI hop | Adjacent sockets in a 2-socket system |
| 30-31 | Two hops | Distant socket in a 4-socket ring topology |

The `IOCTL_QUERY_NUMA_TOPO` ioctl returns the full NxN distance matrix alongside per-node CPU counts and memory sizes. This is the kernel equivalent of `numactl --hardware` or `lstopo`, but available programmatically without spawning a process. The struct compacts sparse kernel node IDs into contiguous array indices -- node IDs can be non-contiguous (e.g., node 0 and node 2 online, node 1 offline), and the ioctl handles the mapping internally.

On a single-socket development workstation, the output is a 1x1 matrix:

```
NUMA Topology:
  Nodes: 1   CPUs: 32
  Node 0: 32 CPUs, 130048 MB total, 112534 MB free

NUMA Distance Matrix:
         node0
  node0  10
```

On a dual-socket c5.metal (2x Xeon Platinum 8124M):

```
NUMA Topology:
  Nodes: 2   CPUs: 96
  Node 0: 48 CPUs, 97936 MB total, 93898 MB free
  Node 1: 48 CPUs, 98304 MB total, 95615 MB free

NUMA Distance Matrix:
       node0  node1
  node0 10     21
  node1 21     10
```

The asymmetry between 10 (local) and 21 (remote) is the penalty factor. A distance of 21 means remote access costs roughly 2.1x local access in latency. Bandwidth penalties are lower than the latency ratio suggests -- 18% on this Skylake-SP platform with 64 MB buffers -- because bandwidth depends on link width and saturation, not just hop count. On older microarchitectures or 4-socket systems with 2-hop paths (distance 30+), the bandwidth penalty grows to 40-60%.

---

## The Bandwidth Benchmark: NxN Matrix via Pinned Kthreads

**The cross-node benchmark measures memcpy throughput for every (cpu_node, buf_node) pair.** For each cell in the NxN matrix, the driver allocates a target buffer on `buf_node`, a scratch buffer on `cpu_node`, spawns a kthread pinned to `cpu_node`, and measures `iterations` memcpy operations:

```c
thread = kthread_create(numa_bench_thread, &cell,
                        "dmaplane_numa_%d_%d",
                        cpu_node, buf_node);
set_cpus_allowed_ptr(thread, cpu_mask);
wake_up_process(thread);
```

The kthread is pinned to all CPUs on the target node via `set_cpus_allowed_ptr`, not to a single core. This is more realistic -- real workloads use all cores on a socket -- and avoids single-core thermal throttling artifacts.

**Each cell runs sequentially, not in parallel.** Parallel execution would saturate the memory bus and under-report per-cell bandwidth. The benchmark measures the available bandwidth for a single streaming writer, which is the relevant metric for a data loader thread filling a gradient staging buffer. Between cells, `cond_resched()` yields to reduce thermal throttling effects on long runs.

The benchmark uses plain `memcpy` rather than DMA operations. CPU memcpy measures the CPU-to-memory path that NUMA placement directly affects. DMA throughput depends on IOMMU configuration and device characteristics that are orthogonal to NUMA placement. In data loading pipelines, the CPU memcpy from the data loader into the staging buffer is the bottleneck that NUMA placement controls.

Both buffers are touched with `memset` before timing begins, ensuring all pages are faulted in. Without this warmup, the first iteration includes page fault overhead that distorts the measurement.

On a single-socket development workstation, the benchmark produces a 1x1 matrix with only local bandwidth -- the interesting case requires multi-socket hardware.

On a dual-socket c5.metal (2x Xeon Platinum 8124M, 96 CPUs, 36 MB L3 per socket), the results depend critically on buffer size.

With 1 MB buffers:

```
Cross-Node Bandwidth Matrix (MB/s):
  cpu\buf | node0  | node1  |
  ------------------------
  node0   |  8015  |  8983  |
  node1   |  8945  |  9637  |

Cross-Node Penalty Analysis:
  node0 -> node1: 0% penalty (8983 vs 8015 MB/s)
  node1 -> node0: 8% penalty (8945 vs 9637 MB/s)
```

No penalty. Remote is actually *faster* for node 0. This is wrong -- or rather, it is measuring the wrong thing. Both the 1 MB source and 1 MB destination fit entirely in the 36 MB L3 cache. After the `memset` pre-touch, both buffers are L3-resident. The benchmark is measuring cache-to-cache bandwidth, not DRAM-to-DRAM bandwidth. The NUMA penalty is invisible because the data never leaves the cache hierarchy.

With 64 MB buffers (exceeding the 36 MB L3):

```
Cross-Node Bandwidth Matrix (MB/s):
  cpu\buf | node0  | node1  |
  ------------------------
  node0   |  6778  |  5577  |
  node1   |  5013  |  6095  |

Cross-Node Penalty Analysis:
  node0 -> node1: 18% penalty (5577 vs 6778 MB/s)
  node1 -> node0: 18% penalty (5013 vs 6095 MB/s)
```

Now the penalty is visible and symmetric. Local access averages ~6.4 GB/s, remote ~5.3 GB/s, an 18% penalty in both directions. The 64 MB working set exceeds L3 capacity, forcing the memcpy to stream from DRAM, and cross-node access pays the UPI interconnect cost.

The 18% penalty on Skylake-SP is lower than the 40-50% seen on older Broadwell Xeons. Intel's wider UPI mesh in the Skylake generation narrows the gap. But 18% still compounds: across thousands of all-reduce iterations in a training run, an 18% throughput loss on every gradient transfer adds up to hours of wasted compute.

**The L3 caching effect is itself a lesson.** Production gradient buffers and KV-cache blocks are hundreds of megabytes to gigabytes -- they will never fit in L3. But microbenchmarks often use small buffers for speed, and the results look great because the cache hides the penalty. Any NUMA benchmark that reports zero cross-node penalty should be suspected of measuring cache, not memory. The fix is simple: use buffers larger than the last-level cache. On a 4-socket system with 2-hop paths and distance-30 nodes, the penalty at DRAM-scale buffers can exceed 60%.

---

## Connection to NCCL: Why GPU Training Cares About NUMA

**NCCL's `ncclCommInitRank` performs topology detection for exactly this reason.** When NCCL initializes a communicator, it queries `nvidia-smi topo -m` to discover the PCIe topology: which GPU is on which NUMA node, which NIC is on which socket, and what the interconnect paths look like. A GPU on socket 0 paired with a NIC on socket 1 produces no error -- the all-reduce completes, gradients synchronize, training continues -- but the effective all-reduce bandwidth drops by 40-100% because every DMA transfer between the GPU and NIC crosses the UPI interconnect.

The NUMA benchmark measures exactly this cost at the CPU-to-memory level. The same penalty applies to DMA: when a NIC on node 1 DMA-reads a buffer whose pages are on node 0, the NIC's DMA engine pays the cross-socket penalty on every cacheline fetch. The penalty compounds with buffer size -- a 1 GB gradient buffer with 40% cross-node penalty wastes hundreds of milliseconds per all-reduce step, multiplied by thousands of steps per epoch.

---

## Single-Socket Development: What You See vs. Production

**On a single-socket workstation, all NUMA tests pass with trivial results.** One node, a 1x1 distance matrix with `distance[0][0] = 10`, and all allocations landing on node 0. On the dual-socket c5.metal, the stats after running the full regression suite (phases 1-5) show:

```
NUMA Stats:
  numa_local_allocs:  2
  numa_remote_allocs: 0
  numa_anon_allocs:   1093
```

The 1093 anonymous allocations come from Phase 2/3/4 regression tests, all using `DMAPLANE_NUMA_ANY`. The 2 local allocations are from Phase 5's explicit node-0 tests. Zero remote allocations confirms that with 94 GB free per node, the buddy allocator never falls back. After a clean module reload (double-cycle test), the stats reset to `local=2, remote=0, anon=1` -- only Phase 5's own allocations.

The test suite validates eight scenarios: topology query, allocation on a specific node, NUMA_ANY allocation, invalid node rejection, coherent buffer NUMA reporting, NUMA stats counters, the cross-node benchmark, and a regression test that confirms mmap-and-pattern-write still works with NUMA-aware allocation. All eight pass on single-socket hardware. On multi-socket hardware, the benchmark test additionally reveals the penalty matrix.

---

## Connection Forward: GPU Memory and NUMA Placement

Phase 8 introduces GPU memory integration -- pinning VRAM pages via `nvidia_p2p_get_pages` and registering them as RDMA memory regions. When a GPU on socket 0 needs to send gradient data to a remote machine via a NIC on socket 0, the host staging buffer must also be on socket 0. If the staging buffer is on socket 1, the transfer path is GPU (socket 0) -> PCIe -> CPU (socket 0) -> UPI -> memory (socket 1) -> UPI -> CPU (socket 0) -> PCIe -> NIC (socket 0). Every byte crosses the UPI interconnect twice.

With NUMA-aware allocation, the staging buffer lands on socket 0: GPU (socket 0) -> PCIe -> memory (socket 0) -> PCIe -> NIC (socket 0). No UPI crossing. The NUMA benchmark from this phase quantifies the cost difference, and the `alloc_pages_node` infrastructure ensures the staging buffer lands where it needs to be.

---

*Next: [Part 6 -- Backpressure & Flow Control](/docs/blog_06_backpressure.md)*
