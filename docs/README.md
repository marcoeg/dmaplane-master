# Documentation

Blog posts and technical notes documenting each phase of the dmaplane project.

| # | Title | Status |
|---|-------|--------|
| 1 | Driver Foundations & Concurrency | Complete |
| 2 | DMA Memory: Two Allocation Paths and the Zero-Copy Chain | Complete |
| 3 | dma-buf Export and Zero-Copy Sharing | Complete |
| 4 | Kernel-Space RDMA from First Principles | Complete |
| 5 | NUMA Topology and the Silent Performance Killer | Complete |
| 6 | Backpressure: Why CQ Overflow Corrupts Data | Complete |
| 7 | Instrumenting the Kernel Data Path | Complete |
| 8 | GPU Memory Over the Wire | Complete |
| 9 | WRITEIMM and the KVCache Pipeline | Complete |
| 9D | Disaggregated KV-Cache: Tokens Across Machines | Draft |

## Files

### Blog posts

- `blog_01_driver_foundations.md` — Phase 1
- `blog_02_dma_memory.md` — Phase 2
- `blog_03_dmabuf_zero_copy.md` — Phase 3
- `blog_04_rdma_engine.md` — Phase 4
- `blog_05_numa_topology.md` — Phase 5
- `blog_06_backpressure.md` — Phase 6
- `blog_07_instrumentation.md` — Phase 7
- `blog_08_gpu_rdma.md` — Phase 8
- `blog_09_writeimm_kvcache.md` — Phase 9A
- `blog_09d_disaggregated_inference.md` — Phase 9D (draft, pending EC2 numbers)

### Technical notes

- `phase01_technical_notes.md` through `phase08_technical_notes.md`

### Reference documents

- `reference/MASTER_PLAN.md` — 9-phase engineering plan
- `reference/CODE_OVERVIEW.md` — Codebase analysis (updated each phase)
- `reference/NARRATIVE_dmaplane_foundation.md` — Project positioning & use cases
