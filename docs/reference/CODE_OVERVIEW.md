# Code Overview

Detailed analysis of every source file in the dmaplane codebase. This document grows with each phase — it describes only what has been implemented so far.

## Current State: Phase 5 — NUMA, Topology & Optimization

## Licensing

All `.c` and `.h` source files carry the `SPDX-License-Identifier: GPL-2.0` tag and a copyright header:

```
Copyright (c) 2026 Graziano Labs Corp.
```

Licensed under the GNU General Public License v2.

---

## Shared Headers

### `include/dmaplane_uapi.h`

The userspace-visible API header. This is the primary definition point for all types shared between kernel and userspace. Includable from both kernel (`__KERNEL__`, uses `<linux/types.h>`) and userspace (provides `typedef` mappings from `stdint.h` to `__u32`, `__u64`, etc.).

**Contents:**
- Module name constant: `DMAPLANE_NAME` (`"dmaplane"`)
- Ring buffer constants: `DMAPLANE_MAX_CHANNELS` (4), `DMAPLANE_RING_SIZE` (1024, power of 2)
- Ioctl magic: `DMAPLANE_IOC_MAGIC` (`0xE4`)
- Ring entry struct (`dmaplane_ring_entry`): `__u64 payload`, `__u32 flags`, `__u32 _pad` (explicit padding for natural alignment)
- Submit parameters (`dmaplane_submit_params`): wraps a single ring entry
- Complete parameters (`dmaplane_complete_params`): wraps a single ring entry
- Channel creation parameters (`dmaplane_channel_params`): `__u32 channel_id`, `__u32 _pad`
- Channel stats (`dmaplane_stats`): `__u64 total_submissions`, `__u64 total_completions`, `__u32 ring_high_watermark`, `__u32 dropped_count`
- Ioctl command defines: `DMAPLANE_IOCTL_CREATE_CHANNEL` (`_IOR`, 0x01), `DMAPLANE_IOCTL_SUBMIT` (`_IOW`, 0x02), `DMAPLANE_IOCTL_COMPLETE` (`_IOWR`, 0x03), `DMAPLANE_IOCTL_GET_STATS` (`_IOR`, 0x04)

**Phase 2 additions:**
- Buffer type constants: `DMAPLANE_BUF_TYPE_COHERENT` (0), `DMAPLANE_BUF_TYPE_PAGES` (1)
- Buffer creation parameters (`dmaplane_buf_params`): `__u32 buf_id` (out), `__u32 alloc_type` (in), `__u64 size` (in), `__s32 numa_node` (in, `-1` = DMAPLANE_NUMA_ANY), `__s32 actual_numa_node` (out)
- mmap info (`dmaplane_mmap_info`): `__u32 buf_id` (in), `__u64 mmap_offset` (out), `__u64 mmap_size` (out)
- Buffer stats (`dmaplane_buf_stats`): `__u64 buffers_created`, `__u64 buffers_destroyed`, `__u64 numa_local_allocs`, `__u64 numa_remote_allocs`, `__u64 numa_anon_allocs`
- Ioctl commands: `DMAPLANE_IOCTL_CREATE_BUFFER` (`_IOWR`, 0x05), `DMAPLANE_IOCTL_DESTROY_BUFFER` (`_IOW`, 0x06, bare `__u32`), `DMAPLANE_IOCTL_GET_MMAP_INFO` (`_IOWR`, 0x08), `DMAPLANE_IOCTL_GET_BUF_STATS` (`_IOR`, 0x09)

**Phase 3 additions:**
- Export parameters (`dmaplane_export_dmabuf_arg`): `__u32 buf_id` (in), `__s32 fd` (out)
- dma-buf stats (`dmaplane_dmabuf_stats`): 6 `__u64` counters — `dmabufs_exported`, `dmabufs_released`, `attachments_total`, `detachments_total`, `maps_total`, `unmaps_total`
- Ioctl commands: `DMAPLANE_IOCTL_EXPORT_DMABUF` (`_IOWR`, 0x0A), `DMAPLANE_IOCTL_GET_DMABUF_STATS` (`_IOR`, 0x0B)

**Phase 4 additions:**
- RDMA setup (`dmaplane_rdma_setup`): `char ib_dev_name[32]` (in), `__u32 port` (in), `__u32 cq_depth` (in, 0=128), `__u32 max_send_wr` (in, 0=64), `__u32 max_recv_wr` (in, 0=64), `__u32 status` (out)
- MR parameters (`dmaplane_mr_params`): `__u32 mr_id` (out), `__u32 buf_id` (in), `__u32 access_flags` (in), `__u32 lkey` (out), `__u32 rkey` (out), `__u64 addr` (out)
- Loopback parameters (`dmaplane_loopback_params`): `__u32 mr_id` (in), `__u32 size` (in), `__u32 status` (out), `__u64 latency_ns` (out)
- Benchmark parameters (`dmaplane_bench_params`): `__u32 mr_id` (in), `__u32 msg_size` (in), `__u32 iterations` (in), `__u32 queue_depth` (in), `__u64 total_ns` (out), `__u64 avg_latency_ns` (out), `__u64 p99_latency_ns` (out), `__u64 throughput_mbps` (out), `__u64 mr_reg_ns` (out)
- RDMA stats (`dmaplane_rdma_stats`): 8 `__u64` counters — `mrs_registered`, `mrs_deregistered`, `sends_posted`, `recvs_posted`, `completions_polled`, `completion_errors`, `bytes_sent`, `bytes_received`
- Ioctl commands: `DMAPLANE_IOCTL_SETUP_RDMA` (`_IOWR`, 0x10), `DMAPLANE_IOCTL_TEARDOWN_RDMA` (`_IO`, 0x11), `DMAPLANE_IOCTL_REGISTER_MR` (`_IOWR`, 0x20), `DMAPLANE_IOCTL_DEREGISTER_MR` (`_IOW`, 0x21, bare `__u32`), `DMAPLANE_IOCTL_LOOPBACK_TEST` (`_IOWR`, 0x30), `DMAPLANE_IOCTL_PINGPONG_BENCH` (`_IOWR`, 0x31), `DMAPLANE_IOCTL_STREAMING_BENCH` (`_IOWR`, 0x32), `DMAPLANE_IOCTL_GET_RDMA_STATS` (`_IOR`, 0x33)

**Phase 5 additions:**
- NUMA constant: `DMAPLANE_NUMA_ANY` (-1) — allocate on current CPU's local node
- `DMAPLANE_MAX_NUMA_NODES` (8) — max nodes in topology/benchmark arrays
- NUMA topology (`dmaplane_numa_topo`): `__u32 nr_nodes` (out), `__u32 nr_cpus` (out), per-node CPU counts, online flags, memory sizes (total/free KB), `__u32 distances[8][8]` (ACPI SLIT matrix)
- NUMA benchmark (`dmaplane_numa_bench_params`): `__u64 buffer_size` (in), `__u32 iterations` (in), `__u64 bw_mbps[8][8]` (out), `__u64 lat_ns[8][8]` (out), `__u32 nr_nodes` (out), `__u32 status` (out)
- Ioctl commands: `DMAPLANE_IOCTL_QUERY_NUMA_TOPO` (`_IOR`, 0x50), `DMAPLANE_IOCTL_NUMA_BENCH` (`_IOWR`, 0x51)

All userspace consumers (`tests/`, `examples/misc/`) include this header via `-I../include` or `-I../../include`.

### `driver/dmaplane.h`

The kernel-internal header. Includes `dmaplane_uapi.h` for the shared types, then adds kernel-only structures behind `#ifdef __KERNEL__`.

