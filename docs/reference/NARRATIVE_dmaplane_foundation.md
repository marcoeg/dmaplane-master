# The DMA Streaming Framework

## A Buffer Orchestration Layer for High-Performance AI

---

## Abstract

Modern AI workloads — disaggregated inference, mixture-of-experts routing, reinforcement learning weight synchronization — share a common systems problem: moving large tensors between heterogeneous devices (GPU VRAM, host DRAM, RDMA NICs) with minimal latency and zero unnecessary copies. The transport layer that sends bytes over the wire is well understood. What sits below it — allocating DMA-mapped buffers on the correct NUMA node, pinning GPU memory through PCIe BAR apertures, registering memory regions with the RDMA subsystem, sharing buffers across device boundaries via dma-buf, and enforcing flow control so that producers don't overwhelm consumers — is infrastructure that every high-performance AI system needs but that no single framework provides.

This paper introduces dmaplane, a Linux kernel module that implements this buffer orchestration layer. dmaplane manages the complete lifecycle of DMA-mapped memory across heterogeneous device boundaries: it allocates NUMA-aware host buffers, pins GPU VRAM through NVIDIA's peer-to-peer API, constructs scatter-gather tables from PCIe BAR pages, exports and imports buffers via dma-buf for zero-copy device sharing, registers memory regions with the RDMA subsystem, and enforces credit-based flow control on completion queues. The module exposes these capabilities through a single character device `/dev/dmaplane` controlled via typed ioctls, enabling both C and Python userspace to orchestrate buffer lifecycles without touching verbs libraries or vendor SDKs.

As its flagship demonstration, this paper presents the implementation of a zero-copy disaggregated KV-cache built on dmaplane. Disaggregated inference — separating the prefill phase from the decode phase across distinct GPU-equipped hosts — has emerged as an architectural pattern for improving utilization in large language model serving. LLM inference has a fundamental resource conflict. The initial prefill forward pass over the input prompt is compute-intensive: it saturates GPU ALUs across all layers in a single burst. Decode, the autoregressive token generation that follows, is memory-intensive: each step reads the entire key-value cache but performs only a fraction of the computation. Running both on the same GPU means one phase always wastes what the other needs. During decode, the compute units sit idle while the memory bus is saturated. During prefill, KV-cache memory competes with model weights for VRAM capacity. Disaggregated inference resolves this by running each phase on a separate GPU-equipped host, allowing prefill and decode to scale independently with hardware matched to their respective bottlenecks.

The critical cost of this separation is the inter-host transfer of the key-value cache (KV-cache), a dense tensor structure that encodes the attention state of the input sequence and grows linearly with sequence length and model depth. In essence, the prefill GPU creates the KV-cache as a byproduct of the full forward pass over the input prompt, and the decode GPU consumes it so that every subsequent token generation step reads the cache to attend to all previous positions. "Disaggregated KV-cache" means the cache is produced on one machine and must be shipped to another before decoding can begin.

Using dmaplane's buffer orchestration primitives, the system uses `IB_WR_RDMA_WRITE_WITH_IMM` to stream KV-cache chunks directly between GPUs over standard Ethernet — with no host DRAM staging, no userspace buffer copies, and no serving framework dependencies. The 32-bit RDMA immediate field carries per-chunk metadata, enabling the receiver to track which layer and chunk arrived via CQ completion without polling memory. On the receiver, the KV-cache is reconstructed as zero-copy tensor views via `torch.as_strided`, allowing the decode model to read attention state directly from the RDMA landing zone.

