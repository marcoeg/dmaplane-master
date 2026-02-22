# Code Overview

Detailed analysis of every source file in the dmaplane codebase. This document grows with each phase — it describes only what has been implemented so far.

## Current State: Phase 3 — dma-buf Export & Zero-Copy Sharing

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
- Buffer creation parameters (`dmaplane_buf_params`): `__u32 buf_id` (out), `__u32 alloc_type` (in), `__u64 size` (in)
- mmap info (`dmaplane_mmap_info`): `__u32 buf_id` (in), `__u64 mmap_offset` (out), `__u64 mmap_size` (out)
- Buffer stats (`dmaplane_buf_stats`): `__u64 buffers_created`, `__u64 buffers_destroyed`
- Ioctl commands: `DMAPLANE_IOCTL_CREATE_BUFFER` (`_IOWR`, 0x05), `DMAPLANE_IOCTL_DESTROY_BUFFER` (`_IOW`, 0x06, bare `__u32`), `DMAPLANE_IOCTL_GET_MMAP_INFO` (`_IOWR`, 0x08), `DMAPLANE_IOCTL_GET_BUF_STATS` (`_IOR`, 0x09)

**Phase 3 additions:**
- Export parameters (`dmaplane_export_dmabuf_arg`): `__u32 buf_id` (in), `__s32 fd` (out)
- dma-buf stats (`dmaplane_dmabuf_stats`): 6 `__u64` counters — `dmabufs_exported`, `dmabufs_released`, `attachments_total`, `detachments_total`, `maps_total`, `unmaps_total`
- Ioctl commands: `DMAPLANE_IOCTL_EXPORT_DMABUF` (`_IOWR`, 0x0A), `DMAPLANE_IOCTL_GET_DMABUF_STATS` (`_IOR`, 0x0B)

All userspace consumers (`tests/`, `examples/misc/`) include this header via `-I../include` or `-I../../include`.

### `driver/dmaplane.h`

The kernel-internal header. Includes `dmaplane_uapi.h` for the shared types, then adds kernel-only structures behind `#ifdef __KERNEL__`.