**Kernel-internal types:**
- `struct dmaplane_ring`: array of `dmaplane_ring_entry[RING_SIZE]`, `spinlock_t lock`, `unsigned int head` (producer writes, `____cacheline_aligned_in_smp`), `unsigned int tail` (consumer reads, `____cacheline_aligned_in_smp`). Head and tail are monotonically increasing — modulo is applied only when indexing into the array. Full when `(head - tail) == RING_SIZE`, empty when `head == tail`.
- `struct dmaplane_stats_kern`: kernel-internal stats with `atomic64_t total_submissions`, `atomic64_t total_completions`, `atomic_t ring_high_watermark`, `atomic_t dropped_count`, `atomic64_t buffers_created`, `atomic64_t buffers_destroyed`, plus Phase 3 dma-buf counters: `atomic64_t dmabufs_exported`, `dmabufs_released`, `dmabuf_attachments`, `dmabuf_detachments`, `dmabuf_maps`, `dmabuf_unmaps`, plus Phase 4 RDMA counters: `atomic64_t mrs_registered`, `mrs_deregistered`, `sends_posted`, `recvs_posted`, `completions_polled`, `completion_errors`, `bytes_sent`, `bytes_received`, plus Phase 5 NUMA counters: `atomic64_t numa_local_allocs`, `numa_remote_allocs`, `numa_anon_allocs`. All counters are device-level (survive individual resource cycles). Safe for concurrent updates and reads.
- `struct dmaplane_channel`: `struct kref refcount` (channel lifetime), `struct mutex lock` (per-channel state lock), `sub_ring` (submission ring), `comp_ring` (completion ring), `struct task_struct *worker` (kthread), `wait_queue_head_t wait_queue` (worker sleeps here), `unsigned int id` (channel index), `atomic_t in_flight` (submissions not yet completed), `bool shutdown` (signals worker to exit), `bool active` (slot is in use, set to false by kref release callback), `struct dmaplane_stats_kern stats` (atomic counters).
- `struct dmaplane_buffer` (Phase 2+3): per-buffer tracking. `unsigned int id` (handle, never 0), `int alloc_type` (`BUF_TYPE_COHERENT` or `BUF_TYPE_PAGES`), `size_t size`, `bool in_use` (protected by `buf_lock`), `void *vaddr` (kernel VA — from `dma_alloc_coherent` or `vmap`), `dma_addr_t dma_handle` (coherent only), `struct page **pages` + `unsigned int nr_pages` (page-backed only), `atomic_t mmap_count` (active userspace mappings, prevents destroy while mapped). Phase 3 additions: `bool dmabuf_exported` (prevents double export and blocks DESTROY_BUFFER while a dma-buf wraps this buffer, set/cleared under `buf_lock`), `struct dma_buf *dmabuf` (back-pointer, NULL when not exported). Phase 5 additions: `int numa_node` (requested NUMA node, -1 = DMAPLANE_NUMA_ANY), `int actual_numa_node` (actual node where majority of pages landed, determined post-hoc via `page_to_nid`).
- `struct poll_cq_wait` (Phase 4): managed CQ completion wait — `struct ib_cqe cqe` (callback entry, `.done = poll_cq_done`), `struct ib_wc wc` (stashed work completion), `struct completion done` (signaled by callback). Used with `IB_POLL_DIRECT` CQs.
- `struct dmaplane_mr_entry` (Phase 4): per-MR tracking — `__u32 id`, `__u32 buf_id`, `struct ib_mr *mr` (NULL for local-only, non-NULL for fast-reg), `__u32 lkey`, `__u32 rkey`, `__u64 sge_addr` (kernel VA for rxe, IOMMU addr for real HW), `struct sg_table *sgt`, `int sgt_nents`, `bool in_use`, `ktime_t reg_time`. Two paths: local-only uses `pd->local_dma_lkey` (rkey=0), fast-reg uses `ib_alloc_mr` + `IB_WR_REG_MR` for remote access.
- `struct dmaplane_rdma_ctx` (Phase 4): RDMA connection context — `struct ib_device *ib_dev`, `struct ib_pd *pd`, `struct ib_cq *cq_a`/`*cq_b` (loopback CQs), `struct ib_qp *qp_a`/`*qp_b` (loopback QPs), `__u8 port`, `__u16 lid`, `int gid_index`, `union ib_gid gid`, `bool initialized`. The loopback pair (QP-A sends, QP-B receives) benchmarks the full DMA data path without a remote peer.
- `struct dmaplane_dev`: `struct platform_device *pdev` (DMA-capable device for all DMA API calls), `channels[DMAPLANE_MAX_CHANNELS]`, `struct mutex dev_mutex`, `struct cdev`, `struct class *`, `dev_t devno`, `struct device *`, `buffers[DMAPLANE_MAX_BUFFERS]` (64 slots), `struct mutex buf_lock` (protects buffer array), `unsigned int next_buf_id` (monotonically increasing, skips 0), `struct dmaplane_stats_kern stats` (shared atomic counters), plus device-level atomic counters: `atomic_t active_channels`, `atomic64_t total_opens`, `atomic64_t total_closes`, `atomic64_t total_channels_created`, `atomic64_t total_channels_destroyed`. Phase 4 additions: `struct dmaplane_rdma_ctx rdma`, `struct rw_semaphore rdma_sem` (read for MR/benchmark ops, write for setup/teardown), `struct dmaplane_mr_entry mrs[DMAPLANE_MAX_MRS]` (64 slots), `struct mutex mr_lock` (protects MR array), `__u32 next_mr_id`. Counters are printed via `pr_info` at module unload; Phase 7 will export through debugfs.
- `struct dmaplane_file_ctx`: `struct dmaplane_dev *dev`, `struct dmaplane_channel *chan` (NULL until CREATE_CHANNEL).
- `dmaplane_channel_release()`: kref release callback — marks slot inactive (`active = false`), decrements `active_channels`, increments `total_channels_destroyed`. Called when the last kref drops to zero.
- Inline helpers: `dmaplane_ring_full()`, `dmaplane_ring_empty()`, `dmaplane_ring_count()`.

**Design notes**: Fixed-size arrays with linear scan are a deliberate simplicity trade-off — 4 channel slots is enough for Phase 1's use cases and avoids IDR complexity. The ring buffer uses unsigned arithmetic for wrap-free full/empty detection.

**kref lifetime model**: Two refs are taken at channel creation: one for the file_ctx, one for the worker kthread. Both are dropped by `dmaplane_channel_destroy` after `kthread_stop` guarantees the worker has exited. The worker does **not** drop its own ref because the kthread wrapper (`kernel/kthread.c`) can skip `threadfn` entirely if `KTHREAD_SHOULD_STOP` is set before the kthread gets a CPU timeslice — common when channels are created and destroyed quickly (e.g., `test_channel_creation`). Dropping both refs in destroy avoids slot leaks.

---

## Kernel Module (`driver/`)

### `driver/main.c`

Character device driver with ioctl dispatch, mmap support, and per-channel worker threads. Singleton `struct dmaplane_dev *dma_dev` at module scope.

**Module init** (`dmaplane_init`):
- `kzalloc` the device context
- `platform_device_alloc("dmaplane_dma")` + `platform_device_add` + `dma_set_mask_and_coherent` (64-bit, fallback 32-bit) — creates a DMA-capable device. `device_create` alone is NOT DMA-capable; the DMA API requires a bus-registered device with a DMA mask.
- `mutex_init` for `buf_lock`, `next_buf_id = 1`. Phase 4: `init_rwsem(&rdma_sem)`, `mutex_init(&mr_lock)`, `next_mr_id = 1`, 8 RDMA `atomic64_set` calls
- `alloc_chrdev_region` for dynamic major number
- `cdev_init` + `cdev_add`
- `class_create` + `device_create` for `/dev/dmaplane` udev node
- Clean teardown in reverse order on failure (goto-based cleanup: `err_class_destroy`, `err_cdev_del`, `err_unreg_region`, `err_unreg_pdev`, `err_put_pdev`, `err_free_dev`). Distinction: `platform_device_put` for devices allocated but never added; `platform_device_unregister` for devices both allocated and added.