The complete pipeline — from PyTorch `model.forward()` on the prefill host to token generation on the decode host — is validated on two Amazon EC2 `g5.xlarge` instances (NVIDIA A10G, 24 GB VRAM) connected via soft-RoCE over Ethernet. The system transfers 18 MB of TinyLlama-1.1B KV-cache in 19 one-megabyte chunks with per-chunk identification and credit-based flow control. The source code is open source and available at [github.com/marcoeg/dmaplane](https://github.com/marcoeg/dmaplane).

---

## 1. Introduction

### 1.1 The Buffer Orchestration Problem

High-performance AI systems move tensors between devices that speak different memory protocols. GPU VRAM lives behind PCIe BAR apertures and lacks `struct page` entries. Host DRAM is managed by the kernel's page allocator and IOMMU. RDMA NICs require memory regions registered with specific access flags and remote keys. Sharing a buffer between a GPU and a NIC — the basic operation underlying any GPU-direct transfer — requires navigating all three memory domains: pinning the GPU pages, constructing scatter-gather tables from BAR physical addresses, registering the result with the RDMA subsystem, and ensuring the completion queue doesn't overflow when the producer outpaces the consumer.

Every system that moves data between GPUs over a network must solve these problems. Mooncake [3] and Perplexity's TransferEngine [6] solve them inside their transport libraries. NVIDIA's `nvidia-peermem` solves the GPU-to-NIC piece inside a proprietary kernel module. But the buffer orchestration layer itself — allocation, pinning, registration, sharing, flow control, and teardown — is not exposed as a reusable primitive. Each system rebuilds it internally.

dmaplane makes this layer explicit. It is a Linux kernel module that manages the complete lifecycle of DMA-mapped memory across heterogeneous device boundaries, exposing each operation as a typed ioctl on a single character device. The module is transport-agnostic (it works on soft-RoCE, ConnectX, or any `ib_device`), GPU-optional (GPU features activate at runtime via `symbol_get` and degrade gracefully), and framework-independent (no PyTorch, no NCCL, no vendor SDK beyond the NVIDIA kernel headers).

### 1.2 Disaggregated Inference as a Motivating Workload

To validate dmaplane's buffer orchestration primitives end-to-end, we implement disaggregated KV-cache transfer — the data movement problem at the heart of prefill-decode separation.

Standard LLM inference performs both phases on the same GPU. Prefill is compute-bound: a full forward pass processes all input tokens in parallel, saturating the GPU's arithmetic units. Decode is memory-bound: each new token requires reading the entire KV-cache but performs only a single-token matrix-vector product. These phases have opposite resource profiles. Running both on the same device means the GPU is either underutilized during decode (excess compute, idle ALUs) or memory-constrained during prefill (KV-cache competes with model weights for VRAM).

Disaggregated inference, as proposed by Splitwise [1], DistServe [2], Mooncake [3], and Perplexity's TransferEngine [6], addresses this by running prefill and decode on separate machines. The prefill GPU computes the KV-cache and sends it to a decode GPU, which generates tokens autoregressively. This decoupling allows independent scaling — more prefill instances when prompts are long, more decode instances when output length dominates.

The challenge is the transfer. KV-cache for a 7B-parameter model at 2048 tokens in float16 is approximately 100 MB. For 70B models with long contexts, it reaches multiple gigabytes. This data must move between machines with minimal latency — the transfer time adds directly to time-to-first-token (TTFT), the primary quality-of-service metric for interactive serving.

### 1.3 Why the Host Staging Path Is Insufficient

The conventional approach to inter-GPU data transfer stages data through host DRAM: `cudaMemcpy` from GPU to host, RDMA send from host, RDMA receive to host on the other side, `cudaMemcpy` from host to GPU. Each `cudaMemcpy` crosses the PCIe bus, consuming 1.5–3 ms per direction for the buffer sizes typical of KV-cache. The total host-staging overhead for an 18 MB transfer (approximately 4.5 ms) exceeds the transfer time itself at 25 Gbps network speeds (5.8 ms). Host staging adds roughly 77% overhead on top of the wire time.

GPUDirect RDMA eliminates these copies by allowing the NIC to read and write GPU VRAM directly through the PCIe Base Address Register (BAR), bypassing host DRAM entirely. On hardware NICs (ConnectX-7), the NIC's DMA engine accesses GPU memory at full PCIe bandwidth (12–24 GB/s on Gen4 ×16). On software RDMA (soft-RoCE/rxe), the kernel reads the BAR mapping via write-combining CPU access at approximately 10 GB/s for writes and 107 MB/s for reads — slower, but architecturally identical.

However, GPUDirect RDMA does not work out of the box. Before the NIC can touch GPU memory, someone must pin the VRAM pages via `nvidia_p2p_get_pages`, map them into kernel address space with `ioremap_wc`, construct a scatter-gather table from the BAR physical addresses (which lack `struct page` entries and cannot use standard DMA APIs), register the result as an RDMA memory region, and handle the asynchronous unpin callback safely when `cudaFree` revokes the pages. This is the buffer orchestration that dmaplane provides — it bridges the gap between "GPUDirect RDMA exists as a capability" and "a specific buffer in GPU VRAM is ready for the NIC to read."

Moreover, dmaplane implements this GPU-direct path from first principles: `nvidia_p2p_get_pages` for BAR pinning, `ioremap_wc` for write-combining mappings, manual SG table construction for RDMA MR registration, and `IB_WR_RDMA_WRITE_WITH_IMM` for chunked streaming with per-chunk notification. The host staging path is not just slower; it is architecturally unnecessary when the buffer orchestration layer correctly bridges GPU memory and the RDMA subsystem.

### 1.4 Contributions

This work makes three contributions:

1. **A general-purpose buffer orchestration layer for heterogeneous AI systems.** dmaplane manages DMA-mapped memory across GPU VRAM, host DRAM, and RDMA NICs through a single kernel module. It handles NUMA-aware allocation, GPU BAR pinning, dma-buf export/import, MR registration (both local-key and fast-reg paths), and credit-based flow control — the complete set of primitives that transport libraries assume are already solved.

2. **A zero-copy disaggregated KV-cache pipeline built on this layer.** The sender consolidates KV-cache into a pre-pinned GPU staging buffer (HBM-to-HBM copy at ~500 GB/s), streams chunks via WRITEIMM with per-chunk layer/chunk identification in the 32-bit immediate field, and the receiver reconstructs typed tensor views via `torch.as_strided` directly into the RDMA landing zone — no host DRAM staging, no deserialization, no memory allocation on the receive path.

3. **An end-to-end validation on commodity cloud hardware.** We demonstrate real token generation across two EC2 `g5.xlarge` instances using soft-RoCE over standard Ethernet — proving that the architecture is correct and portable, independent of InfiniBand or ConnectX hardware. The kernel APIs, data path, and flow-control protocol are identical to those used by production systems; only the link speed and NIC hardware differ.

---

## 2. Background

### 2.1 RDMA Verbs and Transport Semantics

Remote Direct Memory Access (RDMA) allows a host channel adapter (HCA) to read or write remote memory without involving the remote CPU. The InfiniBand verbs API exposes several operation types relevant to KV-cache transfer:

**`IB_WR_SEND`** transmits data from the sender's memory region (MR) to the receiver, which must have pre-posted a matching receive work request (WR). The receiver learns that data arrived but has no metadata about its content; it must infer the payload's identity from ordering or an application-level header.

**`IB_WR_RDMA_WRITE`** writes data directly to a specified address in the receiver's MR using the receiver's remote key (`rkey`). This is a one-sided operation: no receive WR is consumed, and no completion appears on the receiver's completion queue (CQ). The receiver has no notification that a write occurred.

**`IB_WR_RDMA_WRITE_WITH_IMM`** combines both: the sender writes data to a remote address *and* generates a completion on the receiver's CQ carrying a 32-bit immediate value. The receiver must have a pre-posted receive WR (which the WRITEIMM consumes), and the completion entry includes the immediate data in `wc.ex.imm_data`.

In our disaggregated KV-cache implementation, the transfer with dmaplane is done with the WRITEIMM verb because it requires both remote memory targeting (the sender places each chunk at a known offset in the receiver's buffer) and per-chunk notification (the receiver needs to know which layer and chunk arrived). The 32-bit immediate encodes `(layer_index << 16) | chunk_index`, supporting up to 65,535 layers and 65,535 chunks per layer.

### 2.2 GPU VRAM Access via PCIe BAR

GPU VRAM is not directly accessible to the host kernel's memory management subsystem. It lacks `struct page` entries, which means standard DMA APIs (`dma_map_sg`, `ib_reg_mr`) cannot operate on it. Access requires NVIDIA's peer-to-peer API:

```c
nvidia_p2p_get_pages(p2p_token, va_space, gpu_va, size,
                     &page_table, free_callback, data);
```

This call pins a region of GPU VRAM and returns an array of BAR1 page descriptors — physical addresses in the GPU's PCIe BAR aperture. Each descriptor represents a 64 KB page. dmaplane maps these into a contiguous kernel virtual address range via `ioremap_wc` (write-combining) and constructs a scatter-gather table with manually-set DMA addresses, bypassing the IOMMU path entirely. The resulting mapping is registered as an RDMA MR using the protection domain's `local_dma_lkey`.

The 64 KB alignment constraint is imposed by the NVIDIA P2P API: both the virtual address and size of the pinned region must be multiples of 64 KB. For PyTorch tensors, which are sub-allocated from `cudaMalloc` blocks at arbitrary offsets, this requires controlled over-allocation and alignment slicing.

### 2.3 Related Work

**Mooncake** [3] implements a KV-cache-centric disaggregated architecture with a topology-aware RDMA transport. It handles NIC selection, multi-path routing, and RDMA connection management, but assumes that allocation, pinning, access-flag registration, and flow control are already in place — it assumes the entire buffer orchestration layer is in place. dmaplane operates at this buffer orchestration layer.

**Perplexity's TransferEngine** [6] is a portable RDMA-based point-to-point communication library implemented in Rust, designed to work across both NVIDIA ConnectX (RC transport) and AWS EFA (SRD transport). Its key contribution is the *ImmCounter*: a completion notification primitive that tracks 32-bit immediate values and fires callbacks when a target count is reached, providing reliable synchronization without depending on message ordering. The library has been deployed in production at Perplexity AI for disaggregated prefill-decode, RL weight transfer, and MoE dispatch. dmaplane shares some of the architectural assumptions — specifically immediate-value-based chunk identification and credit-based flow control — but operates at the kernel verbs layer rather than through a userspace library, and focuses on demonstrating the data path from first principles rather than providing a production-ready transport abstraction.

**vLLM** [4] and **SGLang** [5] implement KV-cache management within their serving frameworks (PagedAttention, RadixAttention) but treat inter-host transfer as a future extension rather than a current primitive. Their KV-cache pools are designed for single-host reuse, not cross-host streaming.

**NVIDIA GPUDirect RDMA** via `nvidia-peermem` provides the kernel-level plumbing for NIC-to-GPU DMA on supported hardware. dmaplane reimplements this path from first principles and allows `nvidia_p2p_get_pages`, SG table construction, and MR registration to operate on any RDMA transport, including soft-RoCE on commodity Ethernet.

---

## 3. System Design

### 3.1 Architecture Overview

The system consists of a Linux kernel module (dmaplane), a C-level chunked transfer protocol, and a Python inference layer. The kernel module exposes a character device (`/dev/dmaplane`) controlled via `ioctl`, providing buffer allocation, MR registration, GPU pinning, RDMA WRITEIMM, and CQ polling as individual system calls.

```
  Prefill Host (Machine A)              Decode Host (Machine B)

  ┌──────────────────────┐              ┌──────────────────────┐
  │  PyTorch: model()    │              │  PyTorch: model()    │
  │  → KVCache tensors   │              │  with past_key_values│
  ├──────────────────────┤              ├──────────────────────┤
  │  consolidate_kvcache │              │  reconstruct via     │
  │  copy_() into staging│              │  torch.as_strided    │
  │  cuda.synchronize()  │              │  (0 µs, views only)  │
  ├──────────────────────┤              ├──────────────────────┤
  │  dmaplane_py.py      │              │  dmaplane_py.py      │
  │  write_imm() ×N      │              │  poll_recv() ×N      │
  ├──────────────────────┤              ├──────────────────────┤
  │  /dev/dmaplane       │              │  /dev/dmaplane       │
  │  ioctl: WRITE_IMM    │              │  ioctl: POLL_RECV    │
  ├──────────────────────┤              ├──────────────────────┤
  │  rdma_engine.c       │   IB RC QP   │  rdma_engine.c       │
  │  ib_post_send(       │══════════════│  ib_poll_cq()        │
  │    WRITE_WITH_IMM)   │  rxe / ETH   │  → imm_data          │
  ├──────────────────────┤              ├──────────────────────┤
  │  GPU BAR MR          │              │  GPU BAR MR          │
  │  (pre-pinned VRAM)   │              │  (pre-pinned VRAM)   │
  └──────────────────────┘              └──────────────────────┘
```

The data path for each KV-cache chunk:

```
GPU VRAM → PCIe BAR → NIC → Ethernet → NIC → PCIe BAR → GPU VRAM
```

No host DRAM buffer appears in this path.

### 3.2 Kernel Module: dmaplane

dmaplane is a loadable kernel module that implements the InfiniBand verbs API from kernel space using `IB_POLL_DIRECT` (explicit CQ polling, no background callbacks). It maintains a protection domain, completion queues, and queue pairs internally, exposing operations to userspace through typed `ioctl` commands.

Three ioctls implement the WRITEIMM protocol:

| Ioctl | Cmd | Function |
|-------|-----|----------|
| `IOCTL_RDMA_WRITE_IMM` | 0x80 | Post `IB_WR_RDMA_WRITE_WITH_IMM`, poll send CQ |
| `IOCTL_RDMA_POST_RECV` | 0x81 | Post receive WR (no CQ poll) |
| `IOCTL_RDMA_POLL_RECV` | 0x82 | Poll receive CQ, return `imm_data` + `wc_flags` |

This three-ioctl design enables fine-grained userspace control over credit management. The backpressure test ("post 8 receives, send 16 writes, post 8 more receives") cannot be expressed with a single combined ioctl.

RDMA WRITE requires the sender to specify a remote address and remote key (`rkey`) for the destination buffer. Standard memory region registration via the protection domain's `local_dma_lkey` produces `rkey=0` — sufficient for source buffers that only need local read access, but unusable as a WRITE target. For destination MRs, dmaplane uses the fast-reg path: `ib_alloc_mr()` allocates an MR capable of remote access, `ib_map_mr_sg()` maps the buffer's scatter-gather table into it, and a `IB_WR_REG_MR` work request posted on the QP activates the registration with the requested access flags. The result is a valid `(rkey, base_address)` pair that the sender includes in every WRITEIMM work request to target the receiver's buffer. The registration WR is posted on the same QP that will later carry data and is polled synchronously before returning to userspace, ensuring the MR is active before any transfer begins.

### 3.3 Chunked Transfer Protocol

KV-cache is transferred as a sequence of fixed-size chunks (default: 1 MB), each carrying an immediate value that encodes the layer and chunk index:

```c
#define KVCACHE_IMM_ENCODE(layer, chunk) \
    (((uint32_t)(layer) << 16) | ((uint32_t)(chunk) & 0xFFFF))
```

The sender iterates over layers and chunks, posting one WRITEIMM per chunk. Each write targets a computed offset in the receiver's MR: `remote_addr + (chunk_global_index × chunk_size)`. The receiver polls its CQ, decodes the immediate value, and marks the corresponding entry in a per-layer completion bitmap.

Flow control uses a credit window. The receiver pre-posts *N* receive WRs before the transfer begins. Each WRITEIMM consumes one receive WR. When the sender's credit count reaches zero, it stalls and polls the receive CQ to drain completed chunks, then replenishes credits by posting fresh receive WRs. This prevents CQ overflow and provides implicit backpressure — the receiver controls the sender's injection rate through the number of outstanding receive WRs.

A sentinel value (`KVCACHE_SENTINEL = 0xFFFFFFFF`) signals end-of-transfer. The receiver exits its poll loop upon observing this immediate value.

### 3.4 Zero-Copy KV-Cache Serialization and Reconstruction

#### Sender: Consolidation

HuggingFace transformer models return KV-cache as a tuple of `(key, value)` tensor pairs, one per layer. Each tensor has shape `[batch, num_kv_heads, seq_len, head_dim]` in float16 but may be non-contiguous in GPU memory after attention reshaping.

The consolidation step packs all layers into a single pre-allocated GPU staging buffer:

```python
def consolidate_kvcache(layers, staging_buf):
    offset = 0
    manifest = []
    for i, (k, v) in enumerate(layers):
        k_flat = k.contiguous().view(-1).view(torch.uint8)
        v_flat = v.contiguous().view(-1).view(torch.uint8)
        staging_buf[offset:offset + k_flat.numel()].copy_(k_flat)
        # ... record offsets in manifest ...
        offset += k_flat.numel() + v_flat.numel()
    return offset, manifest
```

The `copy_()` packs all the K/V tensors sequentially into the pre-pinned staging buffer and executes as a GPU HBM-to-HBM (High Bandwidth Memory) transfer at internal memory bandwidth (~500 GB/s on A10G). For 18 MB of TinyLlama KV-cache, this completes in approximately 36 µs. Crucially, `torch.cat()` is avoided — it would allocate a new tensor at an unpredictable virtual address, defeating the pin-once pattern.

A `torch.cuda.synchronize()` fence is mandatory after consolidation and before the first WRITEIMM. The `copy_()` operation is asynchronous on the CUDA stream; without the fence, the NIC (or rxe's CPU-side `memcpy`) reads stale or partially-written data from the BAR. This produces silent data corruption — the receiver gets valid-looking tensors with slightly wrong values, and the model generates subtly incorrect tokens.

#### Receiver: as_strided Reconstruction

The receiver's GPU-backed MR contains the packed KV-cache bytes after WRITEIMM completes. Rather than copying this data into separate tensors, we create typed views:

```python
def reconstruct_kvcache_zero_copy(recv_buf, manifest):
    layers = []
    for entry in manifest:
        shape = entry["shape"]
        batch, heads, seq_len, head_dim = shape
        strides = (heads * seq_len * head_dim,
                   seq_len * head_dim, head_dim, 1)

        k_base = recv_buf[entry["k_offset"]:
                          entry["k_offset"] + entry["k_size"]]
        k = torch.as_strided(k_base.view(entry["dtype"]),
                             shape, strides)
        # ... same for v ...
        layers.append((k, v))
    return tuple(layers)
```

`torch.as_strided` creates a tensor header — shape, stride, and storage offset — that points into existing memory. No data is moved. The model's attention kernels read K and V values directly from the addresses where the NIC wrote them. Measured reconstruction time: < 0.01 ms across all tested configurations.

The stride computation must account for grouped query attention (GQA). In GQA models, `num_key_value_heads < num_attention_heads`, which changes the head dimension of the K/V tensors (for instance in Mistral-7B). The implementation reads `num_key_value_heads` from the model config and computes strides accordingly.

### 3.5 GPU Buffer Management

Both the sender's staging buffer and the receiver's landing zone are allocated once at startup and reused across all requests:

1. **Allocation.** Using `torch.empty(size, dtype=torch.uint8, device='cuda')` — not `torch.zeros`, which triggers a `cudaMemset` on the full buffer. The buffer is overwritten by consolidation (sender) or RDMA (receiver) before any read.

2. **Alignment.** NVIDIA's P2P API requires 64 KB alignment on both virtual address and size. PyTorch's CUDA allocator does not guarantee this. We over-allocate by one alignment unit and slice to the first aligned offset:

    ```python
    raw = torch.empty(aligned_size + 65536, dtype=torch.uint8, device='cuda')
    offset = (65536 - raw.data_ptr() % 65536) % 65536
    aligned = raw[offset:offset + aligned_size]
    aligned._raw_storage_ref = raw  # prevent garbage collection
    ```

    The `_raw_storage_ref` attribute prevents Python's garbage collector from freeing the backing allocation while the aligned slice is still referenced.

3. **Pinning.** `IOCTL_GPU_PIN` calls `nvidia_p2p_get_pages`, which pins the BAR pages and returns physical addresses. This is called once; subsequent WRITEIMM operations reuse the same MR handle. Per-request pinning would add 50–200 µs per `nvidia_p2p_get_pages` invocation — small in absolute terms, but relevant when the total per-request overhead budget for the zero-copy path is 36 µs.

---

## 4. Experimental Setup

### 4.1 Hardware

We deploy on two Amazon EC2 `g5.xlarge` instances:

| Component | Specification |
|-----------|--------------|
| GPU | NVIDIA A10G (24 GB GDDR6, ~24 GB BAR1) |
| vCPU | 4× AMD EPYC (2nd gen) |
| Memory | 16 GB DDR4 |
| Network | Up to 25 Gbps (ENA) |
| RDMA | Soft-RoCE (rdma_rxe) over Ethernet |
| Placement | Same VPC, same availability zone |

Both instances use soft-RoCE (`rdma_rxe`), which implements the RDMA protocol stack in software over the standard Ethernet NIC. This means all RDMA verbs are functional — QP state transitions, CQ polling, WRITEIMM — but the NIC's hardware DMA engine is not used for BAR reads. Instead, an rxe kernel thread performs `memcpy` through the write-combining BAR mapping. This is architecturally identical to the ConnectX hardware path; the only difference is whether the CPU or the NIC's DMA engine reads the BAR.

### 4.2 Software

| Component | Version |
|-----------|---------|
| Kernel module | dmaplane (custom, GPL-2.0) |
| RDMA stack | rdma-core / rdma_rxe |
| NVIDIA driver | 535+ (for `nvidia_p2p_get_pages`) |
| PyTorch | 2.x (float16 inference) |
| Model | TinyLlama-1.1B-Chat-v1.0 (22 layers, MHA) |

The kernel module resolves NVIDIA symbols at runtime via `symbol_get`, compiling without a build-time dependency on the proprietary driver.

### 4.3 Model

TinyLlama-1.1B-Chat is a 1.1B-parameter model with 22 transformer layers, 32 attention heads, and a hidden dimension of 2048. At a sequence length of 128 tokens in float16, the total KV-cache size is approximately 18 MB (22 layers × 2 × 32 heads × 128 positions × 64 head_dim × 2 bytes). At 2048 tokens, this grows to approximately 288 MB.

We use TinyLlama because its small weight footprint (~2.2 GB in float16) fits alongside a 512 MB RDMA staging buffer within the A10G's 24 GB VRAM, leaving ample headroom for KV-cache growth. The system architecture is model-agnostic; validation with Mistral-7B-v0.1 (32 layers, GQA with 8 KV heads) is a planned extension that exercises the GQA stride computation.

---

## 5. Pipeline

### 5.1 Data Flow

The end-to-end pipeline for a single inference request:

**Machine A (Prefill):**
1. Tokenize input prompt.
2. `model.forward(input_ids, use_cache=True)` → logits + KV-cache.
3. `extract_kvcache()`: pull `(K, V)` pairs, make contiguous.
4. `consolidate_kvcache()`: `copy_()` into pre-pinned staging buffer.
5. `torch.cuda.synchronize()`: fence before RDMA.
6. For each 1 MB chunk: `IOCTL_RDMA_WRITE_IMM` with `imm = KVCACHE_IMM_ENCODE(layer, chunk)`.
7. Send sentinel (`imm = 0xFFFFFFFF`).
8. TCP: send manifest (JSON) + first decoded token.

**Machine B (Decode):**
1. Pre-post receive WRs (credit window).
2. Poll CQ: for each completion, decode `(layer, chunk)` from immediate value, mark in bitmap, replenish one receive WR.
3. On sentinel: verify all chunks received via bitmap.
4. `reconstruct_kvcache_zero_copy()`: create `as_strided` views into landing buffer.
5. Autoregressive decode loop: `model.forward(next_token, past_key_values=reconstructed_kv)` until EOS or max tokens.

### 5.2 Expected Timing Breakdown

Based on projected measurements for TinyLlama-1.1B at 128 tokens on g5.xlarge with soft-RoCE:

| Phase | Time | Notes |
|-------|------|-------|
| Tokenization | ~1 ms | Negligible |
| Prefill compute | ~45 ms | Full forward pass, 22 layers |
| KV consolidation | ~0.8 ms | GPU `copy_()` + `cuda.synchronize` |
| KV transfer (19 chunks) | ~52 ms | 18 MB over rxe at ~350 MB/s |
| KV reconstruction | < 0.01 ms | `as_strided` (pointer arithmetic) |
| Decode (per token) | ~22 ms | Single-token forward pass |
| **Time-to-first-token** | **~98 ms** | Prefill + consolidation + transfer |

The transfer time of 52 ms is dominated by rxe's software overhead and the Ethernet link, not by the GPU BAR. On hardware RDMA (ConnectX at 200 Gbps), the same 18 MB transfer would complete in under 1 ms, reducing TTFT to approximately 47 ms — dominated entirely by prefill compute.

The host-staging overhead comparison:

| Operation | Host-staged | GPU-direct |
|-----------|------------|------------|
| Sender consolidation | `cudaMemcpy D→H` (~3 ms) | `copy_()` on GPU (~36 µs) |
| Sender per-request pin | `nvidia_p2p_get_pages` (~100 µs) | Pre-pinned (0 µs) |
| Receiver reconstruction | `cudaMemcpy H→D` (~1.5 ms) | `as_strided` (0 µs) |
| **Total overhead** | **~4.6 ms** | **~0.04 ms** |

---

## 6. Implementation Details

### 6.1 Module Architecture

dmaplane is a single loadable kernel module (`dmaplane.ko`) that registers a character device at `/dev/dmaplane` with a `file_operations` table. Each `open()` creates a per-file context (`struct dmaplane_ctx`) that tracks all resources allocated through that file descriptor — buffers, memory regions, GPU pins, mmaps — in handle tables indexed by integer IDs. This per-fd scoping means multiple processes (or multiple opens within the same process) get isolated resource namespaces.

The ioctl dispatch table maps Linux ioctl command numbers to typed handlers. Each command number encodes the transfer direction (read, write, or both), the magic number (`0xE4`), the command ordinal, and the struct size in bits [29:16]. This encoding means a mismatch between the userspace struct layout and the kernel's expected layout produces `-ENOTTY` (wrong ioctl number) rather than silent data corruption — a fail-fast property that proved critical during Python wrapper development.

The UAPI header (`dmaplane_uapi.h`) defines all ioctl structs with fixed-width types (`__u32`, `__u64`, `__s32`) and no implicit padding. The Python wrapper (`dmaplane_py.py`) mirrors these structs using `struct.pack` with the `=` prefix (native byte order, standard sizes) and verifies sizes at import time:

```python
FMT_WRITE_IMM = "=IIQQIIIIQ"  # 48 bytes
assert struct.calcsize(FMT_WRITE_IMM) == 48
```

### 6.2 Buffer Management

Buffers are the fundamental resource in dmaplane. A buffer is a contiguous region of DMA-capable memory, tracked by a `struct dmaplane_buffer` that records the page array, size, NUMA node, allocation type, and reference count.

Host buffers are allocated via `alloc_pages_node` with an explicit NUMA node target. The kernel's page allocator treats this as a preference, not a guarantee — under memory pressure, pages may land on a different node silently. dmaplane verifies actual placement with `page_to_nid()` and reports the actual node back to userspace in the ioctl response, so the caller can detect cross-node allocation:

```c
page = alloc_pages_node(target_node, GFP_KERNEL, order);
actual_node = page_to_nid(page);
/* returned to userspace in buf_params.actual_numa_node */
```

This matters because a buffer allocated on the wrong NUMA node — on the opposite socket from the NIC that will DMA from it — pays a 40–50% throughput penalty on every transfer. The penalty is invisible in error logs; only measurement reveals it.

Each buffer supports `mmap` into userspace via a custom `vm_operations_struct` that maps the buffer's physical pages with appropriate caching attributes. The mmap is tracked in the per-fd context, and `destroy_buffer` returns `-EBUSY` if an mmap or MR still holds a reference. This enforced ordering prevents use-after-free at the kernel level — a class of bug that produces silent data corruption in systems that rely on userspace to manage resource lifetimes.

### 6.3 RDMA Engine

The RDMA engine creates and manages InfiniBand resources through the kernel verbs API. At `IOCTL_SETUP_RDMA`, the module:

1. Looks up the named IB device (e.g., `rxe_eth0`) via `ib_device_get_by_name`
2. Allocates a protection domain (`ib_alloc_pd`)
3. Creates send and receive completion queues (`ib_alloc_cq` with `IB_POLL_DIRECT` — explicit polling, no background callbacks or IRQs)
4. Creates a QP pair for loopback: QP-A for sends, QP-B for receives, connected to each other

QP state transitions follow the standard RC path: `RESET → INIT → RTR → RTS` via `ib_modify_qp`. The GID selection logic scans the device's GID table for a RoCEv2 entry (type `IB_GID_TYPE_ROCE_UDP_ENCAP`) rather than using index 0 — a lesson from early development where GID index 0 pointed to a link-local IPv6 address that rxe accepted without error but that produced zero completions on the wire.

CQ polling is synchronous: each ioctl that posts a work request also polls the relevant CQ before returning to userspace. `IOCTL_RDMA_WRITE_IMM` posts the send WR, then polls the send CQ with a timeout. `IOCTL_RDMA_POLL_RECV` polls the recv CQ and returns the completion's `wc_flags`, `byte_len`, and `imm_data`. This synchronous model is simpler than callback-driven completion and sufficient for the per-request, single-QP usage pattern. It would not scale to thousands of concurrent operations — but that is the transport library's job, not the buffer orchestration layer's.

#### MR Registration: Two Paths

MR registration supports two paths, selected by the access flags in the ioctl request:

**Local-access MRs** (source buffers): When only `IB_ACCESS_LOCAL_WRITE` is requested, the buffer is registered using the PD's `local_dma_lkey`. This is fast — no work request needed — and produces `rkey=0`, which is sufficient for source buffers that the local HCA reads from.

**Remote-write MRs** (RDMA WRITE destinations): When `IB_ACCESS_REMOTE_WRITE` is requested, dmaplane uses the fast-reg path. This is the same mechanism that NVMe-oF and iSER use for remote-access memory regions:

```c
mr = ib_alloc_mr(pd, IB_MR_TYPE_MEM_REG, num_pages);
ib_map_mr_sg(mr, sgt->sgl, sgt->nents, NULL, PAGE_SIZE);
/* Post IB_WR_REG_MR on QP, poll CQ synchronously */
```

The `IB_WR_REG_MR` work request is posted on the same QP that will later carry data. The CQ is polled synchronously before the ioctl returns, ensuring the MR is active — with a valid `(rkey, base_address)` — before any transfer begins. The `rkey` and `base_address` are returned to userspace in the ioctl response, and the sender includes them in every WRITEIMM work request to target the receiver's buffer.

### 6.4 GPU P2P Integration

GPU support is conditionally compiled via `CONFIG_DMAPLANE_GPU`. When enabled, the module resolves NVIDIA's peer-to-peer symbols at load time using the kernel's `symbol_get` / `symbol_put` mechanism:

```c
p2p_get_pages = symbol_get(nvidia_p2p_get_pages);
p2p_put_pages = symbol_get(nvidia_p2p_put_pages);
p2p_free_page_table = symbol_get(nvidia_p2p_free_page_table);
```

This avoids a compile-time dependency on the proprietary NVIDIA driver. The module builds and loads on systems without NVIDIA headers; the GPU ioctls return `-ENODEV` if the symbols are unavailable. On systems with the driver, the symbols resolve at `module_init` and GPU features activate transparently.

**Pinning.** `IOCTL_GPU_PIN` takes a CUDA virtual address (64 KB-aligned) and size, calls `nvidia_p2p_get_pages`, and receives an array of BAR1 page descriptors — physical addresses of 64 KB pages in the GPU's PCIe BAR aperture. dmaplane checks contiguity: if all pages are physically adjacent in BAR space, it creates a single `ioremap_wc` mapping spanning the entire buffer. This write-combining mapping is what makes rxe work — rxe's `memcpy` from the WC mapping coalesces CPU stores into 64-byte TLPs, achieving 10 GB/s for host-to-GPU writes (vs. 44 MB/s with uncacheable `ioremap`).

```c
for (i = 1; i < num_pages; i++) {
    phys_addr_t expected = pages[0]->physical_address
                           + i * GPU_PAGE_SIZE;
    if (pages[i]->physical_address != expected) {
        contiguous = false;
        break;
    }
}
if (contiguous)
    gpu_buf->rdma_vaddr = ioremap_wc(pages[0]->physical_address,
                                      num_pages * GPU_PAGE_SIZE);
```

**MR registration for GPU buffers.** The WC mapping provides a contiguous kernel virtual address that can be placed directly in an RDMA scatter-gather entry. When rxe processes a send work request with `sge.addr = rdma_vaddr`, it performs `memcpy(skb_data, rdma_vaddr, length)` — each byte becomes a CPU load from the WC BAR mapping, which becomes a PCIe read from GPU VRAM. On hardware NICs (ConnectX), the NIC's DMA engine reads the BAR physical addresses directly, bypassing the CPU.

**Unpin callback.** `nvidia_p2p_get_pages` takes a `free_callback` that fires if the GPU driver revokes the pages (e.g., `cudaFree` on the underlying allocation). The callback executes *while holding NVIDIA's internal locks*. Calling `nvidia_p2p_put_pages` from inside the callback deadlocks. Taking any lock that a thread calling into the NVIDIA driver might hold also deadlocks.

dmaplane's callback atomically sets a `revoked` flag and returns immediately:

```c
static void gpu_free_callback(void *data) {
    struct dmaplane_gpu_buffer *buf = data;
    atomic_set(&buf->revoked, 1);
}
```

Subsequent RDMA operations check the flag and fail with `-ENODEV`. Actual cleanup — releasing the page table, unmapping the BAR — happens lazily when the buffer's reference count drops to zero during normal teardown. This is the same pattern that `nvidia-peermem` uses, and the unpin callback is the number one source of deadlocks in third-party GPUDirect RDMA drivers that don't follow it.

### 6.5 dma-buf Integration

dmaplane implements the `dma_buf_ops` interface to export buffers as dma-buf file descriptors. This enables zero-copy sharing between devices — a buffer allocated in dmaplane can be imported by another subsystem (or another instance of dmaplane) without any memcpy.

The exporter provides `map_dma_buf` (builds an SG table from the buffer's pages), `unmap_dma_buf` (releases the SG mapping), and `mmap` (delegates to the buffer's page mapping). The dma-buf holds a reference on the underlying buffer, preventing `destroy_buffer` from freeing memory while an importer still has an attachment. This reference-counted lifecycle means buffer teardown is safe even when multiple devices share the same memory.

### 6.6 Resource Lifecycle and Error Handling

The kernel enforces strict resource ordering. Attempting to destroy a resource that has dependents returns `-EBUSY`:

```
Close mmaps         → before deregistering MRs
Deregister MRs      → before destroying buffers
GPU unpin           → before destroying buffers
Destroy buffers     → before RDMA teardown
Teardown RDMA       → before close()
```

On `close()` (fd released), the module walks the per-fd context and releases all resources in dependency order, logging warnings for any resources that userspace failed to clean up explicitly.

Error handling during transfer follows a drain pattern. If a WRITEIMM fails mid-transfer (e.g., chunk 64 of 128), outstanding recv completions remain in CQ-B. Calling `ib_destroy_qp` with undrained completions fails. dmaplane moves the QP to error state, which flushes all outstanding work requests as error completions, then drains the CQ before teardown:

```c
/* Move QP to error state — flushes outstanding WRs */
attr.qp_state = IB_QPS_ERR;
ib_modify_qp(qp, &attr, IB_QP_STATE);
/* Drain flush completions */
while (ib_poll_cq(recv_cq, 1, &wc) > 0)
    ; /* discard */
/* Now safe to destroy */
ib_destroy_qp(qp);
```

### 6.7 Python-Kernel Interface

The Python wrapper (`dmaplane_py.py`) provides zero-dependency access to all dmaplane ioctls using `struct.pack` and `fcntl.ioctl`. Each method packs a C struct into a `bytearray`, calls `fcntl.ioctl` (which mutates the bytearray in-place), and unpacks the result — mirroring what C code does with a stack-allocated struct and `ioctl(fd, cmd, &s)`.

Error reporting includes the ioctl name for debuggability:

```python
def _ioctl(self, name, cmd, buf):
    try:
        fcntl.ioctl(self.fd, cmd, buf)
    except OSError as e:
        raise OSError(e.errno,
                      f"dmaplane {name}: {os.strerror(e.errno)}") from e
```

The wrapper tracks mmaps in a per-client dictionary and closes them on `close()`, preventing the common leak where a Python process exits with an open mmap on a dmaplane buffer, which would block the buffer's kernel-side destruction.

GPU tensor alignment for NVIDIA's P2P API is handled by `allocate_aligned_gpu_tensor`, which over-allocates by one 64 KB alignment unit and slices to the first aligned offset. A `_raw_storage_ref` attribute on the aligned tensor prevents Python's garbage collector from freeing the backing allocation while the slice is still referenced — the Python equivalent of a dangling pointer that manifests as silent data corruption when PyTorch's caching allocator reuses the freed memory for an unrelated tensor.

### 6.8 Credit Window Dynamics

The credit window governs the maximum number of WRITEIMM operations in flight before the sender must poll for completions. Its effect on throughput is non-linear.

A credit window of 2–4 causes the sender to stall after every few chunks, polling and replenishing before the next batch. This serializes the pipeline: the sender waits for the receiver's CQ completion before issuing the next WRITEIMM. At 1 MB chunks, this limits throughput to roughly one chunk per round-trip time.

A credit window of 16 allows the sender to fill the pipeline: while the NIC (or rxe) processes chunk *N*, chunks *N+1* through *N+15* are queued in the send queue. The receiver processes completions as they arrive and replenishes receive WRs in a sliding window. The sender only stalls if it outpaces the receiver by more than 16 chunks — unlikely at Ethernet speeds.

A credit window exceeding 128 risks CQ overflow if the receiver falls behind. CQ overflow is silent and unrecoverable in most RDMA implementations — completed entries are dropped, and the QP transitions to error state. This is a lesson from dmaplane's earlier development phases, where a streaming benchmark with insufficient CQ depth produced intermittent failures that manifested only after warmup.

---

## 7. Relationship to Production Systems

dmaplane and production transport libraries like Perplexity's TransferEngine [6] and Mooncake [3] operate at different layers of the same stack. The distinction is not quality or completeness — it is scope.

```
┌─────────────────────────────────────────────────────┐
│  Serving Framework                                   │
│  (scheduler, batching, prefix caching, routing)      │
├─────────────────────────────────────────────────────┤
│  Transport Library              ← TransferEngine [6] │
│  (connection mgmt, multi-NIC    ← Mooncake [3]       │
│   sharding, ImmCounter,                              │
│   UVM watchers, work request                         │
│   templating, NIC portability)                       │
├─────────────────────────────────────────────────────┤
│  Buffer Orchestration           ← dmaplane           │
│  (GPU pin, NUMA alloc, SG table,                     │
│   MR registration, dma-buf,                          │
│   credit flow control, BAR map)                      │
├─────────────────────────────────────────────────────┤
│  Kernel Verbs / NIC Hardware                         │
│  (ib_post_send, ib_poll_cq, ConnectX, EFA, rxe)     │
└─────────────────────────────────────────────────────┘
```

**What dmaplane provides:**
- Kernel-owned buffer lifecycle across device boundaries — GPU VRAM, host DRAM, and RDMA MRs managed as a single resource graph, with enforced teardown ordering and reference-counted cleanup.
- GPU memory visible to the RDMA subsystem — `nvidia_p2p_get_pages` → `ioremap_wc` → SG table → MR, with the unpin callback handled safely under NVIDIA's driver locks.
- Flow control at the completion queue level — credit-windowed WRITEIMM that prevents CQ overflow without any userspace polling or out-of-band negotiation.

**What dmaplane does not provide:**
- Multi-NIC sharding, connection management, or NIC portability across vendors — TransferEngine's core value proposition.
- GPU-to-CPU signaling for pipelined transfer (UVM watchers) — dmaplane uses a coarse `cuda.synchronize()` fence.
- Work request templating, doorbell batching, or relaxed PCIe ordering — the per-operation optimizations that TransferEngine uses to saturate 400 Gbps on ConnectX-7 and EFA.

In production, these layers compose. A system like TransferEngine assumes that someone has already pinned the GPU buffer, built the SG table, registered the MR, and set up the access flags. That "someone" is `nvidia-peermem` in NVIDIA's stack, or custom per-application code in most deployments. dmaplane makes this layer explicit, reusable, and observable — with every operation exposed as a typed ioctl and every resource tracked in the kernel's device model.

The code-level differences between dmaplane and these production systems are instructive. Both Mooncake and Perplexity's TransferEngine build on the same RDMA verbs primitives — `ibv_post_send` with write-with-immediate, memory region registration, completion queue polling. The key architectural divergence is in transport abstraction: Perplexity's TransferEngine targets the intersection of RC and SRD guarantees (reliable but unordered delivery), making the same code portable across ConnectX and EFA hardware. dmaplane's WRITEIMM protocol — with per-chunk immediate values and bitmap tracking on the receiver — is naturally compatible with this unordered model, though currently validated only on ordered RC transport via soft-RoCE.

Perplexity's UVM watcher mechanism represents a more sophisticated approach to the GPU-CPU synchronization problem. Where dmaplane uses a coarse `cuda.synchronize()` fence after consolidating the entire KV-cache, UVM watchers enable layer-by-layer transfer: each transformer layer increments a unified memory counter upon completion, triggering the CPU to initiate RDMA for that layer's KV-cache while subsequent layers continue computing on the GPU. This pipelining of compute and transfer can hide transfer latency behind prefill compute — a significant advantage for long-context workloads where KV-cache sizes reach multiple gigabytes.

---

## 8. Limitations and Future Work

**Soft-RoCE overhead.** The rxe implementation processes RDMA operations in a kernel thread using CPU `memcpy` through the BAR mapping. For GPU-to-host reads, this achieves approximately 107 MB/s — two orders of magnitude slower than hardware DMA. The transfer time in our experiments is dominated by this software path, not the network. Hardware RDMA (ConnectX, EFA) would reduce the 52 ms transfer to under 1 ms for 18 MB.

**Single-request pipeline.** The current implementation serves one request at a time. Production systems batch multiple requests and pipeline KV-cache transfers with decode computation. Perplexity's UVM watcher approach [6] — where layer-by-layer RDMA transfers overlap with ongoing GPU compute — represents the natural evolution of dmaplane's coarse synchronize-then-transfer model. Extending dmaplane to support concurrent transfers would require multi-QP management, per-request buffer tracking, and a GPU-CPU signaling mechanism analogous to UVM watchers.

**Model-specific serialization.** The `consolidate_kvcache()` and `reconstruct_kvcache_zero_copy()` functions assume the HuggingFace KV-cache format (tuple of `(K, V)` pairs in `[batch, heads, seq, dim]` layout). Other frameworks (vLLM's PagedAttention, TensorRT-LLM) use different KV-cache organizations. The RDMA transfer layer is format-agnostic — only the Python serialization functions would change.

**Unordered transport compatibility.** The WRITEIMM immediate field encodes `(layer, chunk)` and the receiver tracks completion via a bitmap. This design is compatible with unordered transports (AWS EFA/SRD, multi-path RoCE), where chunks may arrive out of order. Perplexity's TransferEngine [6] validates this approach at scale: their ImmCounter primitive provides order-independent completion notification across both RC and SRD transports, confirming that immediate-value-based protocols are the correct abstraction for cross-provider RDMA. dmaplane's current RC transport guarantees ordering, but the protocol does not depend on it.

---

## 9. Conclusion

We have presented dmaplane, a general-purpose buffer orchestration layer that manages the lifecycle of DMA-mapped memory across GPU VRAM, host DRAM, and RDMA NICs. The module provides the complete set of primitives — NUMA-aware allocation, GPU BAR pinning via `nvidia_p2p_get_pages`, scatter-gather table construction, dma-buf export/import for zero-copy device sharing, RDMA memory region registration (both local-key and fast-reg paths), and credit-based flow control — all the underlying setup that high-performance AI data movement requires but that no existing framework exposes as a reusable, transport-agnostic layer.

To validate these primitives end-to-end, we built a zero-copy disaggregated KV-cache pipeline on top of dmaplane. By operating at the Linux kernel's RDMA verbs layer — `ib_post_send` with `IB_WR_RDMA_WRITE_WITH_IMM` for chunked streaming, pre-pinned GPU staging buffers for consolidation at HBM bandwidth, and `torch.as_strided` for zero-copy reconstruction on the receiver — the system eliminates all host DRAM staging and achieves per-request copy overhead of approximately 36 µs for 18 MB of KV-cache. The 32-bit immediate field provides per-chunk layer and chunk identification, making the protocol compatible with unordered transports without depending on message sequencing.

The architecture is validated end-to-end on two EC2 g5.xlarge instances with soft-RoCE, producing correct tokens from a TinyLlama-1.1B model where the prefill and decode phases execute on separate GPUs connected only by commodity Ethernet. The kernel APIs, the data path, and the flow-control protocol are identical to those used by production systems like Mooncake and Perplexity's TransferEngine [6] — with the only differences being link speed and NIC hardware.

dmaplane's buffer orchestration primitives — GPU pinning, NUMA-aware allocation, SG table construction, MR registration, dma-buf sharing — operate against the kernel's generic `ib_device` interface and are transport-agnostic. On ConnectX-7 with RC transport, the module would operate without code changes at 400 Gbps, since ConnectX-7 exposes the same `ib_post_send` and `ib_poll_cq` verbs that rxe implements in software. Porting to EFA/SRD would require a new transport backend in the RDMA engine (different connection model, different verbs API), but the buffer orchestration layer would remain unchanged.

The disaggregated KV-cache pipeline is dmaplane's first application, not its only one. The same buffer orchestration primitives apply directly to MoE expert-parallel dispatch (variable-sized scatter to per-expert buffers), RL weight synchronization (one-sided broadcast of sparse parameter deltas), and any workload where tensors must cross device or host boundaries without unnecessary copies. The framework is workload-agnostic; only the serialization format and transfer pattern change.

The implementation — approximately 6,000 lines of kernel C and 700 lines of Python — runs as a loadable kernel module on a stock Ubuntu kernel, requires no vendor SDK beyond the NVIDIA kernel headers, and degrades gracefully on systems without GPU hardware. The complete source code — kernel module, RDMA engine, GPU P2P integration, chunked transfer protocol, Python wrapper, and inference pipeline — is open source under GPL-2.0 at [github.com/marcoeg/dmaplane](https://github.com/marcoeg/dmaplane).

---

## References

[1] Patel, P., et al. "Splitwise: Efficient Generative LLM Inference Using Phase Splitting." *ISCA 2024*.

[2] Zhong, Y., et al. "DistServe: Disaggregating Prefill and Decoding for Goodput-optimized Large Language Model Serving." *OSDI 2024*.

[3] Qin, R., et al. "Mooncake: A KVCache-centric Disaggregated Architecture for LLM Serving." 2024.

[4] Kwon, W., et al. "Efficient Memory Management for Large Language Model Serving with PagedAttention." *SOSP 2023*.

[5] Zheng, L., et al. "SGLang: Efficient Execution of Structured Language Model Programs." 2023.

[6] Licker, N., Hu, K., Zaytsev, V., and Chen, L. "RDMA Point-to-Point Communication for LLM Systems." *arXiv:2510.27656*, Perplexity AI, October 2025. [arxiv.org/abs/2510.27656](https://arxiv.org/abs/2510.27656). Code: [github.com/perplexityai/pplx-garden](https://github.com/perplexityai/pplx-garden).

---

## Appendix A: Glossary of Acronyms

| Acronym | Definition |
|---------|-----------|
| ALU | Arithmetic Logic Unit |
| BAR | Base Address Register (PCIe mechanism for exposing device memory to the host) |
| CQ | Completion Queue (RDMA structure where completed operations are reported) |
| CQE | Completion Queue Entry |
| CUDA | Compute Unified Device Architecture (NVIDIA's GPU programming framework) |
| DMA | Direct Memory Access |
| EFA | Elastic Fabric Adapter (AWS proprietary network interface) |
| EOS | End of Sequence (token signaling generation termination) |
| FLOPS | Floating Point Operations Per Second |
| GDRCopy | GPU Direct RDMA Copy (low-latency GPU memory access from CPU) |
| GPL | GNU General Public License |
| GQA | Grouped Query Attention (attention variant with fewer KV heads than query heads) |
| HBM | High Bandwidth Memory (stacked DRAM on the GPU package) |
| HCA | Host Channel Adapter (RDMA network interface) |
| IOMMU | Input/Output Memory Management Unit |
| iSER | iSCSI Extensions for RDMA (protocol for running iSCSI commands over RDMA) |
| KV-cache | Key-Value Cache (attention state stored across decoding steps) |
| LLM | Large Language Model |
| MHA | Multi-Head Attention |
| MMIO | Memory-Mapped I/O |
| MoE | Mixture of Experts |
| MR | Memory Region (RDMA-registered memory buffer) |
| NIC | Network Interface Controller |
| NUMA | Non-Uniform Memory Access |
| NVMe-oF | NVM Express over Fabrics (protocol for accessing NVMe storage over RDMA networks) |
| PD | Protection Domain (RDMA security boundary for MRs and QPs) |
| QP | Queue Pair (RDMA send/receive endpoint) |
| RC | Reliable Connection (InfiniBand transport guaranteeing ordered delivery) |
| RDMA | Remote Direct Memory Access |
| RL | Reinforcement Learning |
| RNR | Receiver Not Ready (RDMA NAK when no receive WR is posted) |
| rkey | Remote Key (RDMA credential authorizing remote memory access) |
| rxe | RDMA over Converged Ethernet — software implementation (Soft-RoCE) |
| SG | Scatter-Gather (list of memory segments for DMA operations) |
| SM | Streaming Multiprocessor (GPU compute unit) |
| SRD | Scalable Reliable Datagram (AWS EFA transport, reliable but unordered) |
| TLP | Transaction Layer Packet (PCIe protocol data unit) |
| TTFT | Time to First Token |
| UC | Uncacheable (CPU memory mapping type — no write combining) |
| UAPI | User Application Programming Interface (kernel headers exposed to userspace) |
| UVM | Unified Virtual Memory |
| VA | Virtual Address |
| VPC | Virtual Private Cloud |
| VRAM | Video Random Access Memory (GPU memory) |
| WC | Write-Combining (CPU memory mapping type that coalesces stores) |
| WR | Work Request (RDMA operation submitted to a QP) |
| WRITEIMM | RDMA WRITE with Immediate Data (`IB_WR_RDMA_WRITE_WITH_IMM`) |