**Kernel-internal types:**
- `struct dmaplane_ring`: array of `dmaplane_ring_entry[RING_SIZE]`, `spinlock_t lock`, `unsigned int head` (producer writes, `____cacheline_aligned_in_smp`), `unsigned int tail` (consumer reads, `____cacheline_aligned_in_smp`). Head and tail are monotonically increasing — modulo is applied only when indexing into the array. Full when `(head - tail) == RING_SIZE`, empty when `head == tail`.
- `struct dmaplane_stats_kern`: kernel-internal stats with `atomic64_t total_submissions`, `atomic64_t total_completions`, `atomic_t ring_high_watermark`, `atomic_t dropped_count`, `atomic64_t buffers_created`, `atomic64_t buffers_destroyed`, plus Phase 3 dma-buf counters: `atomic64_t dmabufs_exported`, `dmabufs_released`, `dmabuf_attachments`, `dmabuf_detachments`, `dmabuf_maps`, `dmabuf_unmaps`. All dma-buf counters are device-level (survive individual export/release cycles). Safe for concurrent updates and reads.
- `struct dmaplane_channel`: `struct kref refcount` (channel lifetime), `struct mutex lock` (per-channel state lock), `sub_ring` (submission ring), `comp_ring` (completion ring), `struct task_struct *worker` (kthread), `wait_queue_head_t wait_queue` (worker sleeps here), `unsigned int id` (channel index), `atomic_t in_flight` (submissions not yet completed), `bool shutdown` (signals worker to exit), `bool active` (slot is in use, set to false by kref release callback), `struct dmaplane_stats_kern stats` (atomic counters).
- `struct dmaplane_buffer` (Phase 2+3): per-buffer tracking. `unsigned int id` (handle, never 0), `int alloc_type` (`BUF_TYPE_COHERENT` or `BUF_TYPE_PAGES`), `size_t size`, `bool in_use` (protected by `buf_lock`), `void *vaddr` (kernel VA — from `dma_alloc_coherent` or `vmap`), `dma_addr_t dma_handle` (coherent only), `struct page **pages` + `unsigned int nr_pages` (page-backed only), `atomic_t mmap_count` (active userspace mappings, prevents destroy while mapped). Phase 3 additions: `bool dmabuf_exported` (prevents double export and blocks DESTROY_BUFFER while a dma-buf wraps this buffer, set/cleared under `buf_lock`), `struct dma_buf *dmabuf` (back-pointer, NULL when not exported).
- `struct dmaplane_dev`: `struct platform_device *pdev` (DMA-capable device for all DMA API calls), `channels[DMAPLANE_MAX_CHANNELS]`, `struct mutex dev_mutex`, `struct cdev`, `struct class *`, `dev_t devno`, `struct device *`, `buffers[DMAPLANE_MAX_BUFFERS]` (64 slots), `struct mutex buf_lock` (protects buffer array), `unsigned int next_buf_id` (monotonically increasing, skips 0), `struct dmaplane_stats_kern stats` (shared atomic counters), plus device-level atomic counters: `atomic_t active_channels`, `atomic64_t total_opens`, `atomic64_t total_closes`, `atomic64_t total_channels_created`, `atomic64_t total_channels_destroyed`. Counters are printed via `pr_info` at module unload; Phase 7 will export through debugfs.
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
- `mutex_init` for `buf_lock`, `next_buf_id = 1`
- `alloc_chrdev_region` for dynamic major number
- `cdev_init` + `cdev_add`
- `class_create` + `device_create` for `/dev/dmaplane` udev node
- Clean teardown in reverse order on failure (goto-based cleanup: `err_class_destroy`, `err_cdev_del`, `err_unreg_region`, `err_unreg_pdev`, `err_put_pdev`, `err_free_dev`). Distinction: `platform_device_put` for devices allocated but never added; `platform_device_unregister` for devices both allocated and added.

**Module exit** (`dmaplane_exit`) — ordering is critical:
1. Acquires `dev_mutex`, stops any remaining active worker kthreads via `dmaplane_channel_destroy`
2. Prints lifetime summary via `pr_info`: total opens, closes, channels created, channels destroyed, buffers created, buffers destroyed
3. Tears down char device (`device_destroy` + `class_destroy` + `cdev_del` + `unregister_chrdev_region`) — prevents new ioctls/opens from racing with cleanup
4. Destroys remaining buffers — warns on leaked mmap references (`mmap_count > 0`) and leaked dma-buf exports (`dmabuf_exported`). Must happen AFTER char device teardown (prevents racing creates) but BEFORE platform device unregister (`dma_free_coherent` needs the platform device alive)
5. `platform_device_unregister`
6. `kfree`

**File operations:**
- `dmaplane_open`: Checks `capable(CAP_SYS_ADMIN)` — DMA buffer allocation and mmap of kernel pages are privileged operations. Allocates `dmaplane_file_ctx` via `kzalloc`, stores in `filp->private_data`. Increments `total_opens`.
- `dmaplane_release`: If a channel was assigned, acquires `dev_mutex`, calls `dmaplane_channel_destroy` (signals worker shutdown, `kthread_stop`, drops both krefs), releases mutex, frees file context. Increments `total_closes`. Handles process exit without explicit cleanup.
- `dmaplane_ioctl`: Dispatches to per-command handlers via switch (Phase 1: 0x01–0x04, Phase 2: 0x05–0x09, Phase 3: 0x0A–0x0B).
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
| `DMAPLANE_IOCTL_GET_BUF_STATS` | `dmaplane_ioctl_get_buf_stats` | Reads `buffers_created` and `buffers_destroyed` atomics into `dmaplane_buf_stats` UAPI struct |
| `DMAPLANE_IOCTL_EXPORT_DMABUF` | `dmaplane_ioctl_export_dmabuf` | `copy_from_user`, delegates to `dmaplane_dmabuf_export` (holds `buf_lock` for entire operation), `copy_to_user`. **No copy_to_user undo** — fd already installed in process fd table by `dma_buf_fd` |
| `DMAPLANE_IOCTL_GET_DMABUF_STATS` | `dmaplane_ioctl_get_dmabuf_stats` | Reads 6 device-level dma-buf atomic counters into `dmaplane_dmabuf_stats` UAPI struct |

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

