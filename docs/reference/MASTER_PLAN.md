# dmaplane — Master Engineering Plan

## Project

Host-Side Data Path for AI-Scale Systems

## Purpose

Modern AI systems move gradients, KV-caches, and model weights across machines at hundreds of gigabits per second. dmaplane is a hands-on project for learning the kernel subsystems that make those transfers possible — DMA allocation, device memory sharing, RDMA, NUMA topology, and GPU memory pinning — by rebuilding them from scratch as a Linux kernel module.

The project walks through nine phases, each targeting a specific piece of the plumbing:

1. How to write a kernel driver with correct locking and concurrency
2. How memory is allocated for DMA — and why the choice between coherent and scatter-gather matters
3. How NUMA topology silently costs you 40–50% throughput if you ignore it
4. How RDMA actually works at the kernel verbs level
5. How devices share memory without copies (dma-buf)
6. How flow control prevents the sender from overwhelming the receiver
7. How to instrument the kernel to see where latency comes from
8. How GPU VRAM gets pinned and exposed to the NIC for direct access
9. How all of it comes together in a two-machine demo where a prompt goes in on one machine and tokens come out on the other

It's not production software. It runs on soft-RoCE over commodity Ethernet — no InfiniBand, no ConnectX card required. The throughput is orders of magnitude slower than real hardware. The point isn't speed. The point is that every API call, every ioctl, every data path decision is something you wrote and can explain. When you later look at how Mooncake, TransferEngine, or `nvidia-peermem` work, you recognize the same primitives because you've already built them yourself.

## Use Cases

All four use cases require point-to-point RDMA and share the same infrastructure requirements that dmaplane addresses: NUMA-correct allocation, zero-copy device sharing, flow control to prevent CQ overflow, and per-transfer completion tracking.

1. **Disaggregated inference — KV-cache transfer.** Prefill GPU computes the KV-cache, sends it over RDMA to a separate decode GPU. This is the Splitwise/DistServe/Mooncake pattern.

2. **MoE dispatch/combine — variable-sized expert routing.** Each GPU sends different numbers of tokens to different experts every forward pass. Collectives can't handle the imbalanced loads; point-to-point with per-expert backpressure can. This repeats twice per layer (dispatch and combine).

3. **RL post-training — one-sided weight updates.** A trainer broadcasts weight deltas to dozens of inference GPUs via fire-and-forget RDMA writes. No barrier, no synchronization. Inference GPUs apply deltas asynchronously while continuing to serve requests.

4. **Host-side data path for non-GPU accelerators.** The kernel plumbing that feeds weights and activations to custom AI hardware (Cerebras, Groq, custom ASICs) over PCIe uses the same DMA allocation, NUMA placement, and dma-buf sharing primitives that dmaplane teaches, regardless of what sits on the other end of the link.

The project is GPU-flavored because it uses `nvidia_p2p_get_pages`, but Phases 1–5 are entirely GPU-agnostic. That's the part that transfers directly to any accelerator's host stack.

---

## Hardware

### Development (laptop — unlimited time, no cost)

| Component | Value |
|-----------|-------|
| GPU | NVIDIA RTX 5000 Ada Generation (Laptop), 16 GB GDDR6, 16 GB BAR1 |
| RDMA | Soft-RoCE (rxe) over Ethernet |
| NUMA | Single node (0), 32 CPUs, 128 GB DRAM |
| Kernel | 6.5.0-1024-oem (stock Ubuntu) |
| Role | All kernel development, loopback testing, GPU integration |

### Second machine (NUC or similar — for two-machine RDMA tests)

| Component | Value |
|-----------|-------|
| GPU | Not required |
| RDMA | Soft-RoCE (rxe) over Ethernet |
| Role | RDMA receiver for cross-machine demos (Phases 4, 8, 9) |

### Cloud (EC2 — provisioned on demand, pay-per-hour)

| Instance | GPU | Use | Phase |
|----------|-----|-----|-------|
| m5.metal or c5.metal (×1) | None | Multi-socket NUMA validation (2 nodes) | 3 |
| g5.xlarge (×2) | A10G, 24 GB | Disaggregated inference demo | 9B |
| p4d.24xlarge (×1, optional) | A100 (×8), ConnectX-6 | Production throughput validation | 8/9 |

EC2 instances are used only for validation after code is debugged locally. Estimated budget: $15–30 total.

---

## Repository Structure

The repo starts as a skeleton. Each phase populates its directories. By end of Phase 9, every directory has real content.

