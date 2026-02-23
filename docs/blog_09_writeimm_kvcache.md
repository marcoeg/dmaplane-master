<!-- SPDX-License-Identifier: GPL-2.0 -->
<!-- Copyright (c) 2026 Graziano Labs Corp. -->

# The Verb Nobody Talks About: RDMA WRITE WITH IMMEDIATE for KVCache Transfer

*Part 9A of 9 in a series on building a host-side data path emulator for AI infrastructure*

---

Eight phases built the full host-side data path: ring buffers, DMA allocation, dma-buf zero-copy sharing, RDMA transport, NUMA topology, flow control, instrumentation, and GPU memory integration. The previous phase proved that GPU VRAM could traverse the RDMA wire -- pinned through the BAR, registered as an MR, sent to a second machine, verified byte-for-byte. But the Phase 8 demo shipped a single 1 MB buffer. A real disaggregated inference system ships a KV-cache: dozens of transformer layers, each split into multiple chunks, each chunk a megabyte or more. The receiver needs to know not just that *data* arrived, but *which* data -- which layer, which chunk, where in the reconstruction buffer it belongs.

This is the notification problem, and it turns out the InfiniBand spec solved it twenty years ago with a verb that most RDMA tutorials never mention: `IB_WR_RDMA_WRITE_WITH_IMM`.

---

## The Notification Problem in Disaggregated Inference

In a production disaggregated LLM inference deployment, the "prefill" GPU computes the KV-cache for an incoming prompt. The KV-cache is large -- a 7B model has roughly 32 transformer layers, each producing 4-16 MB of key-value state. This data must be shipped chunk-by-chunk to a "decode" GPU on another machine. The decode side reconstructs the KV-cache and begins autoregressive token generation.

The decode side needs to know which chunk arrived. Without that knowledge, it cannot place the data at the correct offset in its reconstruction buffer, and it cannot determine when an entire layer is complete (all chunks received) to begin processing that layer.

There are three ways to solve this:

**Memory polling.** The receiver continuously scans its receive buffer for non-zero data, inferring arrivals from content changes. This works only for buffers initialized to zero, fails if a legitimate data value is zero, and burns CPU cycles proportional to buffer size. Production systems do not do this.

**Separate control messages.** After each RDMA WRITE, the sender posts a separate SEND carrying a small header (layer, chunk, offset). The receiver matches data arrivals to control messages. This doubles the number of RDMA operations and adds a round-trip of latency per chunk. It works, but it wastes half the available bandwidth on messages that carry no data.

**WRITE WITH IMMEDIATE.** The sender posts `IB_WR_RDMA_WRITE_WITH_IMM`. This verb does two things atomically: it writes data to the remote buffer at a specified address (like regular RDMA WRITE), and it delivers a 32-bit immediate value through the receiver's completion queue. One verb, one completion, zero extra round trips. The receiver reads the immediate from the CQ entry and knows exactly what arrived.

The 32-bit immediate is enough. For KV-cache tracking: 16 bits of layer index (65,535 layers) and 16 bits of chunk index (65,535 chunks per layer). A 7B model with 32 layers and 16 chunks per layer uses 0.05% of the encoding space.

---

## Three Ioctls, One Verb

Phase 9A adds three new kernel ioctls to dmaplane's RDMA engine. Together they expose the full WRITE WITH IMMEDIATE lifecycle to userspace.

**RDMA_WRITE_IMM (0x80)** posts `IB_WR_RDMA_WRITE_WITH_IMM` on the selected QP. The kernel implementation uses `struct ib_rdma_wr` -- not the plain `ib_send_wr` -- because RDMA WRITE requires `remote_addr` and `rkey` fields that exist only on the RDMA-specific work request type:

```c
struct ib_rdma_wr rdma_wr = {};

sge.addr = mr_copy.sge_addr + local_offset;
sge.length = length;
sge.lkey = mr_copy.lkey;

rdma_wr.wr.opcode = IB_WR_RDMA_WRITE_WITH_IMM;
rdma_wr.wr.send_flags = IB_SEND_SIGNALED;
rdma_wr.wr.ex.imm_data = cpu_to_be32(imm_data);
rdma_wr.wr.sg_list = &sge;
rdma_wr.wr.num_sge = 1;
rdma_wr.remote_addr = remote_addr;
rdma_wr.rkey = remote_rkey;

ret = ib_post_send(qp, &rdma_wr.wr, &bad_wr);
```

The call to `ib_post_send` takes `&rdma_wr.wr` -- the embedded `ib_send_wr` inside the larger `ib_rdma_wr`. This is a pattern that trips up every first-time RDMA programmer: the verbs API uses structure embedding, not inheritance. The transport provider casts the base `ib_send_wr` back to `ib_rdma_wr` based on the opcode to recover the RDMA-specific fields.

