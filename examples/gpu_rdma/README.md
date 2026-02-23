# GPU RDMA Examples

Cross-machine demo sending GPU VRAM over RDMA to a remote host without GPU.

| Program | Machine | Description |
|---------|---------|-------------|
| `gpu_sender` | A (GPU host) | Pins GPU VRAM, registers as RDMA MR, sends via peer QP over rxe |
| `gpu_receiver` | B (no GPU) | Receives into host buffer, verifies gradient data |

## Prerequisites

- `dmaplane.ko` loaded on BOTH machines
- Soft-RoCE configured on both machines (`bash scripts/setup_rxe.sh`)
- CUDA runtime on Machine A (sender) for `cudaMalloc`
- Real network connectivity between machines (same subnet for rxe)

## Usage

Receiver starts FIRST:

```bash
# Machine B (receiver, start FIRST):
sudo ./examples/gpu_rdma/gpu_receiver 9876

# Machine A (sender, start AFTER receiver):
sudo ./examples/gpu_rdma/gpu_sender <machine_b_ip> 9876
```

## Note

Single-machine loopback does NOT work -- RDMA context is a device singleton.
Use `test_phase8_gpu` test 7 (GPU RDMA loopback) for single-machine validation.

## Architecture

```
Machine A (GPU)              Machine B (no GPU)
┌─────────────┐              ┌─────────────┐
│ GPU VRAM    │              │ Host DRAM   │
│ (cudaMalloc)│              │ (page-backed)│
└──────┬──────┘              └──────┬──────┘
       │ nvidia_p2p_get_pages        │ alloc_pages
       ▼                             ▼
┌─────────────┐              ┌─────────────┐
│ BAR1 pages  │              │ Host buffer │
│ (ioremap_wc)│              │ (vmap)      │
└──────┬──────┘              └──────┬──────┘
       │ GPU MR (sge_addr=WC VA)    │ Host MR
       ▼                             ▼
┌─────────────┐   Ethernet   ┌─────────────┐
│ qp_peer SEND├──────────────►qp_peer RECV │
└─────────────┘   (rxe/UDP)  └─────────────┘
```

## Building

```bash
make                                    # From repo root
make -C examples/gpu_rdma               # Or directly
```
