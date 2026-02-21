# Code Overview

Detailed analysis of every source file in the dmaplane codebase. This document grows with each phase — it describes only what has been implemented so far.

## Current State: Phase 1 — Driver Foundations & Concurrency

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

All userspace consumers (`tests/`) include this header via `-I../include`.

### `driver/dmaplane.h`

The kernel-internal header. Includes `dmaplane_uapi.h` for the shared types, then adds kernel-only structures behind `#ifdef __KERNEL__`.

**Kernel-internal types:**
- `struct dmaplane_ring`: array of `dmaplane_ring_entry[RING_SIZE]`, `spinlock_t lock`, `unsigned int head` (producer writes, `____cacheline_aligned_in_smp`), `unsigned int tail` (consumer reads, `____cacheline_aligned_in_smp`). Head and tail are monotonically increasing — modulo is applied only when indexing into the array. Full when `(head - tail) == RING_SIZE`, empty when `head == tail`.
- `struct dmaplane_channel`: `sub_ring` (submission ring), `comp_ring` (completion ring), `struct task_struct *worker` (kthread), `wait_queue_head_t wait_queue` (worker sleeps here), `unsigned int id` (channel index), `atomic_t in_flight` (submissions not yet completed), `bool shutdown` (signals worker to exit), `bool active` (slot is in use), `struct dmaplane_stats stats`.
- `struct dmaplane_dev`: `channels[DMAPLANE_MAX_CHANNELS]`, `struct mutex dev_mutex`, `struct cdev`, `struct class *`, `dev_t devno`, `struct device *`.
- `struct dmaplane_file_ctx`: `struct dmaplane_dev *dev`, `struct dmaplane_channel *chan` (NULL until CREATE_CHANNEL).
- Inline helpers: `dmaplane_ring_full()`, `dmaplane_ring_empty()`, `dmaplane_ring_count()`.

**Design notes**: Fixed-size arrays with linear scan are a deliberate simplicity trade-off — 4 channel slots is enough for Phase 1's use cases and avoids IDR complexity. The ring buffer uses unsigned arithmetic for wrap-free full/empty detection.

---

## Kernel Module (`driver/`)

### `driver/main.c`

Character device driver with ioctl dispatch and per-channel worker threads. Singleton `struct dmaplane_dev *dma_dev` at module scope.

**Module init** (`dmaplane_init`):
- `kzalloc` the device context
- `alloc_chrdev_region` for dynamic major number
- `cdev_init` + `cdev_add`
- `class_create` + `device_create` for `/dev/dmaplane` udev node
- Clean teardown in reverse order on failure (goto-based cleanup: `err_class_destroy`, `err_cdev_del`, `err_unreg_region`, `err_free_dev`)

**Module exit** (`dmaplane_exit`):
- Acquires `dev_mutex`, stops any remaining active worker kthreads via `dmaplane_channel_destroy`
- `device_destroy` + `class_destroy` + `cdev_del` + `unregister_chrdev_region` + `kfree`

**File operations:**
- `dmaplane_open`: Allocates `dmaplane_file_ctx` via `kzalloc`, stores in `filp->private_data`. Does not create a channel — that happens via ioctl.
- `dmaplane_release`: If a channel was assigned, acquires `dev_mutex`, calls `dmaplane_channel_destroy` (signals worker shutdown, `kthread_stop`), releases mutex, frees file context. Handles process exit without explicit cleanup.
- `dmaplane_ioctl`: Dispatches to per-command handlers via switch.

**Ioctl handlers:**

| Command | Handler | Description |
|---------|---------|-------------|
| `DMAPLANE_IOCTL_CREATE_CHANNEL` | `dmaplane_ioctl_create_channel` | Returns `-EBUSY` if fd already has a channel. Finds free slot (under `dev_mutex`), inits rings via `dmaplane_channel_init`, creates kthread `"dmaplane/%d"` via `kthread_create`, wakes thread, stores channel in file context, `copy_to_user` channel ID |
| `DMAPLANE_IOCTL_SUBMIT` | `dmaplane_ioctl_submit` | `copy_from_user`, acquire submission ring spinlock, check full (return `-ENOSPC`), write entry at `head % RING_SIZE`, `smp_store_release` on head, track high watermark, release lock, `atomic_inc(in_flight)`, increment `total_submissions`, `wake_up_interruptible` worker |
| `DMAPLANE_IOCTL_COMPLETE` | `dmaplane_ioctl_complete` | Acquire completion ring spinlock, check empty (return `-EAGAIN`), read entry at `tail % RING_SIZE`, `smp_store_release` on tail, release lock, `copy_to_user` |
| `DMAPLANE_IOCTL_GET_STATS` | `dmaplane_ioctl_get_stats` | `copy_to_user` the channel's `dmaplane_stats` struct |

