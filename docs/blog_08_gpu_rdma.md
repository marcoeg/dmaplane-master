<!-- SPDX-License-Identifier: GPL-2.0 -->
<!-- Copyright (c) 2026 Graziano Labs Corp. -->

# GPU Memory Over the Wire: Building the GPUDirect RDMA Data Path

*Part 8 of 9 in a series on building a host-side data path emulator for AI infrastructure*

---

Seven phases built the host-side data path layer by layer: ring buffers, DMA allocation, dma-buf zero-copy sharing, RDMA transport, NUMA topology, flow control, and instrumentation. Every byte that moved through that path originated in host DRAM -- pages allocated by `alloc_pages_node`, mapped through the IOMMU, sent over Soft-RoCE. But production AI systems do not send host DRAM over the network. They send GPU VRAM. When a prefill GPU finishes computing a KV-cache and needs to ship it to a decode GPU on another machine, the data starts in GPU memory and should go directly onto the wire without ever landing in host DRAM. This is what GPUDirect RDMA does, and Phase 8 builds it.

This post covers what happened when dmaplane learned to pin GPU VRAM, map it through the PCIe BAR, register it as an RDMA Memory Region, and send it over real Ethernet to a second machine. The receiver had no GPU. It verified the data byte-for-byte. Every link in the chain was real hardware and real protocol.

---

## The Problem: GPU Memory Is Not Host Memory

Everything in the first seven phases assumed `struct page` -- the kernel's fundamental unit of memory tracking. Every DMA API, every RDMA verb, every `mmap` implementation in dmaplane converts between virtual addresses, physical addresses, and DMA addresses through `struct page` pointers. GPU VRAM does not have `struct page` entries. It lives behind a PCIe Base Address Register (BAR) -- a window of physical address space that the GPU exposes to the host CPU. When the CPU reads or writes through a BAR mapping, each access becomes a PCIe Transaction Layer Packet that crosses the bus to the GPU's memory controller. From the kernel's perspective, BAR memory is MMIO: you access it via `ioremap`, not `virt_to_page`.

This means none of the host-side infrastructure works for GPU memory directly. `dma_map_sg` expects `struct page` arrays. `ib_reg_mr` expects host-accessible memory that `get_user_pages` can pin. GPU BAR pages fail all of these paths. You need a different entry point into the kernel's DMA and RDMA machinery: `nvidia_p2p_get_pages`.

---

## symbol_get/symbol_put: The GPL/Proprietary Bridge

The first challenge was not algorithmic but jurisdictional. NVIDIA's driver is proprietary. dmaplane is GPL. On kernel 6.5+, the module loader enforces a strict rule: a module that imports GPL-only symbols from any source cannot also statically depend on a proprietary module. Since dmaplane uses `EXPORT_SYMBOL_GPL` symbols from `ib_core` (RDMA verbs), a static dependency on `nvidia.ko` causes `insmod` to fail:

```
module using GPL-only symbols uses symbols from proprietary module
```

The solution was runtime symbol resolution. At module load, `dmaplane_gpu_init()` in `driver/gpu_p2p.c` resolves the three NVIDIA P2P functions dynamically:

```c
nv_get_pages = (nv_p2p_get_pages_fn_t)symbol_get(nvidia_p2p_get_pages);
nv_put_pages = (nv_p2p_put_pages_fn_t)symbol_get(nvidia_p2p_put_pages);
nv_free_page_table = (nv_p2p_free_page_table_fn_t)symbol_get(nvidia_p2p_free_page_table);

if (!nv_get_pages || !nv_put_pages || !nv_free_page_table) {
    pr_err("failed to resolve NVIDIA P2P symbols -- is nvidia.ko loaded?\n");
    dmaplane_gpu_exit();
    return -ENOENT;
}
```

`symbol_get()` performs a runtime lookup in the kernel symbol table and increments the owning module's refcount, preventing `nvidia.ko` from being unloaded while dmaplane holds references. Unlike static symbol resolution at `modpost` and `insmod` time, `symbol_get()` does not create a module dependency in the `depmod` database. At unload, `dmaplane_gpu_exit()` calls `symbol_put()` for each resolved symbol, decrementing the refcounts.

This pattern means the module compiles and loads whether or not the NVIDIA driver is present. GPU features activate when the symbols are available and degrade gracefully when they are not.

---

## Pinning GPU VRAM: BAR Pages Without struct page

