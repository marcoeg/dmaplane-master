# Kernel-Space RDMA from First Principles

*Part 4 of 9 in a series on building a host-side data path emulator for AI infrastructure*

---

Phase 3 established the sharing primitive: dma-buf wraps a buffer's pages, hands out file descriptors, and lets multiple devices map the same physical memory through their own IOMMU contexts. But sharing memory across devices on the same machine is only half the problem. Disaggregated inference, gradient aggregation, and KV-cache transfer all require moving buffer contents between machines at wire speed, without copies. The kernel subsystem for this is RDMA — Remote Direct Memory Access — and it operates through the InfiniBand Verbs API.

**The kernel module must own the RDMA resources because it owns the pages.** Userspace RDMA libraries (`libibverbs`, `ibv_reg_mr`) work by pinning userspace pages and registering them with the NIC. But dmaplane's pages are kernel-allocated (`alloc_pages` in Phase 2), kernel-mapped (`vmap`), and kernel-managed (the buffer lifecycle, the dma-buf export). Userspace `ibv_reg_mr` cannot register kernel pages — it has no handle to them. The kernel must call `ib_dma_map_sg` itself to program the IOMMU, and `pd->local_dma_lkey` to authorize the NIC's DMA engine. This is the same position that `nvidia-peermem` is in: it owns GPU BAR pages and must register them from kernel space because no userspace API can reach them.

This post covers the full RDMA implementation: the resource hierarchy and why each layer depends on the previous, GID selection on Ubuntu with stable-privacy addressing, the `IB_POLL_DIRECT` completion model and why it prevents teardown races, two paths for Memory Region registration, the loopback queue pair (QP) and its state machine, benchmarks over Soft-RoCE, and the teardown discipline that prevents use-after-free.

---

## The Resource Hierarchy: IB Device to Memory Region

**Every RDMA operation depends on a chain of five resources, each created from the previous: IB device, Protection Domain (PD), Completion Queue (CQ), Queue Pair (QP), and Memory Region (MR).** Destroying any resource while a downstream resource still references it is a use-after-free. Creating them out of order is impossible — the API enforces the dependency.

```
IB device (ib_device_get_by_name)
  └── Protection Domain (ib_alloc_pd)
        ├── CQ-A (ib_alloc_cq)
        ├── CQ-B (ib_alloc_cq)
        ├── QP-A (ib_create_qp) → bound to CQ-A
        ├── QP-B (ib_create_qp) → bound to CQ-B
        └── MR (ib_alloc_mr or pd->local_dma_lkey)
```

The **IB device** is the starting point. `ib_device_get_by_name("rxe_eth0", RDMA_DRIVER_UNKNOWN)` returns a reference-counted handle to the Soft-RoCE provider. The name comes from userspace via the `IOCTL_SETUP_RDMA` parameter struct, and the test auto-detects it by scanning `/sys/class/infiniband/` for entries starting with `rxe_`.

The **Protection Domain** is the root of the resource hierarchy. All CQs, QPs, and MRs created under a PD share access permissions and are isolated from resources in other PDs. `ib_alloc_pd` allocates one. The PD also provides `local_dma_lkey` — a per-PD shortcut that authorizes the local NIC to DMA pages without a full MR (more on this in the MR section).

**Completion Queues** collect work completion entries. Each QP is bound to a CQ at creation time. The loopback pair uses two CQs — one for each QP — so send completions on QP-A and recv completions on QP-B never interleave. This eliminates the need for completion routing logic when polling.

**Queue Pairs** carry the actual traffic. dmaplane uses Reliable Connected (RC) QPs — the same transport type that NCCL uses for GPU-to-GPU gradient aggregation. RC guarantees in-order, reliable delivery. `IB_SIGNAL_ALL_WR` is set so every work request generates a completion, simplifying the polling model.

**Memory Regions** authorize the NIC to access specific pages. Without an MR (or `local_dma_lkey`), the NIC has no permission to read or write any memory — posting a work request with an unregistered address returns a completion error.

The setup function creates these resources in dependency order, and every error path tears down only what was successfully created:

```c
ctx->pd = ib_alloc_pd(ctx->ib_dev, 0);
if (IS_ERR(ctx->pd)) {
    ret = PTR_ERR(ctx->pd);
    ctx->pd = NULL;
    goto err_put_dev;
}

ctx->cq_a = ib_alloc_cq(ctx->ib_dev, NULL, cq_depth,
                         0, IB_POLL_DIRECT);
if (IS_ERR(ctx->cq_a)) {
    ret = PTR_ERR(ctx->cq_a);
    ctx->cq_a = NULL;
    goto err_dealloc_pd;
}
/* ... QP creation follows ... */
```