**Worker thread** (`dmaplane_worker_fn`):
- Loops until `kthread_should_stop()`
- Sleeps on channel wait queue: condition is `(submission ring not empty) || kthread_should_stop()`
- On shutdown with empty ring, breaks out cleanly
- Drains submissions one at a time: acquires sub_ring lock, re-checks empty under lock, reads entry at `tail % RING_SIZE`, advances tail via `smp_store_release`, releases lock
- Processes entry (increments payload by 1)
- Pushes to completion ring: acquires comp_ring lock, checks full. **If completion ring is full, releases lock and yields via `cond_resched()` in a retry loop** (checking `kthread_should_stop` each iteration). This prevents entry loss under backpressure when userspace drains completions slower than the worker produces them.
- Decrements `in_flight`, increments `total_completions`
- Calls `cond_resched()` every 64 entries to yield CPU

**Channel lifecycle:**
- `dmaplane_channel_init`: zeroes rings, `spin_lock_init` both ring locks, `init_waitqueue_head`, clears stats
- `dmaplane_channel_destroy`: sets `shutdown = true`, `wake_up_interruptible`, `kthread_stop` (blocking wait for thread exit), sets `active = false`

**Locking model:**
- Submission ring spinlock (`sub_ring.lock`): protects ring entries between ioctl submit (producer, advances head) and worker thread (consumer, advances tail). Comment in `dmaplane.h`: "sub_ring.lock protects ring entries between the ioctl submit path (producer, advances head) and the worker thread (consumer, advances tail)."
- Completion ring spinlock (`comp_ring.lock`): protects ring entries between worker thread (producer, advances head) and ioctl complete (consumer, advances tail). Comment in `dmaplane.h`: "comp_ring.lock protects ring entries between the worker thread (producer, advances head) and the ioctl complete path (consumer, advances tail)."
- Device mutex (`dev_mutex`): protects channel slot allocation/deallocation. Sleeping context OK — only acquired from ioctl and release (process context). Comment in `dmaplane.h`: "Protects channel slot allocation/deallocation. Sleeping context OK — only acquired from ioctl (process context)."
- Worker shutdown: `channel->shutdown = true` + `wake_up_interruptible` + `kthread_stop` (blocking wait for thread exit). The completion ring yield loop also checks `kthread_should_stop` to ensure the worker exits even if the ring stays full.

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

---

## Cross-cutting Observations

**Why ioctl, not read/write**: The driver uses `unlocked_ioctl` for all operations rather than `read`/`write` file operations. This is deliberate — each operation has structured input/output parameters that don't map cleanly to byte streams. The ioctl interface extends naturally as phases add new commands (DMA allocation, RDMA, GPU pinning) without changing the file operations vtable.

**Ring buffer design**: Monotonically increasing head/tail with modulo indexing avoids the "is the ring full or empty?" ambiguity that plagues wrapped-index designs. `smp_store_release` on head and tail updates ensures the other side sees consistent ring entries on weakly-ordered architectures (relevant for ARM, no-op on x86 but correct to include). Head and tail on separate cache lines (`____cacheline_aligned_in_smp`) prevent false sharing between producer and consumer.

**Completion ring backpressure**: The original design dropped entries when the completion ring was full. Testing under load (4-channel × 250K stress test) revealed that the worker could outpace userspace drain, causing silent completion loss and payload mismatches. The fix: the worker yields (`cond_resched` loop) when the completion ring is full, applying backpressure through the submission ring — if the completion ring is full, the worker blocks, the submission ring fills, and userspace gets `-ENOSPC`, forcing it to drain completions.

**Worker thread as processing stub**: The worker's "processing" (increment payload by 1) is intentionally trivial. The value of Phase 1 is the infrastructure — rings, locking, kthreads, ioctl dispatch, lifecycle management. Future phases replace the stub with real DMA operations, RDMA posts, and GPU transfers, but the ring/worker/ioctl scaffolding remains unchanged.

**Error paths**: All error paths use goto-based cleanup with reverse-order resource release. This pattern will be used throughout the project.

**Log prefix**: All kernel messages use the `dmaplane:` prefix via `pr_fmt(fmt) KBUILD_MODNAME ": " fmt`. Lifecycle messages use `pr_info`, per-operation messages use `pr_debug`.