`nvidia_p2p_get_pages` takes a CUDA virtual address and returns a table of BAR page descriptors -- physical addresses of 64KB pages in the GPU's BAR1 aperture. These pages are "pinned" in the sense that the GPU driver guarantees they will not be migrated or freed while the caller holds the page table. The descriptors are not `struct page` pointers; they are `nvidia_p2p_page_t` structures containing physical addresses and page sizes.

To make them useful to the kernel's DMA and RDMA infrastructure, the SG table had to be built manually with a technique that looks wrong until you understand why it is right -- `sg_set_page` with a NULL page pointer:

```c
for_each_sg(sgt->sgl, sg, pt->entries, i) {
    sg_dma_address(sg) = pt->pages[i]->physical_address;
    sg_dma_len(sg) = DMAPLANE_GPU_PAGE_SIZE;
}
```

Normal SG tables carry a `struct page` reference so that `dma_map_sg` can look up the physical address and program the IOMMU. For GPU BAR pages, there is no `struct page`. The DMA address is set directly from the BAR1 physical address, bypassing IOMMU mapping entirely. This works because BAR addresses are already physical addresses visible to any PCIe device on the bus. On rxe (software RDMA), there is no hardware IOMMU to program anyway. On real hardware with ConnectX, `nvidia-peermem` handles the IOMMU mapping through a different path.

---

## Write-Combining: The 230x Speedup

Once the GPU BAR pages were pinned, I needed to map them into kernel virtual address space for CPU access. The obvious approach was `ioremap`, which creates an uncacheable (UC) mapping. Every CPU store becomes a single PCIe write TLP. Every CPU load becomes a PCIe read round-trip with 500ns-1us latency.

The uncacheable numbers were sobering:

```
UC BAR memcpy:  Host->GPU  44 MB/s    GPU->Host  6 MB/s
cudaMemcpy:     Host->GPU  12 GB/s    GPU->Host  13 GB/s
```

44 MB/s for host-to-GPU when CUDA achieves 12 GB/s on the same hardware. The CPU was issuing individual 8-byte stores over PCIe, each one waiting for the bus to accept the previous one before sending the next.

The fix was one function call: replacing `ioremap` with `ioremap_wc`. Write-combining tells the CPU's memory controller to buffer stores in a write-combining buffer and flush them as 64-byte cache-line-sized TLPs. Instead of 8 bytes per PCIe transaction, you get 64 bytes.

```c
gpu_buf->bar_pages[i] = ioremap_wc(
    gpu_buf->page_table->pages[i]->physical_address,
    DMAPLANE_GPU_PAGE_SIZE);
```

The results:

```
WC BAR memcpy:  Host->GPU  10,097 MB/s    GPU->Host  107 MB/s
cudaMemcpy:     Host->GPU  12,552 MB/s    GPU->Host  13,124 MB/s
```

Host-to-GPU went from 44 MB/s to 10 GB/s -- a 230x improvement. The WC mapping closed the gap with `cudaMemcpy` to within 20%. For writes, the CPU's write-combining buffer is nearly as efficient as the GPU's own DMA engine.

GPU-to-host improved from 6 MB/s to 107 MB/s -- better, but still 120x slower than `cudaMemcpy`. This asymmetry is fundamental to PCIe. Writes are *posted* -- the CPU fires them and continues without waiting for acknowledgment. Reads are *non-posted* -- the CPU sends a read request, waits for the data to come back over the bus (500ns-1us round trip), and only then can issue the next read. Write-combining helps writes dramatically but cannot fix the read latency problem. Only a hardware DMA engine -- either the GPU's copy engine or the NIC's RDMA engine -- can achieve full bandwidth for reads, because hardware DMA engines pipeline dozens of outstanding read requests concurrently.

This is the real reason GPUDirect RDMA exists. It replaces CPU-issued PCIe reads (107 MB/s) with NIC-initiated DMA reads (12-24 GB/s on ConnectX-6). The NIC's DMA engine reads GPU BAR pages at full PCIe bandwidth because it can have dozens of read requests in flight simultaneously. The CPU, constrained by its load-store architecture, can have at most a handful.

---

## The SG Table With NULL Pages

The SG table construction deserves closer examination because it reveals a pattern that every GPUDirect RDMA driver uses. In `driver/gpu_p2p.c`, the `gpu_bar_to_sgtable()` function builds the scatter-gather list:

```c
static struct sg_table *gpu_bar_to_sgtable(nvidia_p2p_page_table_t *pt)
{
    struct sg_table *sgt;
    struct scatterlist *sg;
    unsigned int i;

    sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
    if (!sgt)
        return ERR_PTR(-ENOMEM);

    if (sg_alloc_table(sgt, pt->entries, GFP_KERNEL)) {
        kfree(sgt);
        return ERR_PTR(-ENOMEM);
    }

    for_each_sg(sgt->sgl, sg, pt->entries, i) {
        sg_dma_address(sg) = pt->pages[i]->physical_address;
        sg_dma_len(sg) = DMAPLANE_GPU_PAGE_SIZE;
    }

    return sgt;
}
```

Each SG entry's page pointer is left as NULL (from `kzalloc`). The DMA address and length are set directly. This SG table cannot be passed to `dma_map_sg` (which would dereference the page pointer and crash). It can be passed to `ib_map_mr_sg` on hardware that understands BAR addresses, or -- as dmaplane does -- used with `local_dma_lkey` where the RDMA subsystem only needs the DMA address, not the page.

---

## The Unpin Callback Contract

When `cudaFree` is called on a buffer that dmaplane has pinned, NVIDIA's driver invokes the `free_callback` registered during `nvidia_p2p_get_pages`. This callback runs with NVIDIA's internal locks held, creating a set of constraints that are the number one source of deadlocks in third-party GPUDirect RDMA drivers:

```c
static void dmaplane_gpu_unpin_callback(void *data)
{
    struct dmaplane_gpu_buffer *gpu_buf = data;

    atomic_set(&gpu_buf->gpu_revoked, 1);
    complete_all(&gpu_buf->revoke_done);
}
```

The callback must not call `nvidia_p2p_put_pages` (deadlock -- it acquires the same lock the callback already holds). It must not take any mutex, spinlock, or rwsem. It must not call `printk` (which can attempt memory allocation under pressure). It sets an atomic flag and returns.

Subsequent RDMA operations check the flag and fail with `-ENODEV`. Actual cleanup -- releasing the page table, unmapping the BAR -- happens in `dmaplane_gpu_unpin()`, which chooses a different cleanup path depending on revocation state:

```c
if (!atomic_read(&gpu_buf->gpu_revoked)) {
    nv_put_pages(0, 0, gpu_buf->gpu_va, gpu_buf->page_table);
} else {
    nv_free_page_table(gpu_buf->page_table);
}
```

Normal path: `nvidia_p2p_put_pages()` releases the pin and frees the page table. Revoked path: `nvidia_p2p_free_page_table()` frees just the struct, because NVIDIA already reclaimed the pages and `put_pages` would deadlock. This is the same pattern that `nvidia-peermem` uses.

---

## BAR Throughput Benchmark

The Phase 8 test suite (`tests/test_phase8_gpu.c`) includes a benchmark that measures WC BAR throughput across five transfer sizes, compared side-by-side with `cudaMemcpy` on the same hardware (RTX 5000 Ada, PCIe Gen4 x16):

```
Test 4: BAR Throughput Benchmark (write-combining)
  Size       H->G MB/s    G->H MB/s    cudaH2D MB/s   cudaD2H MB/s
  64KB       5327         97           3384           3178
  256KB      8510         105          9757           10714
  1MB        9897         106          12123          12832
  4MB        10097        107          12425          13021
  16MB       10064        107          12552          13124
```

The pattern is clear. WC writes scale up quickly and plateau near 10 GB/s -- within 20% of `cudaMemcpy`'s DMA engine. WC reads plateau at 107 MB/s regardless of transfer size. The GPU's DMA engine (used by `cudaMemcpy`) achieves symmetric 12-13 GB/s in both directions because it pipelines dozens of outstanding PCIe read requests.

---

## GPU RDMA Loopback

With WC BAR mappings and the SG table working, the next step was registering GPU memory as an RDMA Memory Region. The GPU MR registration in `driver/gpu_p2p.c` uses the contiguous WC mapping as `sge.addr`:

```c
mr_entry->lkey = ctx->pd->local_dma_lkey;
mr_entry->sge_addr = (u64)(uintptr_t)gpu_buf->rdma_vaddr;
```

When rxe processes a send work request with `sge.addr = rdma_vaddr`, it does `memcpy(skb_data, rdma_vaddr, length)`. Each byte of that `memcpy` becomes a CPU load from the WC BAR mapping, which becomes a PCIe read from GPU VRAM. The data flows from GPU memory into the network packet buffer without existing in host DRAM.