**RDMA_POST_RECV (0x81)** posts a receive work request on the destination QP. Each WRITE WITH IMMEDIATE consumes exactly one recv WR on completion -- the IB spec mandates this because the immediate value is delivered through the recv CQ entry, which requires a recv WR to exist. Without a posted recv, the remote QP hits RNR (Receiver Not Ready) and retries until timeout.

**RDMA_POLL_RECV (0x82)** polls the recv CQ and extracts the immediate data in host byte order:

```c
*imm_data_out = (wc.wc_flags & IB_WC_WITH_IMM) ?
                be32_to_cpu(wc.ex.imm_data) : 0;
```

The byte order conversion is critical. The IB spec mandates that `imm_data` is transmitted in network byte order (big-endian). The sender calls `cpu_to_be32()` before posting; the receiver calls `be32_to_cpu()` after polling. On x86 (little-endian), getting this wrong causes silent corruption for any value where the upper bytes are non-zero. The value 0x00010002 (layer 1, chunk 2) would be read back as 0x02000100 (layer 512, chunk 256) -- wrong layer, wrong chunk, wrong offset, correct-looking data at the wrong location. The test would pass for single-byte values and fail mysteriously at scale.

---

## The Credit Window: Backpressure Without RNR

The recv WR requirement creates a natural flow control mechanism. If the sender outruns the receiver and exhausts all posted recv WRs, the next WRITE WITH IMMEDIATE hits RNR on the remote QP. The transport retries (up to `rnr_retry` times), but this is a recovery path, not a performance path. The retries add latency and waste bandwidth.

The solution is a credit window. Before the transfer begins, the sender pre-posts `credit_window` recv WRs on the destination QP. Each WRITE WITH IMMEDIATE consumes one credit. When credits reach zero, the sender stalls: it polls the recv CQ for a completion, posts a fresh recv WR (replenishing one credit), and then continues sending.

```c
if (ct.credits <= 0) {
    uint64_t stall_start = now_ns();

    while (ct.credits <= 0) {
        uint64_t cs = now_ns();
        int rc = recv_loop_poll_and_track(
            fd, 0, 10000, &bm, &ls, &cs);
        /* replenish posts a new recv and increments ct.credits */
        recv_loop_replenish(fd, dst_mr.mr_id,
                            cfg->chunk_size, 0, &ct);
    }
    ct.stall_count++;
    ct.total_stall_ns += now_ns() - stall_start;
}
```

Only `recv_loop_replenish()` increments credits -- it is the physical event (posting a recv WR) that creates capacity for one more send. This invariant is the key correctness property: the credit count always reflects the actual number of recv WRs available on the remote QP.

The credit window size controls the tradeoff between latency and throughput. A window of 1 serializes everything -- send, wait for completion, replenish, send again. A window of 64 allows deep pipelining but requires 64 pre-posted recv WRs (memory) and risks 64 in-flight operations during teardown (complexity). The benchmarks below show that window=16 captures most of the throughput while keeping resource consumption modest.

---

## KVCache Chunk Tracking: 32 Bits of Protocol

The `kvcache_proto.h` header defines the encoding:

```c
#define KVCACHE_IMM_ENCODE(layer, chunk) \
    ((((uint32_t)(layer) & 0xFFFF) << 16) | ((uint32_t)(chunk) & 0xFFFF))

#define KVCACHE_IMM_LAYER(imm)  (((uint32_t)(imm) >> 16) & 0xFFFF)
#define KVCACHE_IMM_CHUNK(imm)  ((uint32_t)(imm) & 0xFFFF)

#define KVCACHE_SENTINEL        0xFFFFFFFF
```

Upper 16 bits carry the layer index. Lower 16 bits carry the chunk index. The sentinel value 0xFFFFFFFF signals end-of-transfer -- it cannot collide with a valid (layer, chunk) pair because both fields would have to be 0xFFFF simultaneously, which exceeds any practical model size.

The receiver maintains a per-layer bitmap that tracks which chunks have arrived:

```c
struct layer_bitmap {
    uint64_t bits[MAX_LAYERS];   /* one bit per chunk, per layer */
    int      chunks_per_layer;
    int      num_layers;
};
```

When a completion arrives, the receiver decodes the immediate, sets the corresponding bit, and checks whether the layer is complete (all bits set). This is constant-time per chunk and constant-space per layer -- 64 bits per layer supports up to 64 chunks, which is far more than any practical KV-cache partitioning requires.

The entire protocol is 32 bits wide. No header parsing, no variable-length fields, no allocation. The CQ entry carries the metadata directly. This is the design advantage of WRITE WITH IMMEDIATE over SEND-based notification: the data and the metadata arrive in a single atomic operation, and the metadata extraction is a bit shift and a mask.

