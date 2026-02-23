# KVCache Disaggregated Inference

Validates the full disaggregated KVCache transfer pipeline over RDMA WRITE with immediate data (`IB_WR_RDMA_WRITE_WITH_IMM`). Designed to run on a single laptop via soft-RoCE loopback before deploying to EC2.

## How it works

```
Prefill GPU                                  Decode GPU
┌──────────────┐    RDMA WRITE_IMM    ┌──────────────┐
│  TinyLlama   │    + 32-bit imm      │  Reconstruct │
│  prefill     │───────────────────────│  KVCache via │
│              │   chunk by chunk      │  as_strided  │
│  KVCache     │   (credit window)     │              │
│  consolidate │                       │  Continue    │
│  into staging│                       │  decoding    │
└──────────────┘                       └──────────────┘
```

The 32-bit immediate value encodes `(layer_index, chunk_index)` so the receiver knows which KVCache chunk arrived via CQ completion — no memory polling needed. A credit window provides backpressure.

In loopback mode, the sender owns both MRs on the same machine (QP-A sends, QP-B receives). Two-machine peer mode is structurally stubbed for EC2 deployment.

## Prerequisites

```bash
# Load dmaplane and configure soft-RoCE
sudo modprobe rdma_rxe
IFACE=$(ip -o link show up | grep -v lo | head -1 | awk -F: '{print $2}' | tr -d ' ')
sudo rdma link add rxe_${IFACE} type rxe netdev ${IFACE}
make -C driver load     # from repo root

# Build
make -C examples/kvcache
```

For Python tests: `sudo pip install torch transformers accelerate`

## Files

| File | Description |
|------|-------------|
| `kvcache_common.h` | Shared C header: ioctl wrappers, timing, credit tracker, recv loop helpers, latency stats, layer bitmap |
| `kvcache_sender.c` | Loopback WRITEIMM sender with credit window, GPU source path, data verification |
| `kvcache_receiver.c` | Loopback self-test + peer mode stub with recv loop skeleton |
| `dmaplane_py.py` | Python `struct`/`fcntl` wrapper for all dmaplane ioctls |
| `test_dmaplane_py.py` | Python wrapper tests: struct sizes, ioctl round-trips, GPU pin |
| `kvcache_inference.py` | `extract_kvcache`, `consolidate_kvcache`, `reconstruct_kvcache_zero_copy` |
| `test_kvcache_local.py` | End-to-end test: TinyLlama prefill → WRITEIMM → reconstruct → decode |
| `kvcache_bench.sh` | Benchmark sweep: chunk_size x credit_window x layers → CSV |
| `ec2_setup.sh` | EC2 g5.xlarge setup: deps, rxe, build, model download, smoke test |

## Quick start

### C loopback (no GPU required)

```bash
# 128 chunks (32 layers x 4 chunks x 1MB), data integrity check
sudo ./kvcache_sender --loopback --verify

# GPU-backed source MR
sudo ./kvcache_sender --loopback --gpu --verify

# Smaller test
sudo ./kvcache_sender --loopback --layers 4 --chunks-per-layer 2 --verify
```

### Python wrapper validation

```bash
# Struct size check (no root needed)
python3 dmaplane_py.py

# Ioctl round-trip tests (needs root + dmaplane.ko)
sudo python3 test_dmaplane_py.py
```

### Full disaggregated inference test

```bash
# Requires: CUDA GPU, TinyLlama model, dmaplane.ko + rxe
sudo python3 test_kvcache_local.py
```

This runs:
1. Prefill with TinyLlama-1.1B-Chat
2. Extract + consolidate KVCache into aligned GPU staging buffer
3. WRITEIMM chunks over loopback (GPU MR → host MR)
4. Reconstruct KVCache via `torch.as_strided` (zero-copy)
5. Greedy-decode 20 tokens and verify they match `model.generate()` output

### Benchmark sweep

```bash
sudo bash kvcache_bench.sh
# Writes bench_results.csv with throughput/latency across parameter combinations
```

## Sender CLI reference

```
--loopback            Self-contained loopback test (default)
--peer <ip:port>      Connect to remote receiver (stubbed)
--gpu                 GPU-backed source MR via IOCTL_GPU_PIN
--ib-dev <name>       IB device (auto-detects rxe_* if omitted)
--layers N            Simulated KVCache layers (default: 32)
--chunks-per-layer N  Chunks per layer (default: 4)
--chunk-size N        Bytes per chunk (default: 1048576)
--credit-window N     Max in-flight WRITEIMMs (default: 16)
--iterations N        Repeat full transfer (default: 1)
--verify              Fill pattern + memcmp after transfer
--csv                 Single-line CSV: total_mb,throughput,avg,p50,p99
```

## Architecture notes

- **No libibverbs** — all RDMA ops go through kernel ioctls, not userspace verbs
- **Fast-reg MR** — destination MR uses `ib_alloc_mr` + `IB_WR_REG_MR` for valid rkey
- **Credit window** — sender tracks pre-posted recvs; stalls and drains when credits hit 0
- **64KB alignment** — GPU tensors are over-allocated and sliced for NVIDIA P2P API compatibility
- **Zero-copy reconstruct** — `torch.as_strided` views into the RDMA landing buffer, no memcpy