The contiguity check is critical. `nvidia_p2p_get_pages` returns an array of 64KB page descriptors. For a single `cudaMalloc`, these pages are almost always physically contiguous in BAR space. `dmaplane_gpu_pin()` verifies this:

```c
bool contiguous = true;
for (i = 1; i < gpu_buf->num_pages; i++) {
    phys_addr_t expected =
        gpu_buf->page_table->pages[0]->physical_address
        + (u64)i * DMAPLANE_GPU_PAGE_SIZE;
    if (gpu_buf->page_table->pages[i]->physical_address != expected) {
        contiguous = false;
        break;
    }
}
if (contiguous)
    gpu_buf->rdma_vaddr = ioremap_wc(
        gpu_buf->page_table->pages[0]->physical_address,
        (size_t)gpu_buf->num_pages * DMAPLANE_GPU_PAGE_SIZE);
```

If contiguous, a single `ioremap_wc` spanning the entire buffer gives rxe one contiguous VA range to `memcpy` from. If non-contiguous, `rdma_vaddr` is NULL and GPU RDMA MR registration fails -- pin/unpin and DMA still work, only RDMA is unavailable.

The loopback test confirmed the full path:

```
Test 7: GPU RDMA Loopback (rxe, 64 KB)
  GPU MR: id=0 lkey=0x460a1
  Host MR: id=1 lkey=0x460a1
  Loopback: latency=3236346 ns, recv=65536 bytes
  Verification: 65536/65536 bytes match    PASS
```

3.2ms for 64KB reflects rxe's software overhead plus the per-byte PCIe read latency from the WC BAR mapping. On hardware with ConnectX, this same path achieves 12-24 GB/s because the NIC's DMA engine reads the BAR directly.

---

## Cross-Machine: GPU VRAM Arrives Where There Is No GPU

The loopback test proved that rxe could read from the WC BAR mapping. But loopback means both QPs on the same machine -- the data never touches the network. The cross-machine demo in `examples/gpu_rdma/` uses two machines: Machine A with an RTX GPU sending VRAM contents over Ethernet to Machine B with no GPU.

The cross-machine QP connection follows a three-step ioctl dance. Each side creates a peer QP via `DMAPLANE_IOCTL_RDMA_INIT_PEER`, which returns local metadata (QP number, GID, MAC address). The metadata is exchanged over a TCP socket. Each side then calls `DMAPLANE_IOCTL_RDMA_CONNECT_PEER` with the remote metadata, which transitions the peer QP through INIT, RTR, and RTS:

```c
/* Machine A fills its local metadata via ioctl */
ioctl(fd, DMAPLANE_IOCTL_RDMA_INIT_PEER, &local_info);
/* -> kernel creates qp_peer, transitions to INIT, returns qp_num + GID + MAC */

/* Exchange over TCP */
send(tcp_fd, &local_info, ...);
recv(tcp_fd, &remote_info, ...);

/* Machine A tells the kernel about the remote machine */
ioctl(fd, DMAPLANE_IOCTL_RDMA_CONNECT_PEER, &remote_info);
/* -> kernel transitions qp_peer: INIT -> RTR -> RTS using remote QP/GID/MAC */
```

The destination MAC address is the subtle detail that makes RoCEv2 work over Ethernet. When `rdma_cm` handles connections in userspace, it resolves the DMAC automatically through ARP. With raw kernel verbs, you must populate `ah_attr.roce.dmac` yourself. Each side includes its own MAC in the TCP metadata exchange so the remote side can build the correct Address Handle.

On Machine A, the sender fills 1 MB of GPU VRAM with a deterministic gradient pattern, pins it, registers it as an RDMA MR, and sends it through the kernel peer QP. On Machine B, the receiver allocates a 1 MB host buffer, registers it as an MR, and posts a receive on its peer QP:

Machine A:
```
=== dmaplane: GPU VRAM -> Network (Direct Send) ===
    Target: 192.168.50.17:9876 | Buffer: 1024 KB

[3]  DMAPLANE_IOCTL_GPU_PIN: handle=1, pages=16, numa=-1
[5]  DMAPLANE_IOCTL_GPU_REGISTER_MR: mr_id=1, lkey=0x46370
[10] DMAPLANE_IOCTL_RDMA_REMOTE_SEND (1024 KB from GPU MR 1)...
     Sent 1048576 bytes in 90688589 ns
     Throughput: 11.6 MB/s

  Path: GPU VRAM -> WC BAR -> kernel QP -> rxe -> Ethernet
  No host staging copy.
```