```
dmaplane/
├── README.md                       # Project overview, phase index, build instructions
├── LICENSE                         # GPL-2.0
├── Makefile                        # Top-level: delegates to driver/Makefile
│
├── driver/                         # Kernel module source (single dmaplane.ko)
│   ├── README.md                   # Driver architecture, ioctl reference
│   ├── Makefile                    # Kbuild Makefile
│   ├── dmaplane.h                  # Shared header: ioctls, structs, constants
│   ├── main.c                      # Character device, ioctl dispatch, module init/exit
│   ├── dmabuf_rdma.c               # DMA buffer allocation (coherent, SG, user-pinned)
│   ├── numa_topology.c             # NUMA topology query, cross-node benchmarks
│   ├── numa_topology.h             # NUMA API declarations
│   ├── rdma_engine.c               # Kernel-space RDMA: PD, CQ, QP, MR, verbs
│   ├── dmabuf_export.c             # dma-buf exporter: ops vtable, lifecycle
│   ├── backpressure.c              # Credit-based flow control, watermarks
│   ├── instrument.c                # Tracepoints, latency histograms, debugfs
│   └── gpu_p2p.c                   # GPU BAR pinning, WC mapping, GPU-backed MRs
│
├── include/                        # Userspace-visible headers
│   └── dmaplane_uapi.h             # Ioctl numbers, shared structs (kernel ↔ user)
│
├── tests/                          # One test per phase, cumulative
│   ├── README.md                   # Test inventory, how to run
│   ├── test_phase1_driver.c        # Stress: 10M submissions, lock contention
│   ├── test_phase2_dma.c           # Allocation benchmarks, coherent vs SG vs hugepage
│   ├── test_phase3_numa.c          # Topology display, NUMA-pinned alloc, bandwidth matrix
│   ├── test_phase4_rdma.c          # Loopback ping-pong, streaming throughput
│   ├── test_phase5_dmabuf.c        # Multi-attach churn, mmap verify, RDMA from dma-buf MR
│   ├── test_phase6_backpressure.c  # Credit exhaustion, CQ overflow prevention, 10-min soak
│   ├── test_phase7_instrument.c    # Histogram validation, tracepoint verification
│   ├── test_phase8_gpu.c           # GPU pin/unpin, BAR throughput, GPU-backed RDMA loopback
│   └── test_phase9_writeimm.c      # WRITEIMM loopback, chunked KV-cache simulation
│
├── examples/                       # Progressively complex demos
│   ├── README.md                   # Example index, prerequisites
│   ├── misc/                       # Simple standalone demos (allocation, NUMA, profiling)
│   │   └── README.md
│   ├── streamer/                   # Interactive weight-streaming TUI (ncurses)
│   │   └── README.md
│   ├── netshare/                   # Two-machine RDMA demos
│   │   ├── README.md
│   │   ├── sender.c                # Host-path RDMA sender (rdma_cm)
│   │   ├── receiver.c              # Host-path RDMA receiver
│   │   ├── gpu_sender.c            # GPU VRAM → network sender
│   │   └── gpu_receiver.c          # Network → host receiver (no GPU needed)
│   └── inference/                  # Phase 9: disaggregated inference pipeline
│       ├── README.md
│       ├── kvcache_proto.h         # WRITEIMM chunk encoding, manifest struct
│       ├── kvcache_sender.c        # Chunked WRITEIMM sender with tracing
│       ├── kvcache_receiver.c      # Chunked receiver with bitmap tracking
│       ├── prefill_server.py       # PyTorch prefill + dmaplane transfer
│       ├── decode_server.py        # PyTorch decode with zero-copy reconstruction
│       └── dmaplane_py.py          # Python ioctl wrapper (struct.pack, fcntl)
│
├── docs/                           # Blog posts and technical notes
│   ├── blog_01_driver_foundations.md
│   ├── blog_02_dma_memory.md
│   ├── blog_03_numa_topology.md
│   ├── blog_04_rdma_engine.md
│   ├── blog_05_dmabuf_zero_copy.md
│   ├── blog_06_backpressure.md
│   ├── blog_07_instrumentation.md
│   ├── blog_08_gpu_integration.md
│   └── blog_09_disaggregated_inference.md
│
└── scripts/                        # Build, setup, and benchmark automation
    ├── setup_rxe.sh                # Configure soft-RoCE on Ethernet interface
    ├── setup_hugepages.sh          # Reserve hugepages for DMA benchmarks
    └── ec2_setup.sh                # EC2 instance provisioning and AMI creation
```