---

## Host vs. GPU: The 117x Gap That Hardware Closes

The loopback benchmark transfers 128 MB (32 layers x 4 chunks x 1 MB) with a credit window of 16 and data integrity verification:

**Host source:**

```
  Throughput: 1201.8 MB/s
  Per-chunk: avg=0.5ms P50=0.8ms P99=1.2ms
  Credits stalled: 112 times
  Chunks: 128/128 received
  Data integrity: PASS
```

112 credit stalls on 128 chunks: the first 16 chunks use the initial credits, and each subsequent chunk stalls once to poll and replenish. This is expected behavior -- the credit window is working exactly as designed, preventing RNR without requiring thousands of pre-posted recvs.

**GPU source (same 128 MB):**

```
  Throughput: 10.3 MB/s
  Per-chunk: avg ~98ms
```

117x slower. The same kernel code, the same ioctls, the same RDMA verb. The only difference is where the source data lives.

The reason is rxe. Soft-RoCE is a software transport: when `ib_post_send` is called with a GPU-backed MR, rxe reads the source data via the CPU using the `ioremap_wc` BAR mapping. For a 1 MB chunk, rxe fragments it into approximately 256 individual IB packets (4 KB MTU). Each packet requires rxe to: read 4 KB from the GPU BAR (a PCIe non-posted read at ~500ns per cacheline), compute an ICRC checksum, build a UDP/IP header, and hand the packet to the network stack. With 256 packets per chunk and ~400us per packet, a 1 MB chunk takes roughly 100ms.

This is the bottleneck that GPUDirect RDMA hardware eliminates. A ConnectX NIC with `nvidia-peermem` does not read through the CPU. Its DMA engine reads the GPU BAR pages directly, pipelining dozens of PCIe read requests simultaneously. Where the CPU achieves ~107 MB/s reading from the BAR (one outstanding read at a time), the NIC's DMA engine achieves 12-24 GB/s (64+ outstanding reads). The kernel code is identical. The `ib_rdma_wr` structure, the `imm_data` encoding, the credit window, the recv WR consumption -- all of it works the same way. Only the transport changes.

The 10.3 MB/s number is actually a better demonstration of why GPUDirect RDMA matters than any marketing slide. It shows the concrete cost of CPU-mediated GPU memory access over RDMA. Replace rxe with ConnectX, and the GPU path matches or exceeds the host path because the NIC DMA-reads the BAR directly at PCIe line rate.

---

## Benchmark Sweep: Chunk Size vs. Throughput

A 32-point sweep across chunk sizes with a credit window of 16 reveals the throughput curve:

| Chunk Size | Throughput | Avg Latency | P50 | P99 |
|-----------|-----------|-------------|-----|-----|
| 64 KB | 309 MB/s | 0.1ms | 0.2ms | 0.3ms |
| 256 KB | 1025 MB/s | 0.1-0.3ms | 0.2-0.4ms | 0.4-0.6ms |
| 1 MB | 1114 MB/s | 0.5-0.7ms | 0.8ms | 1.2-1.4ms |
| 4 MB | 1208 MB/s | 1.8-2.3ms | 3.2ms | 3.6-5.8ms |

Throughput saturates around 1.2 GB/s -- rxe's single-threaded `memcpy` limit on this machine. Below 256 KB, per-operation overhead dominates: the ioctl round-trip, CQ polling, and credit bookkeeping cost roughly 100us regardless of chunk size. Above 1 MB, memcpy time dominates and throughput plateaus.

The credit window effect is measurable but small at large chunk sizes. Reducing the window from 16 to 2 (8 layers x 4 chunks = 32 chunks, 30 stalls) drops throughput from 1201 to 1045 MB/s -- a 13% penalty from the additional poll-and-replenish overhead per chunk. The stall time per event shows as "0.0ms" because in loopback, the recv completion is already in the CQ by the time the sender polls for it. On a real network with microsecond-scale latency, each stall would cost a round-trip, and the 13% penalty would grow.

For production KV-cache transfer, 1 MB chunks at window=16 hit the sweet spot: high throughput (>1 GB/s on loopback), manageable per-chunk latency (<1ms P50), and modest resource consumption (16 recv WRs pre-posted at any time).

---

## The Reference Implementation

The `examples/kvcache/` directory contains a complete reference implementation: C sender, C receiver, shared header, Python wrapper, and benchmark harness.

**kvcache_sender.c** owns the full loopback pipeline. In loopback mode, the sender controls both QP-A (send) and QP-B (recv) on the same machine, mimicking the two-machine case without TCP setup. The transfer flow:

