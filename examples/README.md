# Examples

Progressive demos that build on each phase.

| Directory | Phase | Description |
|-----------|-------|-------------|
| `misc/` | 2, 7 | `dma_explorer`, `dma_sweep`, `trace_explorer` |
| `gpu_rdma/` | 8 | Cross-machine GPU VRAM over RDMA (`gpu_sender` / `gpu_receiver`) |
| `kvcache/` | 9 | KVCache WRITEIMM pipeline, C sender/receiver, Python inference servers |
| `netshare/` | 4+ | Two-machine RDMA (placeholder) |
| `streamer/` | 6 | Weight-streaming TUI (deferred) |
| `inference/` | 9 | Redirect to `kvcache/` |