---

## Phase Dependency Chain

Each phase adds files to the repo. No phase replaces previous work.

```
Phase 1: main.c, dmaplane.h                       (driver skeleton)
    │
Phase 2: dmabuf_rdma.c                             (adds allocation to skeleton)
    │
Phase 3: numa_topology.c/h, extends dmabuf_rdma.c  (adds NUMA-aware allocation)
    │
Phase 4: rdma_engine.c, extends main.c             (adds RDMA transport)
    │
Phase 5: dmabuf_export.c, extends rdma_engine.c    (adds dma-buf sharing + MR from dma-buf)
    │
Phase 6: backpressure.c, extends rdma_engine.c     (adds flow control)
    │
Phase 7: instrument.c, extends main.c              (adds tracepoints, histograms, debugfs)
    │
Phase 8: gpu_p2p.c, extends dmabuf_export.c        (adds GPU BAR pinning + GPU dma-buf)
    │
Phase 9: examples/inference/*, extends all          (end-to-end demo)
```

---

## Phase 1 — Driver Foundations & Concurrency

### What You Build

The character device (`/dev/dmaplane`) that everything else hangs on. Ioctl dispatch, per-file context, submission/completion rings, per-channel worker threads with spinlock and lock-free SPSC paths.

### Files Added

| File | Content |
|------|---------|
| `driver/main.c` | Character device registration, ioctl dispatch, module init/exit |
| `driver/dmaplane.h` | Ioctl numbers, core structs, ring buffer definitions |
| `driver/Makefile` | Kbuild makefile |
| `include/dmaplane_uapi.h` | Userspace-visible ioctl definitions |
| `tests/test_phase1_driver.c` | Stress test: ≥10M submissions, lock contention |
| `Makefile` | Top-level build |

### Acceptance Criteria

- Module loads/unloads cleanly with no `dmesg` warnings
- Survives stress test of ≥10M submissions across multiple channels
- No lockdep violations under contention
- No WARN_ON under contention
- Clean `insmod`/`rmmod` cycles

### Skill KPI

You can explain every lock in the system and why it exists. You can remove at least one lock and justify correctness without it.

### Blog Post

`docs/blog_01_driver_foundations.md` — Character devices, kernel object lifetimes, locking models, ring buffer design, race detection with lockdep.

---

## Phase 2 — DMA Memory Allocation

### What You Build

Three allocation paths behind a single ioctl: coherent (`dma_alloc_coherent`) for small hot control structures, scatter-gather (`alloc_pages` + `dma_map_sg`) for large streaming buffers, and user-pinned (`get_user_pages` + DMA mapping) for zero-copy from userspace. Per-buffer stats tracking. Hugepage support with measured SG table construction speedup.

### Files Added

| File | Content |
|------|---------|
| `driver/dmabuf_rdma.c` | All three allocation paths, SG table construction, hugepage support |
| `tests/test_phase2_dma.c` | Allocation benchmarks: coherent vs SG, 4K vs 2M pages, throughput sweeps |
| `examples/misc/` | Simple demos: explorer, sweep, verify |

### What Changes

| File | Change |
|------|--------|
| `driver/dmaplane.h` | Add buffer parameter structs, allocation type enums |
| `driver/main.c` | Wire `IOCTL_ALLOC_BUFFER`, `IOCTL_FREE_BUFFER`, `IOCTL_GET_STATS` |

### Acceptance Criteria

- Measurable performance delta between coherent and SG paths
- Hugepage SG mapping ≥10× faster than 4K-page mapping
- Per-buffer stats accurately track allocation counts, sizes, types
- No memory leaks across 1000 alloc/free cycles

### Skill KPI

You can predict the performance impact of an allocation strategy before measuring it. You understand TLB pressure in practical terms — you can explain why a 16 MB buffer with 4K pages thrashes the TLB while 2M pages don't.

### Blog Post

`docs/blog_02_dma_memory.md` — Coherent vs streaming DMA, scatter-gather tables, IOMMU behavior, hugepage transformation, allocation strategy for AI workloads.

---

## Phase 3 — NUMA Topology & Placement

### What You Build

NUMA-aware allocation with `alloc_pages_node()` and placement verification via `page_to_nid()`. Topology query ioctl exposing node count, CPUs per node, memory per node, and ACPI SLIT distance matrix. Cross-node bandwidth benchmark using pinned kthreads.

### Why Phase 3 (Not Phase 5)