**Module exit** (`dmaplane_exit`) — ordering is critical:
1. Acquires `dev_mutex`, stops any remaining active worker kthreads via `dmaplane_channel_destroy`
2. Prints lifetime summary via `pr_info`: total opens, closes, channels created, channels destroyed, buffers created, buffers destroyed
3. Tears down char device (`device_destroy` + `class_destroy` + `cdev_del` + `unregister_chrdev_region`) — prevents new ioctls/opens from racing with cleanup
4. RDMA teardown (Phase 4): `down_write(&rdma_sem)`. If `rdma.initialized`: deregister all in-use MRs, then `rdma_engine_teardown`. Mark all MR slots `in_use = false`. `up_write`. The write lock synchronizes with any in-flight operations that started before `cdev_del` returned. MRs must be deregistered before teardown because their DMA mappings reference `ib_dev`.
5. Destroys remaining buffers — warns on leaked mmap references (`mmap_count > 0`) and leaked dma-buf exports (`dmabuf_exported`). Must happen AFTER RDMA teardown (MR DMA mappings freed) and AFTER char device teardown, but BEFORE platform device unregister (`dma_free_coherent` needs the platform device alive)
6. `platform_device_unregister`
7. `kfree`

**File operations:**
- `dmaplane_open`: Checks `capable(CAP_SYS_ADMIN)` — DMA buffer allocation and mmap of kernel pages are privileged operations. Allocates `dmaplane_file_ctx` via `kzalloc`, stores in `filp->private_data`. Increments `total_opens`.
- `dmaplane_release`: If a channel was assigned, acquires `dev_mutex`, calls `dmaplane_channel_destroy` (signals worker shutdown, `kthread_stop`, drops both krefs), releases mutex, frees file context. Increments `total_closes`. Handles process exit without explicit cleanup.
- `dmaplane_ioctl`: Dispatches to per-command handlers via switch (Phase 1: 0x01–0x04, Phase 2: 0x05–0x09, Phase 3: 0x0A–0x0B, Phase 4: 0x10–0x11, 0x20–0x21, 0x30–0x33, Phase 5: 0x50–0x51).
- `dmaplane_mmap`: Maps DMA buffer pages into userspace. Extracts `buf_id = vma->vm_pgoff`, finds buffer under `buf_lock`. Sets `VM_DONTCOPY | VM_DONTEXPAND | VM_DONTDUMP`. Coherent: `dma_mmap_coherent` (resets `vm_pgoff = 0` first — `dma_mmap_coherent` interprets pgoff as offset into the allocation). Pages: `vm_insert_page` loop. Increments `mmap_count` on success (NOT via `vma_open` — Linux does not call `vma_open` for the initial mmap). VMA ops: `dmaplane_vma_open` (increment on fork/mremap), `dmaplane_vma_close` (decrement on munmap/exit).

**Ioctl handlers:**

| Command | Handler | Description |
|---------|---------|-------------|
| `DMAPLANE_IOCTL_CREATE_CHANNEL` | `dmaplane_ioctl_create_channel` | Returns `-EBUSY` if fd already has a channel. Finds free slot (under `dev_mutex`), inits channel via `dmaplane_channel_init` (kref_init, mutex_init, rings, atomic stats), takes second kref for worker, creates kthread `"dmaplane/%d"` via `kthread_create`, wakes thread, increments `active_channels` and `total_channels_created`, stores channel in file context, `copy_to_user` channel ID |
| `DMAPLANE_IOCTL_SUBMIT` | `dmaplane_ioctl_submit` | `copy_from_user`, acquire submission ring spinlock, check full (return `-ENOSPC`), write entry at `head % RING_SIZE`, `smp_store_release` on head, track high watermark via `atomic_set`, release lock, `atomic_inc(in_flight)`, `atomic64_inc(total_submissions)`, `wake_up_interruptible` worker |
| `DMAPLANE_IOCTL_COMPLETE` | `dmaplane_ioctl_complete` | Acquire completion ring spinlock, check empty (return `-EAGAIN`), read entry at `tail % RING_SIZE`, `smp_store_release` on tail, release lock, `copy_to_user` |
| `DMAPLANE_IOCTL_GET_STATS` | `dmaplane_ioctl_get_stats` | Reads `dmaplane_stats_kern` atomics into a plain `dmaplane_stats` UAPI struct, then `copy_to_user`. Each atomic read is individually consistent; the set may be momentarily inconsistent |
| `DMAPLANE_IOCTL_CREATE_BUFFER` | `dmaplane_ioctl_create_buffer` | `copy_from_user`, delegates to `dmabuf_rdma_create_buffer`, `copy_to_user` result. **copy_to_user undo**: if `copy_to_user` fails after buffer creation, destroys the buffer to prevent an orphaned allocation that userspace can never reference |
| `DMAPLANE_IOCTL_DESTROY_BUFFER` | `dmaplane_ioctl_destroy_buffer` | `copy_from_user` bare `__u32` handle, delegates to `dmabuf_rdma_destroy_buffer`. Returns `-EBUSY` if `mmap_count > 0` or `dmabuf_exported` |
| `DMAPLANE_IOCTL_GET_MMAP_INFO` | `dmaplane_ioctl_get_mmap_info` | Looks up buffer under `buf_lock`, returns `mmap_offset = buf_id << PAGE_SHIFT` and `mmap_size = buf->size`. Userspace uses these to call `mmap(2)` |
| `DMAPLANE_IOCTL_GET_BUF_STATS` | `dmaplane_ioctl_get_buf_stats` | Reads `buffers_created`, `buffers_destroyed`, and 3 NUMA counters (`numa_local_allocs`, `numa_remote_allocs`, `numa_anon_allocs`) into `dmaplane_buf_stats` UAPI struct |
| `DMAPLANE_IOCTL_EXPORT_DMABUF` | `dmaplane_ioctl_export_dmabuf` | `copy_from_user`, delegates to `dmaplane_dmabuf_export` (holds `buf_lock` for entire operation), `copy_to_user`. **No copy_to_user undo** — fd already installed in process fd table by `dma_buf_fd` |
| `DMAPLANE_IOCTL_GET_DMABUF_STATS` | `dmaplane_ioctl_get_dmabuf_stats` | Reads 6 device-level dma-buf atomic counters into `dmaplane_dmabuf_stats` UAPI struct |
| `DMAPLANE_IOCTL_SETUP_RDMA` | `dmaplane_ioctl_setup_rdma` | `copy_from_user`, null-terminate `ib_dev_name`, `down_write(&rdma_sem)`, call `rdma_engine_setup`, `up_write`, `copy_to_user`. **Undo on copy_to_user failure**: `down_write`, `rdma_engine_teardown`, `up_write` |
| `DMAPLANE_IOCTL_TEARDOWN_RDMA` | `dmaplane_ioctl_teardown_rdma` | `down_write(&rdma_sem)`, deregister ALL in-use MRs first (loop `mrs[]`), then `rdma_engine_teardown`, `up_write` |
| `DMAPLANE_IOCTL_REGISTER_MR` | `dmaplane_ioctl_register_mr` | `copy_from_user`, `down_read(&rdma_sem)`, call `rdma_engine_register_mr`, `up_read`, `copy_to_user`. **Undo on copy_to_user failure**: `down_read`, `rdma_engine_deregister_mr`, `up_read` |
| `DMAPLANE_IOCTL_DEREGISTER_MR` | `dmaplane_ioctl_deregister_mr` | `copy_from_user` bare `__u32`, `down_read(&rdma_sem)`, call `rdma_engine_deregister_mr`, `up_read` |
| `DMAPLANE_IOCTL_LOOPBACK_TEST` | `dmaplane_ioctl_loopback_test` | `copy_from_user`, `down_read(&rdma_sem)`, call `benchmark_loopback`, `up_read`, `copy_to_user` |
| `DMAPLANE_IOCTL_PINGPONG_BENCH` | `dmaplane_ioctl_pingpong_bench` | Same pattern with `benchmark_pingpong` |
| `DMAPLANE_IOCTL_STREAMING_BENCH` | `dmaplane_ioctl_streaming_bench` | Same pattern with `benchmark_streaming` |
| `DMAPLANE_IOCTL_GET_RDMA_STATS` | `dmaplane_ioctl_get_rdma_stats` | Read 8 RDMA atomic counters, `copy_to_user`. No lock — all stats are `atomic64_t` |
| `DMAPLANE_IOCTL_QUERY_NUMA_TOPO` | `dmaplane_ioctl_query_numa_topo` | `kmalloc` topology struct (~600B, too large for stack), call `dmaplane_query_numa_topo`, `copy_to_user`, `kfree`. No lock — reads kernel-maintained topology data |
| `DMAPLANE_IOCTL_NUMA_BENCH` | `dmaplane_ioctl_numa_bench` | `kmalloc` bench struct (~1100B), `copy_from_user`, call `dmaplane_numa_bench`, `copy_to_user`, `kfree`. No lock — benchmark allocates its own temporary buffers |