Machine B:
```
=== dmaplane: GPU VRAM -> Network (Direct Receive) ===

[8]  DMAPLANE_IOCTL_RDMA_REMOTE_RECV (1024 KB into MR 1)...
     Received 1048576 bytes in 290838241 ns
[10] Verifying gradient data (layer 42)...
     First 4: [-0.021314, -0.021324, -0.021333, -0.021342]
     PASSED -- all sampled values match.

  Path: Ethernet -> rxe -> kernel QP -> host DRAM -> mmap -> verify
  No GPU required on receiver.
```

PASSED. Every byte that was in GPU VRAM on Machine A arrived correct on Machine B. The data traveled: GPU VRAM, PCIe BAR read, rxe memcpy into UDP packets, real Ethernet, Machine B's rxe, host DRAM, userspace verification. No host staging buffer on the sender. No GPU on the receiver.

The 11.6 MB/s throughput is bounded by two factors in series: the GPU-to-host BAR read at ~107 MB/s (rxe must read the data from GPU memory into packet buffers) and the 1 GbE Ethernet link at ~120 MB/s theoretical. On ConnectX hardware with `nvidia-peermem`, the NIC bypasses the CPU entirely -- its DMA engine reads the GPU BAR at full PCIe bandwidth (12-24 GB/s), and a 100 GbE link becomes the bottleneck.

---

## The Performance Hierarchy

Over the course of this phase, a clear hierarchy emerged showing how each tier of hardware access improves bandwidth:

```
                          Host->GPU        GPU->Host
UC BAR (ioremap):            44 MB/s          6 MB/s
WC BAR (ioremap_wc):     10,097 MB/s        107 MB/s
cudaMemcpy:              12,552 MB/s     13,124 MB/s
GPUDirect RDMA (HW):     12-24 GB/s      12-24 GB/s
```

Each tier uses a smarter DMA strategy. UC mapping sends individual 8-byte TLPs over PCIe. WC mapping coalesces stores into 64-byte TLPs. `cudaMemcpy` uses the GPU's DMA engine with dozens of outstanding requests. GPUDirect RDMA uses the NIC's DMA engine with the same pipelining. The asymmetry between writes (posted) and reads (non-posted) persists at every tier -- only hardware DMA engines can close the gap.

Understanding this hierarchy is understanding why GPUDirect RDMA exists. It is not a performance optimization. It is a fundamental architectural requirement. CPU-mediated GPU memory access tops out at 107 MB/s for reads. The NIC's DMA engine achieves 100x that. For disaggregated inference transferring gigabytes of KV-cache per second, the CPU path is not just slow -- it is unusable.

---

## Connection to Production

The `nvidia_p2p_get_pages` API and the `free_callback` mechanism are the same ones that `nvidia-peermem` uses internally. The SG table construction with manually-set DMA addresses is how every GPUDirect RDMA third-party driver works around the lack of `struct page` entries for BAR memory. The contiguous `ioremap_wc` mapping trick is specific to rxe -- hardware NICs do not need a kernel VA, they DMA from physical addresses -- but it reveals the same underlying constraint: someone has to build a contiguous address mapping for the RDMA subsystem, whether it is a kernel VA for software or an IOMMU mapping for hardware.

The two-machine demo implements the exact data path that Splitwise, DistServe, and Mooncake's TransferEngine use for disaggregated KV-cache transfer. A prefill GPU computes the KV-cache, a NIC reads it directly from GPU VRAM via RDMA, and a decode machine receives it. The differences from production are the transport (rxe vs. ConnectX), the link speed (1 GbE vs. 400 GbE), and the NIC's ability to bypass the CPU for BAR reads. The kernel primitives are identical.

The entire demo runs on commodity hardware. A laptop with an RTX GPU, a NUC with an Ethernet port, a kernel module, and Soft-RoCE. The production path replaces rxe with ConnectX and 1 GbE with 400 GbE. The architecture stays the same.

---

*The code -- character device, DMA allocator, dma-buf exporter, RDMA engine, GPU P2P integration, and cross-machine demo -- is built as loadable kernel modules against a stock Ubuntu kernel. `nvidia_p2p_get_pages` resolves at runtime via `symbol_get`, so the module compiles and loads whether or not the NVIDIA driver is present. The GPU features activate when the symbols are available and degrade gracefully when they are not.*

*Next: [Part 9 -- Disaggregated Inference Demo](/docs/blog_09_disaggregated_inference.md)*