NUMA placement is an allocation concern, not a late optimization. Every buffer allocated in Phase 2 lands somewhere — if you don't control where, you silently lose 40–50% throughput on multi-socket systems. Making NUMA explicit early means every subsequent phase (RDMA, dma-buf, GPU) allocates correctly from the start.

On the single-socket development laptop, the ioctls work and the code paths are exercised, but the penalty is zero — every allocation is "local." After validating correctness locally, provision a multi-socket EC2 instance (e.g., m5.metal or c5.metal with 2 NUMA nodes) specifically to run the N×N bandwidth matrix and measure real cross-node penalties. This is a short session (1–2 hours) to collect the numbers that make NUMA concrete.

### Files Added

| File | Content |
|------|---------|
| `driver/numa_topology.c` | Topology enumeration, cross-node bandwidth benchmark with kthread pinning |
| `driver/numa_topology.h` | NUMA API declarations |
| `tests/test_phase3_numa.c` | Topology display, NUMA-pinned allocation, N×N bandwidth matrix |

### What Changes

| File | Change |
|------|--------|
| `driver/dmaplane.h` | Add `numa_node` / `actual_numa_node` to buffer params, new ioctls |
| `driver/dmabuf_rdma.c` | `alloc_page()` → `alloc_pages_node()`, placement verification, fallback tracking |
| `driver/main.c` | Wire `IOCTL_QUERY_NUMA_TOPO`, `IOCTL_NUMA_BENCH` |
| `driver/Makefile` | Add `numa_topology.o` |

### Acceptance Criteria

**Local (laptop):**
- Buffers allocated on requested NUMA node (verified by `page_to_nid`)
- Fallback under memory pressure tracked in stats
- Topology query returns correct node count, distances, memory
- Coherent buffers correctly report that NUMA steering is not possible
- All ioctls exercised and passing on single-socket system

**EC2 (multi-socket instance, 1–2 hour session):**
- N×N bandwidth matrix shows measurable local vs remote penalty
- Cross-node penalty quantified (expected 30–50% throughput loss)
- Allocation placement verified across both NUMA nodes

### Skill KPI

You can look at `lstopo` output and predict which device-to-memory paths are fast and which pay the interconnect penalty. You can explain why `alloc_pages_node()` is a preference, not a guarantee.

### Blog Post

`docs/blog_03_numa_topology.md` — NUMA architecture, memory placement, the invisible throughput penalty, distance matrices, why NCCL does topology detection.

---

## Phase 4 — RDMA Engine

### What You Build

A kernel-space RDMA engine using InfiniBand verbs. Protection Domain, Completion Queues (`IB_POLL_DIRECT`), Queue Pairs with full lifecycle (RESET → INIT → RTR → RTS → ERROR → teardown). GID selection for RoCEv2 with stable-privacy IPv6 addressing. Memory Region registration from Phase 2 buffers. Loopback ping-pong and streaming throughput tests over rxe. Two-machine RDMA between laptop and NUC.

### Files Added

| File | Content |
|------|---------|
| `driver/rdma_engine.c` | PD, CQ, QP setup, MR registration, send/recv/poll, connection management, teardown |
| `tests/test_phase4_rdma.c` | Loopback: ping-pong latency, streaming throughput, MR lifecycle stress |
| `examples/netshare/sender.c` | Two-machine RDMA sender (host-backed MRs) |
| `examples/netshare/receiver.c` | Two-machine RDMA receiver |
| `scripts/setup_rxe.sh` | Soft-RoCE configuration |

### What Changes

| File | Change |
|------|--------|
| `driver/dmaplane.h` | RDMA ioctls, QP/MR parameter structs |
| `driver/main.c` | Wire RDMA ioctls: connect, send, recv, poll, register MR |
| `driver/Makefile` | Add `rdma_engine.o` |

### Acceptance Criteria

- Loopback: ≥400 completions, zero errors, both directions
- GID selection handles stable-privacy addresses (not hardcoded index 0)
- Per-slot CQE tracking (not just "something completed")
- Clean QP teardown with error-state transition
- Two-machine test: data arrives byte-for-byte correct over real Ethernet

### Skill KPI

You can explain why GID index 0 silently drops all packets on modern kernels. You can describe the QP state machine transitions and what happens if you skip one. You can explain why MR registration is expensive and why NCCL pre-registers MR pools.

### Blog Post

`docs/blog_04_rdma_engine.md` — RDMA verbs from kernel space, GID selection, QP lifecycle, CQ management, silent failure modes, loopback validation, cross-machine demo.