**Worker thread** (`dmaplane_worker_fn`):
- Loops until `kthread_should_stop()`
- Sleeps on channel wait queue: condition is `(submission ring not empty) || kthread_should_stop()`
- On shutdown with empty ring, breaks out cleanly
- Drains submissions one at a time: acquires sub_ring lock, re-checks empty under lock, reads entry at `tail % RING_SIZE`, advances tail via `smp_store_release`, releases lock
- Processes entry (increments payload by 1)
- Pushes to completion ring: acquires comp_ring lock, checks full. **If completion ring is full, releases lock and yields via `cond_resched()` in a retry loop** (checking `kthread_should_stop` each iteration). This prevents entry loss under backpressure when userspace drains completions slower than the worker produces them.
- Decrements `in_flight`, `atomic64_inc(total_completions)`
- Calls `cond_resched()` every 64 entries to yield CPU

**Channel lifecycle:**
- `dmaplane_channel_init`: `kref_init` (refcount=1 for file_ctx), `mutex_init` (per-channel lock), zeroes rings, `spin_lock_init` both ring locks, `init_waitqueue_head`, `atomic64_set`/`atomic_set` for all stats counters
- `dmaplane_channel_destroy`: acquires `channel->lock`, sets `shutdown = true`, releases lock, `wake_up_interruptible`, `kthread_stop` (blocking wait for thread exit), then two `kref_put` calls (worker ref + file_ctx ref). The second `kref_put` triggers `dmaplane_channel_release` which sets `active = false` and updates device-level counters.
- `dmaplane_channel_release`: kref release callback — sets `active = false`, `atomic_dec(active_channels)`, `atomic64_inc(total_channels_destroyed)`.

**Locking model (7 locks):**
- Device mutex (`dev_mutex`): protects channel slot allocation/deallocation. Sleeping context OK — only acquired from ioctl and release (process context). Outermost lock in the ordering. Independent of `buf_lock` (never nested).
- Buffer mutex (`buf_lock`): protects `buffers[]` array — slot allocation, lookup, deallocation, and mmap. Sleeping context OK. Independent of `dev_mutex` (never nested). Ordering: `rdma_sem` (read) → `buf_lock` → `mr_lock`.
- Per-channel mutex (`channel->lock`): protects channel state transitions (shutdown flag, worker pointer). Separate from ring spinlocks (which protect ring data) and from dev_mutex (which protects slot allocation). Ordering: `dev_mutex` → `channel->lock` → ring spinlocks.
- Submission ring spinlock (`sub_ring.lock`): protects ring entries between ioctl submit (producer, advances head) and worker thread (consumer, advances tail).
- Completion ring spinlock (`comp_ring.lock`): protects ring entries between worker thread (producer, advances head) and ioctl complete (consumer, advances tail).
- RDMA semaphore (`rdma_sem`, Phase 4): read-write semaphore protecting the RDMA context. Read lock: MR registration/deregistration, all benchmarks (concurrent). Write lock: RDMA setup and teardown (exclusive). Ordering: `rdma_sem` (read) → `buf_lock` → `mr_lock`.
- MR mutex (`mr_lock`, Phase 4): protects `mrs[]` array — slot allocation, lookup, deallocation. Ordering: always acquired after `rdma_sem` (read) and after `buf_lock` if both needed.
- Worker shutdown: acquire `channel->lock`, set `channel->shutdown = true`, release lock, `wake_up_interruptible` + `kthread_stop` (blocking wait for thread exit). The completion ring yield loop also checks `kthread_should_stop` to ensure the worker exits even if the ring stays full.

---

### `driver/dmabuf_rdma.h`

Header declaring three buffer management functions with locking contracts: `dmabuf_rdma_create_buffer` and `dmabuf_rdma_destroy_buffer` acquire `buf_lock` internally; `dmabuf_rdma_find_buffer` requires the caller to hold `buf_lock` for the pointer's lifetime.

### `driver/dmabuf_rdma.c`

Buffer allocation and lookup. Two allocation paths:

**`dmabuf_rdma_create_buffer`**:
- Validates size (> 0, <= 1 GB), alloc_type, and NUMA node (Phase 5). NUMA validation happens before `buf_lock` — rejects invalid/offline nodes with `-EINVAL`. `DMAPLANE_NUMA_ANY` (-1) is always valid.
- Acquires `buf_lock`, linear scan for free slot. Zeroes the slot via `memset` to clear stale pointers from a previous occupant.
- Assigns `next_buf_id++` (wraps to 1, skipping 0 — sentinel for "no buffer"). Stores `buf->numa_node = target_node`.
- **Coherent path** (`BUF_TYPE_COHERENT`): `dma_alloc_coherent(&dev->pdev->dev, ...)` + `memset` zero. Cannot be NUMA-steered — `dma_alloc_coherent` has no node parameter. Reports actual placement post-hoc via `page_to_nid(virt_to_page(buf->vaddr))`.
- **Page-backed path** (`BUF_TYPE_PAGES`): `kvcalloc` page array, `alloc_pages_node(alloc_node, GFP_KERNEL | __GFP_ZERO, 0)` loop where `alloc_node = NUMA_NO_NODE` for `NUMA_ANY` or the target node ID. Post-hoc verification via `page_to_nid()` on each page tracks local/remote counts. After the page loop, majority vote determines `actual_numa_node`. Warns via `pr_warn` if any pages are misplaced. `vmap()` for contiguous kernel VA.
- Sets `params->actual_numa_node = buf->actual_numa_node` before returning to userspace.
- Increments one of `numa_local_allocs`, `numa_remote_allocs`, or `numa_anon_allocs` per buffer.
- All error paths (no slots, allocation failure, vmap failure) properly unwind with reverse-order cleanup.

**`dmabuf_rdma_destroy_buffer`**:
- Acquires `buf_lock`, finds buffer. Refuses with `-EBUSY` if `mmap_count > 0` or `dmabuf_exported` (Phase 3 guard — buffer cannot be destroyed while a dma-buf wraps it).
- Pages: `vunmap` → `__free_page` loop → `kvfree(pages)`.
- Coherent: `dma_free_coherent(&dev->pdev->dev, ...)`.