The goto chain — `err_destroy_qp_b`, `err_destroy_qp_a`, `err_destroy_cq_b`, `err_destroy_cq_a`, `err_dealloc_pd`, `err_put_dev` — mirrors the creation order exactly reversed. This is the same pattern from Phase 1's character device registration, applied to a deeper resource tree.

---

## GID Selection: Why Index Zero Fails on Ubuntu

**Soft-RoCE exposes multiple GID entries, and the default (index 0) is often unusable.** All entries are typed `ROCE_UDP_ENCAP` (RoCEv2), but GID[0] is typically the EUI-64 MAC-derived link-local address — a "ghost" address that does not exist on the interface when Ubuntu's stable-privacy IPv6 addressing is active. Using GID[0] causes QP state transitions to succeed, work requests to post without error, and all traffic to fail silently. No error code. No completion error. The packets simply never arrive.

The stable-privacy addressing mechanism (RFC 7217) replaces the MAC-based interface identifier with a hash derived from the interface name, a secret key, and a network prefix. The result is a link-local address that has no relation to the MAC address. The NIC's GID table still contains the old MAC-derived entry at index 0, but the network stack does not recognize it — packets sourced from that address are dropped.

The `scan_gid_table` function iterates up to 16 GID entries and selects the last link-local `ROCE_UDP_ENCAP` entry, which is empirically the real interface address:

```c
for (idx = 0; idx < 16; idx++) {
    attr = rdma_get_gid_attr(ctx->ib_dev, ctx->port, idx);
    if (IS_ERR(attr))
        break;

    if (attr->gid_type == IB_GID_TYPE_ROCE_UDP_ENCAP &&
        !ipv6_addr_any((struct in6_addr *)attr->gid.raw) &&
        ipv6_addr_type((struct in6_addr *)attr->gid.raw) &
                       IPV6_ADDR_LINKLOCAL) {
        last_linklocal = idx;
    }
    rdma_put_gid_attr(attr);
}
```

If no suitable entry is found, the function falls back to index 0 with a warning. On a properly configured system, the selected index is typically 1 or 2. The choice is logged via `pr_debug` so `dmesg` shows exactly which GID was selected and why.

---

## IB_POLL_DIRECT: Deterministic Completion Processing

**CQs are created with `IB_POLL_DIRECT` to eliminate asynchronous completion processing.** The three CQ polling modes in the kernel verbs API are `IB_POLL_SOFTIRQ`, `IB_POLL_WORKQUEUE`, and `IB_POLL_DIRECT`. The first two delegate completion processing to the kernel's softirq or workqueue subsystems — the completion callback fires asynchronously, potentially on a different CPU, at an unpredictable time relative to the caller.

The problem with asynchronous callbacks is teardown. When the module calls `ib_free_cq` to destroy a CQ, any in-flight callback holds a reference to the CQ's memory. If the callback fires after the CQ is freed, it dereferences freed memory. The standard mitigation — drain the CQ before destroying it — requires `ib_drain_cq`, which waits for all outstanding work to complete. But on a module that is unloading, "outstanding work" may include callbacks that reference module code that is about to be unmapped.

`IB_POLL_DIRECT` avoids the entire problem. Completions are retrieved only when the caller explicitly calls `ib_poll_cq`. No background thread, no softirq, no workqueue. The `rdma_engine_poll_cq` function busy-polls with a wall-clock timeout:

```c
int rdma_engine_poll_cq(struct ib_cq *cq, struct ib_wc *wc, int timeout_ms)
{
    ktime_t deadline = ktime_add_ms(ktime_get(), timeout_ms);
    int ret;

    do {
        ret = ib_poll_cq(cq, 1, wc);
        if (ret > 0)
            return 1;
        if (ret < 0)
            return ret;
        cond_resched();
        usleep_range(50, 200);
    } while (ktime_before(ktime_get(), deadline));

    return 0;  /* timeout */
}
```

The trade-off is that callers must actively poll — there is no notification when a completion arrives. For a benchmark driver this is acceptable. For a production data path, event-driven completion (`IB_POLL_SOFTIRQ`) with careful teardown ordering would be preferred. The `usleep_range(50, 200)` between polls yields the CPU and avoids burning an entire core on a spin loop.