1. Setup RDMA (PD, CQs, loopback QP pair)
2. Create source MR (host or GPU) and destination MR (host, fast-reg with `REMOTE_WRITE`)
3. Pre-post `credit_window` recv WRs on QP-B
4. For each (layer, chunk): `WRITE_IMM` on QP-A, decrement credits; stall and replenish when credits hit zero
5. Send `KVCACHE_SENTINEL` (imm=0xFFFFFFFF)
6. Drain remaining recv completions and verify data integrity

**kvcache_common.h** contains the shared infrastructure: ioctl wrappers, credit tracker, latency statistics, layer bitmap, and recv loop helpers. The `recv_loop_poll_and_track()` function polls one completion, decodes the immediate, updates the bitmap and latency stats -- a single function that both sender (loopback drain) and receiver (peer mode) can use.

**kvcache_proto.h** defines only the IMM encoding macros and the sentinel value. It is intentionally minimal -- a userspace-only header with no kernel dependencies, suitable for inclusion in any language's FFI bindings.

**dmaplane_py.py** wraps the three new ioctls (plus existing buffer and RDMA setup) for Python. The ioctl numbers required updating when flow control moved from 0x60 to 0x40 and GPU moved from 0x80 to 0x60 -- a consequence of the ioctl renumbering that freed the 0x80 range for WRITE WITH IMMEDIATE. The C code uses symbolic macros (`IOCTL_RDMA_WRITE_IMM`), so recompilation was sufficient. The Python wrapper hardcodes the ioctl numbers as computed constants, so three buffer ioctl values needed manual correction.

---

## Ioctl Renumbering

Phase 9A required the 0x80 range for the new WRITEIMM ioctls. The existing occupants:

- Flow control: moved from 0x60-0x63 to 0x40-0x43
- GPU P2P: moved from 0x80-0x87 to 0x60-0x67

Both groups use symbolic macros (`DMAPLANE_IOCTL_CONFIGURE_FLOW`, `DMAPLANE_IOCTL_GPU_PIN`, etc.) defined once in `dmaplane_uapi.h`. Changing the number at the definition point propagates to every C file that includes the header. No behavioral change, no ABI break for code that recompiles. The only casualties were hardcoded values in the Python wrapper, which required three manual fixes.

The final ioctl map:

```
0x01-0x04  Phase 1: channels, submit/complete, stats
0x05-0x09  Phase 2: buffer management
0x0A-0x0B  Phase 3: dma-buf export
0x10-0x11  Phase 4: RDMA setup/teardown
0x20-0x21  Phase 4: MR register/deregister
0x30-0x33  Phase 4: benchmarks
0x40-0x43  Phase 6: flow control (moved from 0x60)
0x50-0x51  Phase 5: NUMA topology
0x60-0x67  Phase 8: GPU P2P (moved from 0x80)
0x70       Phase 7: histogram
0x80-0x82  Phase 9: WRITE WITH IMMEDIATE
0x90-0x94  Phase 8: peer RDMA
```

---

## Connection to Production

The three new ioctls implement the exact verb that Mooncake's TransferEngine uses for KV-cache chunk delivery between prefill and decode GPUs. TransferEngine's `submitTransfer()` with `RDMA_WRITE_WITH_IMM` carries segment metadata in the immediate field. The receiver's CQ handler decodes the immediate to determine buffer placement and transfer completion -- the same pattern as the layer/chunk bitmap in `kvcache_common.h`.

The credit window is a simplified version of the credit-based flow control that every production RDMA transport uses. NCCL's `ib_send_credits` and `ib_recv_credits` track exactly the same invariant: the number of recv WRs available on the remote side. Mooncake uses a variant where the receiver periodically sends credit updates via SEND operations. The principle is identical -- the sender must never post more WRITEIMMs than there are recvs to absorb them.

The 117x GPU-vs-host throughput gap is the core argument for GPUDirect RDMA hardware. With rxe, every byte of GPU data is read by the CPU through the BAR, one PCIe transaction at a time. With ConnectX and `nvidia-peermem`, the NIC's DMA engine reads the BAR directly, pipelining dozens of PCIe reads and achieving line rate. The kernel module's WRITEIMM implementation is transport-agnostic -- the same `ib_rdma_wr` structure, the same `cpu_to_be32(imm_data)`, the same recv WR consumption. Swapping rxe for ConnectX changes the transport, not the architecture.

---

*The code -- WRITEIMM kernel ioctls, credit window, KVCache chunk protocol, and loopback/GPU sender -- is built as part of the dmaplane loadable kernel module against a stock Ubuntu kernel. The GPU source path resolves `nvidia_p2p_get_pages` at runtime via `symbol_get` and activates only when the NVIDIA driver is present. The host path works on any machine with Soft-RoCE.*

*Next: [Part 9B -- Disaggregated Inference Demo](/docs/blog_09_disaggregated_inference.md)*