**`dmabuf_rdma_find_buffer`**: Linear scan. Caller must hold `buf_lock`.

**`dmabuf_rdma_find_mr`** (Phase 4): Linear scan of `mrs[]` array by MR ID. Caller must hold `mr_lock`. Returns pointer into `dev->mrs[]`, or NULL.

All four are `EXPORT_SYMBOL_GPL` for use by rdma_engine.c and benchmark.c.

### `driver/dmabuf_export.h`

Header declaring the dma-buf export function and kernel self-test with locking contracts: `dmaplane_dmabuf_export` acquires `buf_lock` for the entire operation (mutex, sleeping OK); `dmaplane_dmabuf_selftest` creates a buffer, exports, exercises attach/map/unmap/detach via the kernel dma-buf API.

### `driver/dmabuf_export.c`

dma-buf exporter for page-backed buffers. Implements the `dma_buf_ops` vtable with 9 callbacks:

**Export context** (`dmaplane_dmabuf_ctx`): borrows `pages`, `nr_pages`, `size`, `vaddr` from the backing buffer. Tracks `attach_count` (atomic). All stats counters are device-level on `dmaplane_stats_kern`, not per-export.

**`dma_buf_ops` callbacks:**
- `attach`/`detach`: increment/decrement `attach_count`, update device-level stats. Accept all devices unconditionally (no IOMMU group filtering).
- `map_dma_buf`: the expensive callback — builds a per-device SG table via `sg_alloc_table` + `sg_set_page` loop + `dma_map_sgtable`. Each importer may sit behind a different IOMMU, so the SG table must contain DMA addresses valid for that device's context. This is why SG tables are built per-attachment, not at export time.
- `unmap_dma_buf`: `dma_unmap_sgtable` + `sg_free_table` + `kfree`.
- `vmap`/`vunmap`: vmap reuses the buffer's existing kernel VA via `iosys_map_set_vaddr`. vunmap is a no-op — the buffer owns the mapping.
- `release`: acquires `buf_lock` (runs in process context from `fput`, mutex_lock is legal), clears `dmabuf_exported` and `dmabuf` pointer, frees context. No deadlock with destroy: destroy checks `dmabuf_exported` and returns `-EBUSY` without waiting.
- `mmap`: `vm_insert_page` loop with `VM_DONTCOPY | VM_DONTEXPAND | VM_DONTDUMP`. Allows userspace to mmap the dma-buf fd directly.
- `begin_cpu_access`/`end_cpu_access`: stubs on x86 (hardware cache coherency). ARM/RISC-V would need `dma_sync_sg_for_cpu`/`dma_sync_sg_for_device`.

**`dmaplane_dmabuf_export`**: acquires `buf_lock` for the entire operation (prevents double-export race). Validates `BUF_TYPE_PAGES` (coherent → `-EINVAL`), checks `!dmabuf_exported` (→ `-EBUSY`), allocates context, calls `dma_buf_export` + `dma_buf_fd(O_CLOEXEC)`. `exp_info.flags = O_RDWR` is required — without it, mmap of the dma-buf fd with `PROT_WRITE` fails with `EACCES`.

**`dmaplane_dmabuf_selftest`**: kernel-space test callable via `test_dmabuf=1` module parameter. Exercises attach/map/unmap/detach 100 times using the platform device as the importing device. Verifies SG table has entries after each map.

`MODULE_IMPORT_NS(DMA_BUF)` is required in `main.c` — the dma-buf framework exports its symbols under this namespace since kernel 5.x.

### `driver/rdma_engine.h`

Header declaring the RDMA engine API: `poll_cq_done` (managed CQ callback), `rdma_engine_setup`/`teardown`, `rdma_engine_register_mr`/`deregister_mr`, `rdma_engine_post_send`/`post_recv`, `rdma_engine_poll_cq`. Locking contract: callers must hold `rdma_sem` (read for MR/benchmark ops, write for setup/teardown).

### `driver/rdma_engine.c`

Kernel-level InfiniBand Verbs client. Communicates with `ib_core` and `rdma_rxe` (Soft-RoCE).

**Internal helpers:**
- `find_ib_device`: `ib_device_get_by_name(name, RDMA_DRIVER_UNKNOWN)`
- `scan_gid_table`: scans up to 16 GID entries, prefers last link-local `ROCE_UDP_ENCAP` GID. GID[0] on Ubuntu with stable-privacy addressing is an EUI-64 ghost that causes silent traffic failure. Falls back to index 0 with warning.
- `qp_to_init`/`qp_to_rtr`/`qp_to_rts`: QP state machine transitions. Each requires exact attribute masks — wrong combinations silently fail.
- `connect_loopback_qps`: extracts local DMAC via `rdma_read_gid_attr_ndev_rcu` under RCU read lock. Connects QP-A ↔ QP-B through full RESET → INIT → RTR → RTS sequence.
- `register_mr_build_sgt`: `sg_alloc_table` + `sg_set_page` loop + `ib_dma_map_sg(DMA_BIDIRECTIONAL)`. One SG entry per page.
- `register_mr_fastreg`: `ib_alloc_mr(IB_MR_TYPE_MEM_REG)` + `ib_map_mr_sg` + post `IB_WR_REG_MR` on QP-A. Only called when access_flags include remote permissions.

**Public API (all `EXPORT_SYMBOL_GPL`):**
- `poll_cq_done`: managed CQ callback — `container_of(wc->wr_cqe)`, stash WC, `complete()`. Exists to satisfy `ib_cqe` contract; rarely invoked with `IB_POLL_DIRECT`.
- `rdma_engine_setup`: creates IB device → PD → 2× CQ (`IB_POLL_DIRECT`) → 2× RC QP (`IB_SIGNAL_ALL_WR`) → loopback connection. Goto-based reverse-order cleanup on failure.
- `rdma_engine_teardown`: moves QPs to `IB_QPS_ERR` (flushes outstanding WRs), destroys in strict reverse order: QP-B → QP-A → CQ-B → CQ-A → PD → `ib_device_put`. Race-free because CQs use `IB_POLL_DIRECT` (no background callbacks).
- `rdma_engine_register_mr`: snapshots buffer pages under `buf_lock` into `pages_copy` (kvmalloc + memcpy), releases lock. Builds SG table, optionally allocates fast-reg MR. Finds free slot under `mr_lock`. Two paths: local-only (`pd->local_dma_lkey`, rkey=0, `sge_addr = kernel VA`) or fast-reg (`mr->lkey/rkey/iova`).
- `rdma_engine_deregister_mr`: under `mr_lock`, `ib_dma_unmap_sg` (defensive NULL check on `ib_dev`), `sg_free_table`, `kfree(sgt)`, `ib_dereg_mr` if fast-reg, mark `in_use = false`.
- `rdma_engine_post_send`/`post_recv`: build SGE from MR entry, init completion, post via `ib_post_send`/`ib_post_recv`.
- `rdma_engine_poll_cq`: busy-poll with wall-clock deadline, `ib_poll_cq` + `usleep_range(50, 200)` + `cond_resched`. Returns 1 (success), 0 (timeout), negative (error).

### `driver/benchmark.h`

Header declaring three benchmark functions: `benchmark_loopback`, `benchmark_pingpong`, `benchmark_streaming`. All require `rdma_sem` read lock.

### `driver/benchmark.c`

Three RDMA benchmarks over the loopback QP pair. All snapshot MR and buffer fields by value under locks, then release locks before I/O.