The `ib_alloc_cq` API contract requires that every `wr_cqe->done` function pointer is valid, even with `IB_POLL_DIRECT`. The `poll_cq_done` callback exists to satisfy this contract. It is rarely invoked under normal operation, but it must exist — setting `wr_cqe->done = NULL` would cause a null pointer dereference if the managed CQ layer ever processes the completion internally.

---

## Memory Region Registration: Two Paths

**MR registration has two paths: `local_dma_lkey` for local-only access, and fast-reg MR for remote access.** The path is selected by the access flags passed in the ioctl.

When `access_flags` contains only `IB_ACCESS_LOCAL_WRITE`, the NIC needs permission to DMA local pages but no remote key is required. The PD's `local_dma_lkey` provides this — it is a pre-allocated key that authorizes any local DMA within the PD's scope. No `ib_alloc_mr`, no `IB_WR_REG_MR` work request, no CQ poll. The MR entry records `lkey = pd->local_dma_lkey`, `rkey = 0`, and `sge_addr` set to the buffer's kernel virtual address (which Soft-RoCE interprets directly via the `local_dma_lkey` path).

When access flags include `IB_ACCESS_REMOTE_WRITE` or `IB_ACCESS_REMOTE_READ`, a real rkey is needed so a remote QP can specify the target memory region. This requires the fast-reg path: `ib_alloc_mr` allocates an MR object, `ib_map_mr_sg` maps the scatter-gather list into it, and `IB_WR_REG_MR` is posted on QP-A to activate the registration:

```c
frmr = ib_alloc_mr(ctx->pd, IB_MR_TYPE_MEM_REG, nr_pages);
nr_mapped = ib_map_mr_sg(frmr, sgt->sgl, sgt->nents, NULL, PAGE_SIZE);

reg_wr.wr.opcode     = IB_WR_REG_MR;
reg_wr.wr.send_flags = IB_SEND_SIGNALED;
reg_wr.mr            = frmr;
reg_wr.key           = frmr->rkey;
reg_wr.access        = access_flags;

ib_post_send(ctx->qp_a, &reg_wr.wr, &bad_wr);
```

Both paths start with the same SG table construction. The `register_mr_build_sgt` function takes the buffer's page array, builds an `sg_table` with one entry per page via `sg_set_page`, then calls `ib_dma_map_sg` to program the IOMMU. This is the same `sg_set_page` call that dma-buf's `map_dma_buf` uses in Phase 3 — the page array is the shared currency between subsystems.

A critical detail: the page array is snapshotted under `buf_lock` into a `pages_copy` array before the lock is released. After releasing `buf_lock`, the buffer could be destroyed concurrently, freeing the `buf->pages` array. The snapshot decouples MR registration from buffer lifecycle — the `struct page` frames themselves are stable kernel objects, but the pointer array that holds them is owned by the buffer and freed on destroy.

---

## The Loopback Pair: QP State Machine and DMAC Resolution

**The loopback pair consists of QP-A (sender) and QP-B (receiver) connected to each other on the same machine.** This provides a self-contained benchmark target — the full data path (buffer, MR, post_send, wire, post_recv, completion) executes without a remote peer.

Connecting the pair requires driving each QP through the state machine: RESET, INIT, RTR (Ready to Receive), RTS (Ready to Send). Each transition is a call to `ib_modify_qp` with a specific attribute mask. The wrong mask — missing a required attribute or including an extra one — causes the transition to fail silently or return opaque errors on later operations.

**INIT** sets the port, partition key, and access flags. This is where `IB_ACCESS_REMOTE_WRITE | IB_ACCESS_REMOTE_READ` is configured:

```c
attr.qp_state = IB_QPS_INIT;
attr.pkey_index = 0;
attr.port_num = port;
attr.qp_access_flags = IB_ACCESS_LOCAL_WRITE |
                       IB_ACCESS_REMOTE_WRITE |
                       IB_ACCESS_REMOTE_READ;
```