---

## Phase 5 — dma-buf & Zero-Copy Sharing

### What You Build

A dma-buf exporter for host-memory buffers. Full `dma_buf_ops` vtable: attach, detach, `map_dma_buf` (builds per-device SG table through importer's IOMMU), `unmap_dma_buf`, vmap, release, `begin_cpu_access`, `end_cpu_access`. Refcounting with leak detection. Userspace mmap of dma-buf file descriptors. Integration with the RDMA engine: register dma-buf-backed pages as RDMA Memory Regions, prove zero-copy RDMA from shared buffers.

### Why Phase 5 (Not Phase 3)

dma-buf is the cross-device sharing primitive. It's needed when two different devices (GPU + NIC, or NIC + CPU) access the same buffer. Basic RDMA (Phase 4) works fine with directly-allocated MRs. dma-buf becomes essential in Phase 8 when GPU BAR pages need to be shared with the NIC. Placing it here means it's fresh when GPU integration arrives.

### Files Added

| File | Content |
|------|---------|
| `driver/dmabuf_export.c` | dma-buf ops vtable, exporter implementation, refcount tracking |
| `tests/test_phase5_dmabuf.c` | Multi-attach churn, mmap verify, RDMA from dma-buf-backed MR |

### What Changes

| File | Change |
|------|--------|
| `driver/dmaplane.h` | dma-buf export ioctl, attachment tracking structs |
| `driver/main.c` | Wire `IOCTL_DMABUF_EXPORT`, `IOCTL_DMABUF_IMPORT` |
| `driver/rdma_engine.c` | MR registration path accepts dma-buf-backed pages |
| `driver/Makefile` | Add `dmabuf_export.o` |

### Acceptance Criteria

- dma-buf export produces valid file descriptor
- Multi-device attach/detach cycles (1000×) with zero leaks
- Per-device SG tables have correct IOMMU-mapped DMA addresses
- Userspace mmap reads/writes data visible to DMA
- RDMA send from dma-buf-backed MR succeeds on loopback
- `attach_count > 0` warning fires on improper release

### Skill KPI

You can draw the full memory ownership graph: who allocated the pages, who holds references, what happens if an importer crashes without detaching. You can explain why the same physical pages appear at different DMA addresses for different devices.

### Blog Post

`docs/blog_05_dmabuf_zero_copy.md` — The dma-buf contract, exporter/importer lifecycle, per-device IOMMU mappings, refcounting discipline, mmap integration, bridge to RDMA, foundation for GPUDirect RDMA.

---

## Phase 6 — Backpressure & Flow Control

### What You Build

Credit-based flow control for the RDMA engine. Configurable credit window (number of pre-posted receive WRs). High/low watermark enforcement. CQ moderation tuning (batching completion notifications). Sustained streaming validation: ≥10 minutes of continuous transfer with zero descriptor overruns and stable throughput. Streaming throughput matrix across message sizes and queue depths.

### Files Added

| File | Content |
|------|---------|
| `driver/backpressure.c` | Credit accounting, watermark enforcement, CQ moderation |
| `tests/test_phase6_backpressure.c` | Credit exhaustion, CQ overflow prevention, 10-minute soak test |
| `examples/streamer/` | Interactive ncurses TUI showing streaming throughput in real time |

### What Changes

| File | Change |
|------|--------|
| `driver/dmaplane.h` | Backpressure configuration structs, credit window ioctls |
| `driver/rdma_engine.c` | Integrate credit checks into send path, sliding-window recv |
| `driver/main.c` | Wire backpressure configuration ioctls |
| `driver/Makefile` | Add `backpressure.o` |

### Acceptance Criteria

- Sender stalls when credits exhausted (not CQ overflow)
- Receiver drains CQ and replenishes credits correctly
- 10-minute sustained streaming: zero overruns, throughput within 5% of steady state
- CQ overflow intentionally triggered in test, then prevented by backpressure
- Throughput matrix: message size × queue depth, reported in MB/s

### Skill KPI

You can intentionally shape throughput curves by adjusting credit window and watermarks. You can explain why CQ overflow is silent and unrecoverable in most RDMA implementations, and what cascading failure it causes.

### Blog Post

`docs/blog_06_backpressure.md` — Queue theory applied to DMA, credit-based flow control, CQ overflow mechanics, watermark tuning, sustained streaming validation.

---

## Phase 7 — Instrumentation & Latency Measurement

### What You Build

Kernel tracepoints on the submit/complete path. Bucketed latency histograms (P50/P99/P999) exported via debugfs. Cacheline alignment of ring buffer head/tail indices (`____cacheline_aligned_in_smp`) with measured before/after impact. `perf stat` integration for TLB misses, cache misses, branch mispredictions. `ftrace` function-graph tracing of the submit path.

### Files Added

| File | Content |
|------|---------|
| `driver/instrument.c` | Tracepoint definitions, histogram collection, debugfs export |
| `tests/test_phase7_instrument.c` | Histogram validation, tracepoint firing verification |

### What Changes

| File | Change |
|------|--------|
| `driver/dmaplane.h` | Histogram structs, debugfs paths |
| `driver/main.c` | Wire debugfs entries, integrate tracepoints into submit/complete |
| `driver/Makefile` | Add `instrument.o` |

### Acceptance Criteria

- Tracepoints visible in `ftrace` output for every submit and complete
- Latency histograms reproducible across runs (P50 within 10% variance)
- Cacheline alignment produces measurable improvement under high-rate polling
- debugfs files readable from userspace with correct histogram data
- `perf stat` shows TLB miss reduction with hugepages (connecting back to Phase 2)

### Skill KPI

You can explain observed latency variance using cache behavior and scheduling jitter. You can connect `perf stat` counters to specific code paths and predict the effect of alignment changes.

### Blog Post

`docs/blog_07_instrumentation.md` — Kernel tracepoints, perf + ftrace, cacheline alignment, false sharing, latency histograms, connecting microarchitecture to measured performance.

---

## Phase 8 — GPU Memory Integration

### What You Build

GPU VRAM pinning via `nvidia_p2p_get_pages`. BAR mapping with write-combining (`ioremap_wc`). Host↔GPU DMA throughput benchmarks (UC vs WC vs cudaMemcpy). The unpin callback contract and safe handling under `cudaFree`. dma-buf export of GPU BAR pages (extending Phase 5's host-page exporter to handle 64KB GPU pages). RDMA MR registration from GPU-backed dma-buf. Two-machine demo: GPU VRAM on laptop arrives byte-for-byte verified on NUC over Ethernet.

### Conditional Compilation

GPU support compiles only when NVIDIA headers are present. Without them, GPU ioctls return `-ENODEV`. The module loads and works for all host-side functionality regardless.

```makefile
GPU_HEADERS := $(shell test -f /usr/src/nvidia-*/nvidia/nv-p2p.h && echo yes)
ifeq ($(GPU_HEADERS),yes)
  dmaplane-objs += gpu_p2p.o
  ccflags-y += -DCONFIG_DMAPLANE_GPU
endif
```

### Files Added

| File | Content |
|------|---------|
| `driver/gpu_p2p.c` | `nvidia_p2p_get_pages`, BAR mapping, WC/UC benchmarks, unpin callback, GPU dma-buf ops, GPU-backed MR registration |
| `tests/test_phase8_gpu.c` | Pin/unpin lifecycle, BAR throughput, data integrity roundtrip, unpin callback under cudaFree, GPU-backed RDMA loopback |
| `examples/netshare/gpu_sender.c` | GPU VRAM → network sender (kernel peer QP, no libibverbs) |
| `examples/netshare/gpu_receiver.c` | Network → host receiver (no GPU needed) |

### What Changes

| File | Change |
|------|--------|
| `driver/dmaplane.h` | GPU pin/unpin ioctls, GPU DMA params, GPU benchmark params |
| `driver/dmabuf_export.c` | Extended `map_dma_buf` for GPU BAR pages (64KB, not 4KB) |
| `driver/rdma_engine.c` | MR registration from GPU BAR SG tables |
| `driver/main.c` | Wire GPU ioctls, conditional compilation |
| `driver/Makefile` | Conditional `gpu_p2p.o` |

### Acceptance Criteria

- Pin/unpin GPU memory at sizes 64KB–256MB with zero leaks
- Data integrity: host→GPU→verify roundtrip passes
- BAR throughput measured: UC (~44 MB/s write, ~6 MB/s read), WC (~10 GB/s write, ~107 MB/s read)
- Unpin callback: `cudaFree` while pinned triggers callback, no crash, no deadlock
- GPU dma-buf export: valid fd, NIC can attach and get SG table
- GPU-backed RDMA MR: loopback send succeeds
- Two-machine: GPU VRAM bytes arrive verified on NUC over Ethernet
- Module loads cleanly without NVIDIA headers (GPU ioctls return `-ENODEV`)

### Skill KPI

You can explain the full chain: `cudaMalloc` → `nvidia_p2p_get_pages` → SG table → `dma_buf_export` → `ib_reg_mr` → RDMA SEND. You can describe the unpin callback contract and why violating it causes deadlocks. You know why BAR1 size matters and what the performance hierarchy (UC → WC → cudaMemcpy → GPUDirect RDMA) reveals about PCIe architecture.

### Blog Post

`docs/blog_08_gpu_integration.md` — GPU BAR mappings, write-combining, the performance hierarchy, nvidia_p2p_get_pages, unpin callback, GPUDirect RDMA data path, cross-machine demo.

---

## Phase 9 — Disaggregated Inference Demo

### What You Build

RDMA WRITE with Immediate (`IB_WR_RDMA_WRITE_WITH_IMM`) for per-chunk KV-cache tracking. The 32-bit immediate field encodes (layer, chunk) so the receiver knows what landed without polling memory. Credit-window flow control for chunked streaming. Bitmap-based completion tracking on the receiver. A two-machine demo on EC2 g5.xlarge instances: Machine A runs PyTorch prefill, dmaplane transfers KV-cache chunks over RDMA, Machine B runs PyTorch decode with zero-copy reconstruction via `torch.as_strided`. Real tokens come out. No host DRAM staging — KV-cache travels GPU VRAM → BAR → NIC → Ethernet → NIC → BAR → GPU VRAM.

### Sub-Phases

**9A — Local development and PyTorch integration (laptop, loopback)**

All kernel work, protocol development, and PyTorch integration happen locally before touching EC2.

- Add `IB_WR_RDMA_WRITE_WITH_IMM` to the RDMA engine.
- Loopback tests: single WRITEIMM, chunked streaming with sequence numbers, simulated KV-cache transfer (32 layers × 4 chunks), backpressure validation with credit exhaustion and recovery.
- Two-machine WRITEIMM test (laptop → NUC) to verify the protocol works over real Ethernet before EC2.
- PyTorch integration on loopback: run `prefill_server.py` and `decode_server.py` on the same laptop using rxe loopback. The prefill side runs `model.forward()` on the local GPU, consolidates KV-cache, and sends via WRITEIMM. The decode side receives, reconstructs via `torch.as_strided`, and generates tokens. This validates the entire Python→ioctl→kernel→RDMA→kernel→ioctl→Python pipeline, the KV-cache serialization format, and the zero-copy reconstruction — all without EC2 cost. Throughput will be limited by loopback, but correctness is the goal.

**9B — Two-machine demo on EC2 (2× g5.xlarge, ~6 hours)**

Everything is debugged. EC2 time is purely for the cross-machine demo with two GPUs.

- Build dmaplane on A10G, verify GPU pin/unpin and BAR throughput, create AMI snapshot.
- Run the C-level chunked KV-cache transfer between two instances: 128 chunks, per-chunk latency histograms, backpressure validation, chunk size sweep.
- Run the PyTorch disaggregated inference demo: prompt in on Machine A → KV-cache over RDMA → tokens out on Machine B. No host DRAM staging.
- Collect final numbers: time-to-first-token, transfer throughput, per-chunk latency, host-staging overhead comparison.

### Files Added

| File | Content |
|------|---------|
| `examples/inference/kvcache_proto.h` | WRITEIMM encoding macros, manifest struct |
| `examples/inference/kvcache_sender.c` | Chunked WRITEIMM sender with tracing |
| `examples/inference/kvcache_receiver.c` | Chunked receiver with bitmap tracking |
| `examples/inference/prefill_server.py` | PyTorch prefill + dmaplane transfer |
| `examples/inference/decode_server.py` | PyTorch decode with zero-copy reconstruction |
| `examples/inference/dmaplane_py.py` | Python ioctl wrapper |
| `tests/test_phase9_writeimm.c` | WRITEIMM loopback, chunk tracking, backpressure |

### What Changes

| File | Change |
|------|--------|
| `driver/dmaplane.h` | WRITEIMM ioctls: `IOCTL_RDMA_WRITE_IMM`, `IOCTL_RDMA_POST_RECV`, `IOCTL_RDMA_POLL_RECV` |
| `driver/rdma_engine.c` | WRITEIMM verb implementation, fast-reg MR path for remote-write access |
| `driver/main.c` | Wire WRITEIMM ioctls |

### Acceptance Criteria

**9A (local):**
- WRITEIMM loopback: immediate value arrives in CQ completion with correct flags
- 128-chunk KV-cache simulation: all chunks tracked via bitmap, zero drops
- Backpressure: sender stalls at credit exhaustion, resumes after receiver replenishes
- Two-machine (laptop → NUC): WRITEIMM chunks arrive with correct sequence numbers over Ethernet
- PyTorch loopback: prompt in → prefill → WRITEIMM transfer → decode → tokens out, all on one machine
- Zero `cudaMemcpy` host staging in the data path
- KV-cache reconstruction via `torch.as_strided` produces correct model output

**9B (EC2):**
- GPU-backed WRITEIMM transfer between two g5.xlarge instances, byte-for-byte correct
- Per-chunk latency histogram: P50, P99, P999 reported
- Chunk size sweep: optimal size identified for A10G + 25 Gbps network
- 10× consecutive transfers: zero errors, stable throughput
- PyTorch demo: prompt in on Machine A → tokens out on Machine B
- Time-to-first-token and host-staging overhead comparison measured and reported

### Skill KPI

You can run a live demo where a prompt goes in on one machine, KV-cache transfers over RDMA with per-chunk tracking, and tokens come out on another machine — with zero host DRAM staging — and you can explain every kernel API, every ioctl, every fence, and every data format in the pipeline.

### Blog Post

`docs/blog_09_disaggregated_inference.md` — WRITEIMM semantics, chunked transfer protocol, credit-window flow control, zero-copy reconstruction with `torch.as_strided`, end-to-end demo, comparison to production systems (Mooncake, TransferEngine).

---

## Execution Discipline

- **One phase at a time.** Don't start Phase N+1 until Phase N's acceptance criteria are met and the blog post is drafted.
- **Git tag per phase.** `git tag phase-1-complete`, `git tag phase-2-complete`, etc. The repo at any tag should build and pass all tests for that phase and all previous phases.
- **Break things on purpose.** After each phase, intentionally violate an invariant (wrong NUMA node, skip CQ drain, unmap before detach) and observe the failure mode. This is where the deepest learning happens.
- **Measure before and after.** Every optimization claim must have numbers. "Hugepages are faster" means nothing. "48× faster SG mapping with 2M pages vs 4K pages at 16 MB buffer size" is knowledge.
- **Read kernel source.** When the documentation is insufficient (it usually is), read the implementation. `mm/page_alloc.c` for allocation, `drivers/infiniband/sw/rxe/` for soft-RoCE, `drivers/dma-buf/` for the sharing framework.

---

## Blog Series Index

| # | Title | Phase | Core Topic |
|---|-------|-------|------------|
| 1 | Building a Kernel Driver from Scratch | 1 | Character devices, locking, ring buffers |
| 2 | DMA Memory: Coherent vs Scatter-Gather | 2 | Allocation strategies, hugepages, IOMMU |
| 3 | The NUMA Tax Nobody Sees | 3 | Topology, placement, the invisible 40% penalty |
| 4 | Kernel-Space RDMA from First Principles | 4 | Verbs API, GID selection, silent failures |
| 5 | Zero-Copy Sharing with dma-buf | 5 | Exporter/importer lifecycle, refcounting, MR bridge |
| 6 | Backpressure: Why CQ Overflow Corrupts Data | 6 | Credit-based flow control, sustained streaming |
| 7 | Instrumenting the Kernel Data Path | 7 | Tracepoints, latency histograms, cache effects |
| 8 | GPU Memory Over the Wire | 8 | BAR mapping, write-combining, GPUDirect RDMA |
| 9 | Disaggregated KV-Cache: Tokens Across Machines | 9 | WRITEIMM, chunked transfer, PyTorch, real inference |

---

## What Success Looks Like

You have succeeded when:

- You can reason about DMA mapping without looking at docs.
- You understand why IOMMU settings matter — you saw it in the SG table merging.
- You can explain how page size impacts TLB and streaming — you measured the 48× difference.
- You can defend zero-copy architecture tradeoffs — you know what zero-copy eliminates and what it still costs.
- You can interpret `perf` output meaningfully — you connected TLB miss counts to hugepage configuration.
- You can look at `lstopo` and predict performance on a multi-socket system.
- You can explain the full GPU-to-network data path and demo it live between two machines.
- You are comfortable reading kernel subsystem source code.

When you later encounter Mooncake, TransferEngine, MORI, or `nvidia-peermem` in production, you recognize the same primitives — because you built them yourself.