- `benchmark_loopback`: single send (QP-A) → recv (QP-B). Writes 0xDEADBEEF pattern, polls both CQs, reports `latency_ns`.
- `benchmark_pingpong`: N iterations of send+recv. Collects per-iteration latency, sorts via `sort()`, computes avg, P99, throughput (MB/s = bytes * 1000 / ns).
- `benchmark_streaming`: pipelines sends up to `queue_depth` outstanding. Pre-posts `min(2*qdepth, iterations)` receives, replenishes during the send/poll loop. Eagerly drains recv CQ-B to prevent overflow. Flushes both CQs before and after for clean state.

All three are `EXPORT_SYMBOL_GPL`.

---

## Tests (`tests/`)

### `tests/test_phase1_driver.c`

Userspace stress test for the driver. Opens `/dev/dmaplane`, exercises all ioctls, validates correctness under load. Uses `pthreads` for multi-channel testing. Includes helper functions: `dev_open`, `create_channel`, `submit_entry`, `complete_entry`, `complete_poll` (retry loop with `usleep`).

**Test cases:**
1. **Basic open/close** — open and close `/dev/dmaplane`, no crash
2. **Channel creation** — verify channel IDs 0, 1 across two fds
3. **Submit and complete** — roundtrip with payload `0xDEADBEEF`, verify worker increments to `0xDEADBEF0`
4. **Ring full behavior** — submit entries until `-ENOSPC` or worker drains faster than submit (noted as acceptable), then drain all completions and verify payload ordering
5. **Single-channel stress** — 1M submit/complete cycles in batches of 256, verify count match, report throughput (ops/sec)
6. **Multi-channel stress** — 4 channels × 250K entries via pthreads, each channel uses payload base `channel_id * count` for cross-contamination detection, report total throughput
7. **Cleanup on close** — submit 100 entries without completing, close fd, verify no kernel crash (user checks dmesg)

Reports PASS/FAIL per test and a summary. Exit code 0 if all pass, 1 if any fail.

### `tests/test_phase2_dma.c`

Userspace test suite for Phase 2 DMA buffer management. Opens `/dev/dmaplane`, exercises buffer creation, mmap, destroy, and stats. Includes helpers: `create_buffer`, `destroy_buffer`, `get_mmap_info`, `map_buffer`, `get_buf_stats`.

**Test cases:**
1. **Coherent alloc + mmap** — create 4 KB coherent buffer, mmap, write `0xDEADBEEF` pattern, read back and verify, munmap, destroy
2. **Page-backed alloc + mmap** — create 1 MB page-backed buffer, same write/verify cycle with `0xCAFEBABE`
3. **Destroy with active mmap** — create buffer, mmap, try destroy (must fail with `EBUSY`), munmap, destroy again (must succeed)
4. **Allocation stress** — 1000 create/destroy cycles, verify `buf_stats` match
5. **Max buffers** — allocate 64 buffers, verify 65th returns `ENOMEM`, free all
6. **Handle uniqueness** — 10 create/destroy cycles, verify handles are monotonically increasing and never 0
7. **Large buffer** — 16 MB page-backed buffer, mmap, write incrementing pattern, verify
8. **Invalid parameters** — size 0 → `EINVAL`, bad alloc_type → `EINVAL`, destroy non-existent → `ENOENT`, mmap_info non-existent → `ENOENT`

### `tests/test_phase3_dmabuf.c`

Userspace test suite for Phase 3 dma-buf export. Opens `/dev/dmaplane`, exercises buffer export, fd lifecycle, mmap through dma-buf fd, and the destroy guard. Includes helpers: `create_buffer`, `destroy_buffer`, `export_dmabuf`, `get_dmabuf_stats`.

**Test cases:**
1. **Export page-backed buffer** — create, export, verify fd >= 0, close fd, destroy
2. **Export coherent (must fail)** — coherent buffers have no page array, export returns `-EINVAL`
3. **Double export (must fail)** — one dma-buf per buffer, second export returns `-EBUSY`
4. **Destroy while exported** — must fail with `-EBUSY`, close dma-buf fd, then destroy succeeds
5. **mmap via dma-buf fd** — export, mmap the dma-buf fd (not `/dev/dmaplane`), write `0xBEEFCAFE` pattern, verify
6. **Export + close fd + destroy** — verify release callback clears `dmabuf_exported` flag
7. **Stats verification** — export/close cycle, verify `dmabufs_exported` and `dmabufs_released` counters
8. **Multiple buffers exported** — 4 buffers simultaneously, close in reverse order
9. **Large buffer export** — 16 MB buffer, mmap via dma-buf fd, full write/read cycle

### `tests/test_phase4_rdma.c`

Userspace test suite for Phase 4 RDMA integration. Auto-detects the rxe device by scanning `/sys/class/infiniband/`. Prints setup instructions if no rxe device found. Saves benchmark results for a summary table.

**Test cases:**
1. **RDMA setup** — setup with auto-detected rxe device, verify `status == 0`
2. **Double setup (must fail)** — second `SETUP_RDMA` returns `-EBUSY`
3. **MR registration** — create page-backed buffer, register with `IB_ACCESS_LOCAL_WRITE`, verify `mr_id`/`lkey` non-zero
4. **MR registration of coherent (must fail)** — coherent has no page array, returns `-EINVAL`
5. **Loopback test** — single send/recv, verify `status == 0`, print latency
6. **Ping-pong benchmark** — 1000 iterations, 4KB messages, print avg/P99/throughput
7. **Streaming benchmark** — 1000 iterations, 4KB messages, queue depth 8, print throughput
8. **MR deregistration** — deregister MR, verify. Non-existent returns `-ENOENT`
9. **RDMA stats** — verify `sends_posted`, `completions_polled` > 0, `completion_errors == 0`
10. **Teardown + re-initialization** — teardown, loopback-after-teardown fails, re-setup succeeds

### `tests/test_phase5_numa.c`

Userspace test suite for Phase 5 NUMA topology and optimization. Opens `/dev/dmaplane`, exercises NUMA-aware buffer allocation, topology query, cross-node bandwidth benchmark, and regression with `DMAPLANE_NUMA_ANY`. Includes helpers: `create_buffer` (with `numa_node` and `actual_node` parameters), `destroy_buffer`.

**Test cases:**
1. **Topology query** — call `QUERY_NUMA_TOPO`, verify `nr_nodes >= 1`, `nr_cpus >= 1`, at least one `node_online[i] == 1`, `distance[0][0] == 10`. Print full topology: nodes, CPUs per node, memory, distance matrix.
2. **Allocate on node 0** — page-backed buffer with `numa_node = 0`, verify `actual_numa_node == 0`
3. **Allocate with NUMA_ANY** — page-backed buffer with `numa_node = -1`, verify `actual_numa_node >= 0`
4. **Invalid node (must fail)** — request node 99, verify `-EINVAL`
5. **Coherent NUMA reporting** — coherent buffer with `numa_node = 0`, verify `actual_numa_node >= 0` (informational — coherent cannot be steered)
6. **NUMA stats** — call `GET_BUF_STATS`, verify `numa_local_allocs + numa_remote_allocs + numa_anon_allocs > 0`
7. **Cross-node bandwidth benchmark** — `NUMA_BENCH` with 1 MB, 100 iterations. Print NxN bandwidth matrix. On single-node, print 1x1 result. On multi-node, compute and print penalty ratios.
8. **Regression with NUMA_ANY** — create buffer, mmap, write/read `0xDEAD0000|i` pattern, munmap, destroy. Verifies NUMA fields didn't break existing functionality.

---

## Examples (`examples/`)

### `examples/misc/dma_explorer.c`

Interactive demo that creates one buffer of each type (coherent, page-backed), mmaps each, writes a pattern, verifies, and cleans up. Prints buffer details (handle, mmap offset, size) and final stats.

