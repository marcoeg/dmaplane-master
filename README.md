# dmaplane

A Linux kernel module for learning the host-side data path between AI frameworks (PyTorch, NCCL) and hardware (GPUs, NICs, DRAM). dmaplane exposes a character device (`/dev/dmaplane`) controlled via ioctl, implementing the buffer orchestration layer that sits beneath transport libraries — DMA allocation, NUMA-aware placement, RDMA memory registration, dma-buf zero-copy sharing, GPU BAR pinning, and credit-based flow control. The project builds cumulatively across nine phases, each targeting a specific kernel subsystem.

## Phase Index

| Phase | Status | Topic |
|-------|--------|-------|
| 1 | Complete | Driver foundations & concurrency |
| 2 | Complete | DMA memory allocation |
| 3 | Complete | dma-buf export & zero-copy sharing |
| 4 | Complete | RDMA engine |
| 5 | **Current** | NUMA, topology & optimization |
| 6 | Planned | Backpressure & flow control |
| 7 | Planned | Instrumentation & latency measurement |
| 8 | Planned | GPU memory integration |
| 9 | Planned | Disaggregated inference demo |

## Build & Test

```bash
# Build the kernel module and test programs
make

# Load the module
sudo insmod driver/dmaplane.ko
ls -la /dev/dmaplane
dmesg | tail -5

# Run Phase 1 tests (regression)
sudo ./tests/test_phase1_driver

# Run Phase 2 tests
sudo ./tests/test_phase2_dma

# Run Phase 3 tests
sudo ./tests/test_phase3_dmabuf

# Run Phase 4 tests (requires Soft-RoCE: bash scripts/setup_rxe.sh)
sudo ./tests/test_phase4_rdma

# Run Phase 5 tests
sudo ./tests/test_phase5_numa

# Run Phase 2 examples
sudo ./examples/misc/dma_explorer
sudo ./examples/misc/dma_sweep

# Unload the module
sudo rmmod dmaplane
dmesg | tail -5
```

## Directory Structure

```
dmaplane-master/
├── CLAUDE.md                  # Claude Code entry point
├── README.md                  # This file
├── LICENSE                    # GPL-2.0
├── Makefile                   # Top-level: delegates to driver/ and tests/
├── driver/                    # Kernel module source (single dmaplane.ko)
│   ├── dmaplane.h             # Kernel-internal header: structs, constants
│   ├── dmabuf_rdma.h          # Buffer subsystem API declarations
│   ├── dmabuf_rdma.c          # Buffer allocation: coherent + page-backed
│   ├── dmabuf_export.h        # dma-buf exporter API declarations
│   ├── dmabuf_export.c        # dma-buf export: dma_buf_ops, SG table, selftest
│   ├── rdma_engine.h          # RDMA engine API declarations
│   ├── rdma_engine.c          # RDMA setup/teardown, MR registration, post/poll
│   ├── benchmark.h            # RDMA benchmark API declarations
│   ├── benchmark.c            # Loopback, ping-pong, streaming benchmarks
│   ├── numa_topology.h        # NUMA topology API declarations
│   ├── numa_topology.c        # Topology query, NxN bandwidth benchmark
│   └── main.c                 # Char device, ioctl dispatch, mmap, module init/exit
├── include/                   # Userspace-visible headers
│   └── dmaplane_uapi.h       # Ioctl numbers, shared structs (kernel ↔ user)
├── tests/                     # One test per phase
│   ├── test_phase1_driver.c   # Phase 1: rings, workers, stress test
│   ├── test_phase2_dma.c      # Phase 2: buffer alloc, mmap, lifecycle
│   ├── test_phase3_dmabuf.c   # Phase 3: dma-buf export, fd lifecycle, mmap
│   ├── test_phase4_rdma.c    # Phase 4: RDMA setup, MR, loopback, benchmarks
│   └── test_phase5_numa.c   # Phase 5: NUMA topology, allocation, benchmark
├── examples/                  # Progressive demos (populated in later phases)
│   ├── misc/                  # Standalone demos (Phase 2+)
│   ├── streamer/              # Weight-streaming TUI (Phase 6)
│   ├── netshare/              # Two-machine RDMA (Phase 4+)
│   └── inference/             # Disaggregated inference (Phase 9)
├── docs/                      # Blog posts (one per phase)
│   └── reference/             # Engineering plan, code overview, narrative
│       ├── MASTER_PLAN.md     # 9-phase engineering plan
│       ├── CODE_OVERVIEW.md   # Codebase analysis (updated each phase)
│       └── NARRATIVE_dmaplane_foundation.md  # Project positioning & use cases
└── scripts/                   # Build and setup automation
```

## References

- **[Full Engineering Plan](docs/reference/MASTER_PLAN.md)** — 9-phase spec with acceptance criteria and skill KPIs
- **[Project Narrative](docs/reference/NARRATIVE_dmaplane_foundation.md)** — Technical essay on positioning, use cases, and relationship to production systems

## License

GPL-2.0 — see [LICENSE](LICENSE).