**RTR** is the most complex transition. It sets the path MTU, destination QP number (the peer's QPN), receive PSN, and the Address Handle — which contains the GID, GID index, and destination MAC. For the loopback pair, QP-A's destination is QP-B's QPN, and vice versa.

**RoCEv2 requires the destination MAC address in the Address Handle.** With InfiniBand fabric, the subnet manager resolves addresses. With RoCE over Ethernet, the kernel verbs layer needs the Ethernet MAC. For loopback, the destination MAC is the interface's own MAC address, retrieved inside an RCU read section:

```c
rcu_read_lock();
ndev = rdma_read_gid_attr_ndev_rcu(sgid_attr);
if (!IS_ERR(ndev)) {
    memcpy(dmac_buf, ndev->dev_addr, ETH_ALEN);
    have_dmac = true;
}
rcu_read_unlock();
```

The `ndev` pointer is only valid inside the RCU read section — the network device could be unregistered concurrently. The MAC is copied into a local buffer before the RCU lock is released. Without this RCU discipline, the `memcpy` could read from a freed `net_device` structure.

**RTS** sets the send-side parameters: local ACK timeout, retry counts, send PSN. The timeout of `14` (4.096us * 2^14 = ~4 seconds) and retry count of 7 (IB spec maximum) are conservative defaults. `rnr_retry = 7` means infinite retries on Receiver Not Ready — the receiver will eventually post a recv buffer, and the sender should wait rather than fail.

---

## Benchmarks: Loopback, Ping-Pong, and Streaming

**Three benchmark patterns measure different aspects of the RDMA data path.** All three snapshot MR and buffer fields by value under their respective locks, then release the locks before performing I/O. This decouples benchmark execution from concurrent MR deregistration or buffer destruction.

**Loopback** is a single send/recv validation test. Post recv on QP-B, post send on QP-A, poll CQ-A for the send completion, poll CQ-B for the recv completion, measure total latency. The test writes `0xDEADBEEF` into the buffer before sending — this validates the data path works, though the loopback does not memcmp on receive (data integrity is verified by the RC transport's CRC).

**Ping-pong** measures round-trip latency over N iterations. Each iteration is one send/recv cycle with per-iteration timing. P99 latency is computed by sorting the latency array and indexing at position `(iterations * 99) / 100`. With 1000 iterations this gives true P99; with fewer iterations, a proportionally lower percentile.

**Streaming** measures throughput with pipelined sends. Multiple sends are posted back-to-back (up to `queue_depth` outstanding), with completions polled as they arrive. Receives are pre-posted up to 2x queue depth and replenished during the send/poll loop — this prevents RQ overflow regardless of iteration count. Both CQs are flushed before and after the benchmark to prevent stale completions from poisoning adjacent runs.

The streaming benchmark computes per-completion inter-arrival times for P99, rather than per-iteration round-trip times. This captures the jitter in the pipelined path — a high P99 relative to the average indicates head-of-line blocking or CQ contention.

### Results on Soft-RoCE (rxe)

Actual results from `test_phase4_rdma` on a workstation with Soft-RoCE (`rxe_enp0s31f6`):

| Benchmark | Message Size | Iterations | Queue Depth | Metric | Result |
|-----------|-------------|------------|-------------|--------|--------|
| Loopback | 64 B | 1 | -- | Latency | 205 us |
| Ping-pong | 4 KB | 1000 | -- | Avg latency | 200 us |
| Ping-pong | 4 KB | 1000 | -- | P99 latency | 211 us |
| Ping-pong | 4 KB | 1000 | -- | Throughput | 20 MB/s |
| Streaming | 4 KB | 1000 | 8 | Throughput | 163 MB/s |

These numbers are software-emulated RDMA — rxe processes everything in the kernel networking stack. On real InfiniBand hardware (ConnectX-6), loopback latency is under 2 microseconds, and streaming throughput exceeds 20 GB/s. The value of the rxe numbers is not absolute performance but relative consistency: zero completion errors across 2001 sends, 2001 receives, and 4002 completions polled, with clean teardown. The 8x throughput improvement from ping-pong (20 MB/s) to streaming (163 MB/s) demonstrates the pipeline amortization from queue depth.

---

## Teardown Discipline: ERR Transition Before Destroy

**Destroying a QP with outstanding work requests leaves the provider holding references to the caller's CQE and SGE memory.** The solution is to transition the QP to the error state before destroying it. `IB_QPS_ERR` causes the RDMA subsystem to flush all outstanding work requests — every pending send and recv is completed with `IB_WC_WR_FLUSH_ERR` status. After the flush, the provider holds no references, and `ib_destroy_qp` can proceed safely.

```c
struct ib_qp_attr attr = { .qp_state = IB_QPS_ERR };

/* Step 1: Move QPs to error state — flushes outstanding WRs */
if (ctx->qp_b)
    ib_modify_qp(ctx->qp_b, &attr, IB_QP_STATE);
if (ctx->qp_a)
    ib_modify_qp(ctx->qp_a, &attr, IB_QP_STATE);

/* Step 2: Destroy in strict reverse order of creation */
if (ctx->qp_b) { ib_destroy_qp(ctx->qp_b); ctx->qp_b = NULL; }
if (ctx->qp_a) { ib_destroy_qp(ctx->qp_a); ctx->qp_a = NULL; }
if (ctx->cq_b) { ib_free_cq(ctx->cq_b); ctx->cq_b = NULL; }
if (ctx->cq_a) { ib_free_cq(ctx->cq_a); ctx->cq_a = NULL; }
if (ctx->pd)   { ib_dealloc_pd(ctx->pd); ctx->pd = NULL; }
if (ctx->ib_dev) { ib_device_put(ctx->ib_dev); ctx->ib_dev = NULL; }
```

The destroy order is the exact reverse of creation: QP-B, QP-A, CQ-B, CQ-A, PD, IB device reference. Destroying a CQ before its bound QP would leave the QP pointing at freed memory. Deallocating the PD before destroying the QPs would orphan the QPs' protection context.

Because CQs use `IB_POLL_DIRECT`, there are no background workqueues or softirq handlers running against the CQ. Teardown is race-free without explicit CQ draining — a property that would not hold with `IB_POLL_SOFTIRQ`, where a softirq handler could fire between `ib_modify_qp(ERR)` and `ib_free_cq`.

The teardown ioctl also deregisters all outstanding MRs before touching the RDMA resources. Each MR holds an `sg_table` that was DMA-mapped via `ib_dma_map_sg` — destroying the IB device before unmapping these tables would leak IOMMU entries.

The locking for setup and teardown uses `rdma_sem` as a write lock (`down_write`), which excludes all readers (MR registration, benchmarks) that hold `rdma_sem` as a read lock (`down_read`). This ensures no benchmark is mid-execution when the RDMA resources are destroyed.

---

## Connection to Phase 3: Same Pages, Multiple Consumers

**The same `struct page **` array that Phase 3's `map_dma_buf` uses for SG table construction feeds directly into `ib_dma_map_sg` during MR registration.** The physical pages allocated in Phase 2 (`alloc_pages`) are the shared currency across the entire stack.

The chain in Phase 3 was: `alloc_page` produces `struct page *` entries, the page array is stored on the buffer, dma-buf export borrows the array and builds per-importer SG tables. Phase 4 extends the chain: MR registration builds its own SG table from the same page array and hands it to the NIC via `ib_dma_map_sg`. The result is a buffer whose pages are simultaneously accessible via userspace mmap (virtual addresses), dma-buf import (per-device IOMMU mappings), and RDMA MR (NIC DMA permissions).

Only page-backed buffers support MR registration, for the same reason only page-backed buffers support dma-buf export: the SG table construction requires `struct page *` pointers, which coherent allocations (`dma_alloc_coherent`) do not reliably provide. The registration path enforces this:

```c
if (buf->alloc_type != DMAPLANE_BUF_TYPE_PAGES ||
    !buf->pages || buf_nr_pages == 0) {
    mutex_unlock(&edev->buf_lock);
    return -EINVAL;
}
```

---

## Connection Forward: Peer QPs and GPU Memory

Phase 4 establishes the loopback pair — a self-contained benchmark tool. Phase 5 adds the dma-buf integration that proves zero-copy RDMA from shared buffers: dma-buf-backed pages registered as MRs, sent over the loopback pair, with no copies at any stage.

Later phases extend the RDMA engine in two directions. Cross-machine RDMA adds peer QPs — a second QP pair where each end lives on a different machine, connected over real Ethernet. The QP state machine and MR registration are identical; only the DMAC, GID, and QPN change from loopback values to the remote peer's values. GPU memory integration (Phase 8) registers GPU BAR pages as MRs — the same `ib_dma_map_sg` path, applied to pages obtained from `nvidia_p2p_get_pages` instead of `alloc_pages`. The RDMA engine does not change; the page source does.

---

*Next: [Part 5 -- dma-buf & Zero-Copy Sharing](/docs/blog_05_dmabuf_zero_copy.md)*