### `examples/misc/dma_sweep.c`

Allocates page-backed buffers from 4 KB to 64 MB (powers of 2), times the full create/mmap/write/verify/munmap/destroy cycle. Outputs a table of: size, create time, map time, write time, total time, and page count. Shows linear scaling with page count.

---

## Cross-cutting Observations

**Why ioctl, not read/write**: The driver uses `unlocked_ioctl` for all operations rather than `read`/`write` file operations. This is deliberate — each operation has structured input/output parameters that don't map cleanly to byte streams. The ioctl interface extends naturally as phases add new commands (DMA allocation, RDMA, GPU pinning) without changing the file operations vtable.

**Ring buffer design**: Monotonically increasing head/tail with modulo indexing avoids the "is the ring full or empty?" ambiguity that plagues wrapped-index designs. `smp_store_release` on head and tail updates ensures the other side sees consistent ring entries on weakly-ordered architectures (relevant for ARM, no-op on x86 but correct to include). Head and tail on separate cache lines (`____cacheline_aligned_in_smp`) prevent false sharing between producer and consumer.

**Completion ring backpressure**: The original design dropped entries when the completion ring was full. Testing under load (4-channel × 250K stress test) revealed that the worker could outpace userspace drain, causing silent completion loss and payload mismatches. The fix: the worker yields (`cond_resched` loop) when the completion ring is full, applying backpressure through the submission ring — if the completion ring is full, the worker blocks, the submission ring fills, and userspace gets `-ENOSPC`, forcing it to drain completions.

**Worker thread as processing stub**: The worker's "processing" (increment payload by 1) is intentionally trivial. The value of Phase 1 is the infrastructure — rings, locking, kthreads, ioctl dispatch, lifecycle management. Future phases replace the stub with real DMA operations, RDMA posts, and GPU transfers, but the ring/worker/ioctl scaffolding remains unchanged.

**kref and the kthread race**: The initial kref design had the worker thread dropping its own ref as its last act in `dmaplane_worker_fn`. This caused a slot leak: when a channel is created and destroyed quickly (e.g., `test_channel_creation`), the kthread may not receive a CPU timeslice before `kthread_stop` is called. The kthread wrapper (`kernel/kthread.c`) checks `KTHREAD_SHOULD_STOP` before calling `threadfn` — if set, `threadfn` is skipped entirely, the worker's `kref_put` never runs, and the refcount stays at 2. The fix: both refs are dropped by `dmaplane_channel_destroy` after `kthread_stop` guarantees the worker has exited (whether `threadfn` ran or not). This is unconditionally safe because `kthread_stop` is a blocking call.

**Atomic stats**: Per-channel stats use `atomic64_t` / `atomic_t` (`dmaplane_stats_kern`) instead of plain integers. This is portable and correct under the C memory model — plain `__u64` reads are atomic on x86-64 but not on 32-bit architectures or under compiler reordering. The UAPI struct (`dmaplane_stats`) remains plain `__u64`/`__u32`; the ioctl handler converts by reading each atomic.

**Device-level observability**: `dmaplane_dev` tracks lifetime counters (opens, closes, channels created/destroyed) and a current active channel count via atomics. Printed at module unload via `pr_info`. Phase 7 will export these through debugfs.

**Error paths**: All error paths use goto-based cleanup with reverse-order resource release. This pattern will be used throughout the project.

**Log prefix**: All kernel messages use the `dmaplane:` prefix via `pr_fmt(fmt) KBUILD_MODNAME ": " fmt`. Lifecycle messages use `pr_info`, per-operation messages use `pr_debug`.

**Why platform_device**: `device_create` produces a device node for udev but the underlying `struct device` has no bus registration and no DMA mask. The DMA API (`dma_alloc_coherent`, `dma_map_sg`) requires a device registered with a bus. `platform_device` solves this — it represents a virtual hardware device on the platform bus. Without it, `dma_alloc_coherent` returns NULL or triggers WARN on IOMMU-enabled machines.

**Two allocation paths**: Coherent (`dma_alloc_coherent`) gives physically contiguous, cache-coherent memory — good for small control structures where CPU and device need consistent access. Page-backed (`alloc_pages` + `vmap`) gives scattered physical pages with a contiguous kernel VA — good for large buffers (gradients, weights, KV-cache) where NUMA steering matters (Phase 3) and SG tables are needed for RDMA (Phase 4). The module owns all pages; there is no user-pinned path.

**mmap zero-copy chain**: `alloc_pages` → `vmap` (kernel access) → `mmap` via `vm_insert_page` (userspace access) → NIC reads via `ib_dma_map_sg` (Phase 4). The same physical pages serve all three consumers with zero copies.

**mmap_count and the vma_open gotcha**: `mmap_count` prevents buffer destruction while userspace holds a mapping. The count is incremented in `dmaplane_mmap` (the initial call) and in `vma_open` (fork/mremap). Linux does **not** call `vma_open` for the initial mmap — only for fork and mremap. If you only increment in `vma_open`, the initial mmap is untracked. `VM_DONTCOPY` blocks fork inheritance and `VM_DONTEXPAND` blocks mremap, so `vma_open` is a defensive increment that should rarely fire.

**dma_mmap_coherent pgoff reset**: The driver encodes the buffer ID in `vma->vm_pgoff` (via `IOCTL_GET_MMAP_INFO` → `mmap(fd, offset)`). But `dma_mmap_coherent` interprets `vm_pgoff` as a page offset into the coherent allocation. A non-zero pgoff (the buffer ID) causes `ENXIO` because the offset exceeds the allocation size. The fix: reset `vma->vm_pgoff = 0` before calling `dma_mmap_coherent`.

**copy_to_user undo pattern**: `IOCTL_CREATE_BUFFER` creates the buffer first, then copies the result to userspace. If `copy_to_user` fails, the buffer exists in the kernel but userspace has no handle to reference or destroy it — an orphaned allocation. The undo pattern destroys the buffer on `copy_to_user` failure.

**buf_lock vs dev_mutex separation**: These protect different resources (buffers vs channels) and are never nested. This allows concurrent buffer and channel operations — important for Phase 4+ where RDMA operations touch both.

**dma-buf as the cross-device sharing primitive**: Phase 3 wraps page-backed buffers as `struct dma_buf` objects. Only page-backed buffers can be exported — coherent allocations have no `struct page` array, and `virt_to_page` on `dma_alloc_coherent` memory is unreliable across architectures. The `dmabuf_exported` flag on `dmaplane_buffer` prevents both double export and destruction while the dma-buf exists — same defense-in-depth pattern as `mmap_count`.

**SG tables are per-device, built in map_dma_buf**: Each importer may sit behind a different IOMMU. The SG table must contain DMA addresses valid for that device's IOMMU context. Building at export time would assume a single IOMMU — wrong for cross-device sharing. The map callback is the expensive path: `sg_alloc_table` + `sg_set_page` loop + `dma_map_sgtable`.

**dma-buf release callback and buf_lock**: The `release` callback fires from `fput` (process context) when the last dma-buf reference drops. It acquires `buf_lock` to clear `dmabuf_exported` atomically. No deadlock with `dmabuf_rdma_destroy_buffer`: destroy checks `dmabuf_exported` and returns `-EBUSY` without waiting for the lock.

**O_RDWR on dma_buf_export**: The `exp_info.flags` must include `O_RDWR`. Without it, the dma-buf file is not writable, and userspace `mmap` with `PROT_WRITE` fails with `EACCES`. `O_CLOEXEC` is passed separately to `dma_buf_fd`.

