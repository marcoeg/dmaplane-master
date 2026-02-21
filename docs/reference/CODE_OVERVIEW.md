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

### `driver/dmaplane.h`

The central header shared between the kernel module and test programs. Defines the ioctl contract.

**UAPI section** (exposed to both kernel and userspace):
- Ioctl magic `0xE4` with Phase 1 commands: `IOCTL_CREATE_CHANNEL` (`0x01`), `IOCTL_SUBMIT` (`0x02`), `IOCTL_COMPLETE` (`0x03`), `IOCTL_GET_STATS` (`0x04`)
- Ring entry struct: `u64 payload`, `u32 flags`
- Submit/complete parameter structs passed through ioctl
- Channel stats struct: total submissions, total completions, ring high watermark, dropped count
- Constants: `DMAPLANE_MAX_CHANNELS` (4), `DMAPLANE_RING_SIZE` (1024, power of 2)

**Kernel-internal section** (`#ifdef __KERNEL__`):
- Ring buffer struct: array of entries, `head` and `tail` indices on separate cache lines (`____cacheline_aligned_in_smp`), spinlock. Head and tail are monotonically increasing — modulo is applied only when indexing into the array. Full when `(head - tail) == RING_SIZE`, empty when `head == tail`.
- Channel struct: submission ring, completion ring, `struct task_struct *worker` (kthread), `wait_queue_head_t` (worker sleeps here), channel ID, `atomic_t in_flight`, `bool shutdown` flag.
- Device context struct: array of channels, `struct cdev`, `struct class *`, `dev_t`, `struct device *`, `struct mutex` for device-level operations (channel allocation).
- File context struct: pointer to device context, pointer to assigned channel (one channel per open fd).

**Design notes**: Fixed-size arrays with linear scan are a deliberate simplicity trade-off — 4 channel slots is enough for Phase 1's use cases and avoids IDR complexity. The ring buffer uses unsigned arithmetic for wrap-free full/empty detection.

### `include/dmaplane_uapi.h`

Userspace API header extracted from the UAPI section of `driver/dmaplane.h`. Contains only types safe for userspace inclusion: ioctl command defines, parameter structs, constants. All userspace consumers (`tests/`) include this header via `-I../include`. Must be kept in sync with `driver/dmaplane.h` when ioctl structures change.

---

## Kernel Module (`driver/`)

### `driver/main.c`

Character device driver with ioctl dispatch and per-channel worker threads.

**Module init** (`dmaplane_init`):
- `alloc_chrdev_region` for dynamic major number
- `class_create` + `device_create` for `/dev/dmaplane` udev node
- `cdev_init` + `cdev_add`
- Clean teardown in reverse order on failure (goto-based cleanup)

**Module exit** (`dmaplane_exit`):
- Stops any remaining worker kthreads
- `device_destroy` + `class_destroy` + `cdev_del` + `unregister_chrdev_region`

**File operations:**
- `open`: Allocates file context, stores in `filp->private_data`. Does not create a channel — that happens via ioctl.
- `release`: If a channel was assigned, signals worker shutdown, calls `kthread_stop`, frees channel resources, frees file context. Handles process exit without explicit cleanup.
- `unlocked_ioctl`: Dispatches to per-command handlers.

**Ioctl handlers:**

| Command | Handler | Description |
|---------|---------|-------------|
| `IOCTL_CREATE_CHANNEL` | `dmaplane_create_channel` | Finds free slot (under device mutex), inits rings, creates kthread `"dmaplane/%d"`, wakes thread, stores channel in file context, returns channel ID |
| `IOCTL_SUBMIT` | `dmaplane_submit` | `copy_from_user`, acquire submission ring spinlock, check full, write entry, `smp_store_release` on head, release lock, `wake_up_interruptible` worker, update stats |
| `IOCTL_COMPLETE` | `dmaplane_complete` | Acquire completion ring spinlock, check empty (return `-EAGAIN`), read entry, advance tail, release lock, `copy_to_user` |
| `IOCTL_GET_STATS` | `dmaplane_get_stats` | Copy channel stats to userspace |

**Worker thread** (`dmaplane_worker_fn`):
- Loops until `kthread_should_stop()`
- Sleeps on channel wait queue: condition is `(submission ring not empty) || kthread_should_stop()`
- Drains submissions: reads from submission ring tail, processes (increments payload by 1), pushes result to completion ring head
- Calls `cond_resched()` periodically to yield CPU

**Locking model:**
- Submission ring spinlock: protects ring entries between userspace writer (head) and worker reader (tail). Comment: "Protects submission ring entries. Acquired by ioctl submit (producer) and worker thread (consumer)."
- Completion ring spinlock: protects ring entries between worker writer (head) and userspace reader (tail). Comment: "Protects completion ring entries. Acquired by worker thread (producer) and ioctl complete (consumer)."
- Device mutex: protects channel array during allocation/deallocation. Comment: "Protects channel slot allocation. Sleeping context OK — only acquired from ioctl."
- Worker shutdown: `channel->shutdown = true` + `wake_up` + `kthread_stop` (blocking wait for thread exit).

---

## Tests (`tests/`)

### `tests/test_phase1_driver.c`

Userspace stress test for the driver. Opens `/dev/dmaplane`, exercises all ioctls, validates correctness under load.

**Test cases:**
1. Basic open/close — no crash
2. Channel creation — verify channel IDs 0, 1
3. Submit and complete — roundtrip with payload verification (worker increments by 1)
4. Ring full behavior — submit `RING_SIZE` entries, verify `-ENOSPC` on overflow
5. Single-channel stress — 1M submit/complete cycles, verify count match
6. Multi-channel stress — 4 channels × 250K entries via pthreads, verify no cross-contamination
7. Cleanup on close — submit without completing, close fd, verify no kernel warnings

Reports PASS/FAIL per test, throughput for stress tests, and a summary.

---

## Cross-cutting Observations

**Why ioctl, not read/write**: The driver uses `unlocked_ioctl` for all operations rather than `read`/`write` file operations. This is deliberate — each operation has structured input/output parameters that don't map cleanly to byte streams. The ioctl interface extends naturally as phases add new commands (DMA allocation, RDMA, GPU pinning) without changing the file operations vtable.

**Ring buffer design**: Monotonically increasing head/tail with modulo indexing avoids the "is the ring full or empty?" ambiguity that plagues wrapped-index designs. `smp_store_release` on head updates ensures the worker sees consistent ring entries on weakly-ordered architectures (relevant for ARM, no-op on x86 but correct to include).

**Worker thread as processing stub**: The worker's "processing" (increment payload by 1) is intentionally trivial. The value of Phase 1 is the infrastructure — rings, locking, kthreads, ioctl dispatch, lifecycle management. Future phases replace the stub with real DMA operations, RDMA posts, and GPU transfers, but the ring/worker/ioctl scaffolding remains unchanged.

**Error paths**: All error paths use goto-based cleanup with reverse-order resource release. This pattern will be used throughout the project.

**Log prefix**: All kernel messages use the `dmaplane:` prefix via `pr_fmt(fmt) KBUILD_MODNAME ": " fmt`.
