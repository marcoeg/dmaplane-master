# Tests

One test program per phase. Each exercises the new functionality added in that phase.

| Test | Phase | Description |
|------|-------|-------------|
| `test_phase1_driver` | 1 | Character device, ioctl, rings, worker threads, stress test |
| `test_phase2_dma` | 2 | DMA buffer alloc (coherent + pages), mmap, lifecycle, stress |
| `test_phase3_dmabuf` | 3 | dma-buf export, fd lifecycle, mmap via dma-buf, destroy guard |
| `test_phase4_rdma` | 4 | RDMA setup, MR registration, loopback, ping-pong, streaming |
| `test_phase5_dmabuf` | 5 | dma-buf multi-attach and RDMA (planned) |
| `test_phase6_backpressure` | 6 | Credit exhaustion and soak test (planned) |
| `test_phase7_instrument` | 7 | Tracepoint and histogram validation (planned) |
| `test_phase8_gpu` | 8 | GPU pin/unpin and BAR throughput (planned) |
| `test_phase9_writeimm` | 9 | WRITEIMM chunked KV-cache simulation (planned) |

## Running Tests

```bash
make tests                              # Build test programs
sudo insmod driver/dmaplane.ko          # Load the module
sudo ./tests/test_phase1_driver         # Run Phase 1 tests
sudo ./tests/test_phase2_dma            # Run Phase 2 tests
sudo ./tests/test_phase3_dmabuf         # Run Phase 3 tests
sudo ./tests/test_phase4_rdma           # Run Phase 4 tests (needs Soft-RoCE)
sudo rmmod dmaplane                     # Unload
dmesg | tail -20                        # Check for warnings
```