**MODULE_IMPORT_NS(DMA_BUF)**: Since kernel 5.x, the dma-buf framework exports its symbols under the `DMA_BUF` namespace. Modules using `dma_buf_export`, `dma_buf_fd`, etc. must declare this import or fail to load with unresolved symbol errors.

**No copy_to_user undo for EXPORT_DMABUF**: Unlike `CREATE_BUFFER`, the export ioctl does not undo on `copy_to_user` failure. The fd is already installed in the process's fd table by `dma_buf_fd` before `copy_to_user`. The fd will be cleaned up on process exit.

**dma-buf stats are device-level**: All 6 dma-buf counters (exported, released, attachments, detachments, maps, unmaps) live on `dmaplane_stats_kern`, not on the per-export context. This ensures counters survive individual export/release cycles — matching the pattern used for `buffers_created`/`buffers_destroyed`.

**IB_POLL_DIRECT for CQs**: CQs are created with `IB_POLL_DIRECT` — explicit polling via `ib_poll_cq`, no softirq or workqueue callbacks. This gives deterministic, race-free teardown without explicit CQ draining. Trade-off: callers must busy-poll, acceptable for a benchmark module.

**rdma_sem protects the RDMA context**: Read lock (concurrent): MR registration/deregistration, all benchmarks. Write lock (exclusive): RDMA setup and teardown. This allows parallel benchmarks while preventing teardown from racing with in-flight operations. Ordering: `rdma_sem` (read) → `buf_lock` → `mr_lock`.

**pages_copy snapshot in MR registration**: After releasing `buf_lock`, the buffer could be destroyed, which frees the page array. `kvmalloc_array` + `memcpy` creates a local copy of page pointers. The struct page frames are stable kernel objects — `__free_page` marks them for reuse but the frame remains valid as a kernel data structure. This decouples MR registration from buffer lifetime.

**GID selection is the #1 silent failure mode with Soft-RoCE**: GID[0] on Ubuntu with stable-privacy addressing is an EUI-64 ghost address. QPs transition cleanly but traffic goes nowhere. `scan_gid_table` prefers the last link-local `ROCE_UDP_ENCAP` GID (empirically the real interface address). Falls back to index 0 with a warning.

**DMAC for loopback**: With raw verbs (no rdma_cm), the destination MAC must be populated manually in the address handle. For loopback, it's the local interface MAC extracted via `rdma_read_gid_attr_ndev_rcu` inside an RCU read section.

**Two MR paths: local_dma_lkey vs fast-reg**: Local-only access (`IB_ACCESS_LOCAL_WRITE`) uses `pd->local_dma_lkey` — a per-PD shortcut with `rkey=0` (no remote access). For remote access (`REMOTE_WRITE`, `REMOTE_READ`), a real rkey is needed from `ib_alloc_mr` + `IB_WR_REG_MR`. The `sge_addr` is set to the kernel VA for rxe compatibility (rxe interprets `sge.addr` via `local_dma_lkey` as kernel VA; real hardware uses IOMMU mapping).

**TEARDOWN_RDMA deregisters all MRs first**: MR SG table DMA mappings reference `ib_dev`. If teardown releases `ib_dev` while MRs are still mapped, `ib_dma_unmap_sg` would use-after-free. The teardown ioctl handler loops `mrs[]` under write lock and deregisters before calling `rdma_engine_teardown`.

**copy_to_user undo for SETUP_RDMA and REGISTER_MR**: If the kernel creates RDMA resources but can't tell userspace the handles (copy_to_user fails), tears them down immediately to prevent orphaned resources that userspace can never reference.

**ERR transition before QP destroy**: Transitioning QPs to `IB_QPS_ERR` before destruction causes the RDMA subsystem to flush all outstanding WRs, generating error completions. Without this, destroying a QP with outstanding WRs leaves the provider holding references to our CQE/SGE memory.

**Buffer lookup in rdma_engine.c**: `rdma_engine_register_mr` does its own inline linear scan of `edev->buffers[]` under `buf_lock` rather than calling `dmabuf_rdma_find_buffer`. This is intentional: it needs to snapshot additional fields (`pages`, `nr_pages`, `vaddr`, `size`) atomically under the same lock hold, and the find function is static in main.c.

### Phase 5: NUMA Topology & Optimization

**Files:** `driver/numa_topology.h` (declarations), `driver/numa_topology.c` (~553 lines, topology query + NxN bandwidth benchmark). Modified: `driver/dmabuf_rdma.c` (NUMA-aware allocation), `driver/main.c` (2 ioctl handlers, stats init), `include/dmaplane_uapi.h` (2 structs, 2 ioctls, buf_params + 2 fields, buf_stats + 3 fields).

**NUMA-aware buffer allocation** (`dmabuf_rdma_create_buffer`): `dmaplane_buf_params` gains `numa_node` (input) and `actual_numa_node` (output). For page-backed buffers, `alloc_page` is replaced by `alloc_pages_node(alloc_node, GFP_KERNEL | __GFP_ZERO, 0)` where `alloc_node = (target_node == DMAPLANE_NUMA_ANY) ? NUMA_NO_NODE : target_node`. NUMA_NO_NODE delegates to the current CPU's local node (same as old behavior). Post-hoc verification via `page_to_nid()` on each page tracks local vs remote counts; majority vote determines `actual_numa_node`. For coherent buffers, `dma_alloc_coherent` has no node parameter — placement is reported via `page_to_nid(virt_to_page(buf->vaddr))` for informational purposes only.

**NUMA stats are per-buffer, not per-page**: Each `CREATE_BUFFER` increments exactly one of `numa_local_allocs` (all pages on requested node), `numa_remote_allocs` (at least one page misplaced), or `numa_anon_allocs` (NUMA_ANY). Stats live on `dmaplane_stats_kern` and are exposed via `GET_BUF_STATS`.

**ABI change**: `dmaplane_buf_params` grows 16→24 bytes, `dmaplane_buf_stats` grows 16→40 bytes. The `_IOR`/`_IOWR` macros encode struct size in the ioctl number — kernel and userspace must agree or `ioctl()` returns `-ENOTTY`. All existing userspace callers updated to set `.numa_node = DMAPLANE_NUMA_ANY`. Note: `{0}` initializes `numa_node` to 0 (node 0), not -1 (DMAPLANE_NUMA_ANY) — explicit initialization is required.

**Topology query** (`dmaplane_query_numa_topo`): Enumerates online NUMA nodes with sparse-to-compact index mapping via `node_to_idx[]` (kvcalloc). Populates per-node CPU counts via `for_each_online_cpu` + `cpu_to_node`, memory via `node_present_pages` + zone walk for free pages, and the ACPI SLIT distance matrix via `node_distance(a, b)`. No lock needed — kernel topology data is static.

**Bandwidth benchmark** (`dmaplane_numa_bench`): For each (cpu_node, buf_node) pair, allocates target buffer on buf_node and source buffer on cpu_node via `alloc_pages_node` + `vmap`, spawns a kthread pinned to cpu_node via `set_cpus_allowed_ptr`, runs timed `memcpy` loop with `barrier()` after each iteration (prevents compiler optimization), and reports bandwidth (MB/s) and latency (ns/iter). Uses kthreads rather than workqueues for precise CPU affinity control. Sequential execution (not parallel) avoids cross-cell memory bus interference. Buffers allocated per-cell to avoid cache warming. Pre-touches buffers with `memset` before timing to eliminate page fault overhead.

**No lock for topology or benchmark ioctls**: Topology reads kernel-maintained data structures. The benchmark allocates temporary buffers and doesn't touch any dmaplane device state. Both ioctl structs are heap-allocated (`kmalloc`) because they exceed safe kernel stack size (~600B and ~1100B vs 8KB stack on x86).