**Locking model (5 locks):**
- Device mutex (`dev_mutex`): protects channel slot allocation/deallocation. Sleeping context OK — only acquired from ioctl and release (process context). Outermost lock in the ordering. Independent of `buf_lock` (never nested).
- Buffer mutex (`buf_lock`): protects `buffers[]` array — slot allocation, lookup, deallocation, and mmap. Sleeping context OK. Independent of `dev_mutex` (never nested). In Phase 4+, ordering will be: `rdma_sem` (read) → `buf_lock`.
- Per-channel mutex (`channel->lock`): protects channel state transitions (shutdown flag, worker pointer). Separate from ring spinlocks (which protect ring data) and from dev_mutex (which protects slot allocation). Ordering: `dev_mutex` → `channel->lock` → ring spinlocks.
- Submission ring spinlock (`sub_ring.lock`): protects ring entries between ioctl submit (producer, advances head) and worker thread (consumer, advances tail).
- Completion ring spinlock (`comp_ring.lock`): protects ring entries between worker thread (producer, advances head) and ioctl complete (consumer, advances tail).
- Worker shutdown: acquire `channel->lock`, set `channel->shutdown = true`, release lock, `wake_up_interruptible` + `kthread_stop` (blocking wait for thread exit). The completion ring yield loop also checks `kthread_should_stop` to ensure the worker exits even if the ring stays full.

---

### `driver/dmabuf_rdma.h`

Header declaring three buffer management functions with locking contracts: `dmabuf_rdma_create_buffer` and `dmabuf_rdma_destroy_buffer` acquire `buf_lock` internally; `dmabuf_rdma_find_buffer` requires the caller to hold `buf_lock` for the pointer's lifetime.

### `driver/dmabuf_rdma.c`

Buffer allocation and lookup. Two allocation paths:

**`dmabuf_rdma_create_buffer`**:
- Validates size (> 0, <= 1 GB) and alloc_type. Acquires `buf_lock`, linear scan for free slot.
- Zeroes the slot via `memset` to clear stale pointers from a previous occupant.
- Assigns `next_buf_id++` (wraps to 1, skipping 0 — sentinel for "no buffer").
- **Coherent path** (`BUF_TYPE_COHERENT`): `dma_alloc_coherent(&dev->pdev->dev, ...)` + `memset` zero. Returns physically contiguous, cache-coherent memory with a single DMA address.
- **Page-backed path** (`BUF_TYPE_PAGES`): `kvcalloc` page array, `alloc_page(GFP_KERNEL | __GFP_ZERO)` loop with unwind on failure (frees all previously allocated pages), `vmap()` for contiguous kernel VA. `__GFP_ZERO` prevents leaking kernel data to userspace via mmap.
- All three error paths (no slots, allocation failure, vmap failure) properly unwind with reverse-order cleanup.

**`dmabuf_rdma_destroy_buffer`**:
- Acquires `buf_lock`, finds buffer. Refuses with `-EBUSY` if `mmap_count > 0` or `dmabuf_exported` (Phase 3 guard — buffer cannot be destroyed while a dma-buf wraps it).
- Pages: `vunmap` → `__free_page` loop → `kvfree(pages)`.
- Coherent: `dma_free_coherent(&dev->pdev->dev, ...)`.

**`dmabuf_rdma_find_buffer`**: Linear scan. Caller must hold `buf_lock`.

All three are `EXPORT_SYMBOL_GPL` for use by future modules (Phase 4 rdma_engine).

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
