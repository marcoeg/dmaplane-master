# Building a Linux Kernel Driver: Rings, Workers, and the Locking That Holds It Together

*Part 1 of 9 in a series on building a host-side data path emulator for AI infrastructure*

---

Every system that moves tensors between GPUs over a network relies on the same kernel-level plumbing: a character device for control, ring buffers for command submission and completion, worker threads for asynchronous processing, and a locking discipline that prevents all of it from corrupting itself under concurrency. NVMe storage, network packet processing, GPU command submission, and the RDMA transports that synchronize gradients across thousand-GPU training clusters all use this pattern.

This post builds it from scratch as a loadable kernel module. By the end you will understand how ring buffers work in kernel space, why the locking is shaped the way it is, how a worker thread drains a submission queue and posts completions, and how backpressure propagates when the consumer cannot keep up. This is the scaffolding that later phases use to move real DMA buffers, register RDMA memory regions, and transfer KV-cache data between disaggregated inference nodes.

---

## The Character Device: Structured Control via Ioctl

A kernel module becomes useful to userspace through a device node — a file in `/dev/` that programs can `open`, `read`, `write`, and `ioctl`. For a data-path driver, the character device is the control plane.

Each dmaplane operation has structured input and output: a submission carries a `payload` and `flags`, a completion returns a transformed entry, stats return a set of counters. These do not map cleanly to byte streams. With `read`/`write`, you need framing, serialization, and a protocol. With ioctl, each command is a typed function call: the kernel knows the struct layout from the command number (which encodes direction and size), copies the right number of bytes, and dispatches to the right handler.

The ioctl interface also extends naturally. Phase 2 adds `IOCTL_CREATE_BUFFER`. Phase 4 adds `IOCTL_RDMA_WRITE_IMM`. Phase 8 adds `IOCTL_GPU_PIN`. Each new command is a new case in the dispatch switch — the file operations vtable never changes. This is how InfiniBand verbs work at the kernel level: a single character device, dozens of ioctl commands, each with its own typed parameter struct.

The command encoding carries safety properties:

```c
#define DMAPLANE_IOCTL_SUBMIT \
    _IOW(DMAPLANE_IOC_MAGIC, 0x02, struct dmaplane_submit_params)
```

`_IOW` encodes the direction (write from user to kernel), the magic number (`0xE4`), the command ordinal, and the struct size into a single 32-bit integer. If userspace passes the wrong struct (different size due to a version mismatch), the kernel returns `-ENOTTY` rather than silently corrupting memory. This fail-fast property matters when the Python wrapper comes in Phase 9.

Registration requires four steps in dependency order, and teardown requires reversing that order exactly:

```c
/* Init: create resources in dependency order */
alloc_chrdev_region(&dev->devno, 0, 1, "dmaplane");
dev->class = class_create("dmaplane");
cdev_init(&dev->cdev, &dmaplane_fops);
cdev_add(&dev->cdev, dev->devno, 1);
device_create(dev->class, NULL, dev->devno, ...);

/* Exit: destroy in reverse */
device_destroy(dev->class, dev->devno);
cdev_del(&dev->cdev);
class_destroy(dev->class);
unregister_chrdev_region(dev->devno, 1);
```

Every error path during init must undo only the resources that were successfully created. The standard kernel pattern is goto-based cleanup:

```c
ret = alloc_chrdev_region(&dev->devno, 0, 1, "dmaplane");
if (ret)
    goto err_free_dev;

dev->class = class_create("dmaplane");
if (IS_ERR(dev->class)) {
    ret = PTR_ERR(dev->class);
    goto err_unreg_region;
}
/* ... */
return 0;

err_unreg_region:
    unregister_chrdev_region(dev->devno, 1);
err_free_dev:
    kfree(dev);
    return ret;
```

This is not defensive programming — it is the only correct approach. Kernel code cannot leak. There is no garbage collector. If `class_create` fails and you return without calling `unregister_chrdev_region`, that major number is gone until reboot.

---

## The Ring Buffer: Monotonic Indices and Unsigned Arithmetic

The submission and completion rings are the central data structure. Each is a fixed-size array of 1024 entries with a `head` (producer writes here) and `tail` (consumer reads here):

```c
struct dmaplane_ring {
    struct dmaplane_ring_entry entries[DMAPLANE_RING_SIZE];
    spinlock_t lock;
    unsigned int head ____cacheline_aligned_in_smp;
    unsigned int tail ____cacheline_aligned_in_smp;
};
```

**Monotonic indices.** Head and tail are unsigned integers that increase forever. They never wrap explicitly. To index into the array, apply modulo: `entries[head % RING_SIZE]`. To check fullness, compute `head - tail == RING_SIZE`.

The alternative — wrapping head and tail at `RING_SIZE` — creates an ambiguity: when `head == tail`, is the ring full or empty? Both states produce the same index values. The standard workaround is to sacrifice one slot (full when `(head + 1) % SIZE == tail`), which wastes memory and adds a modulo on every full check. With monotonic indices and unsigned subtraction, the occupancy is always `head - tail`. Full is `== RING_SIZE`, empty is `== 0`. No ambiguity, no wasted slot.

This works because unsigned subtraction handles wrap correctly. If head is `UINT_MAX` and tail is `UINT_MAX - 5`, the difference is 5 — five entries in the ring — regardless of the unsigned wrap. The ring size must be a power of 2, making the modulo a cheap bitwise AND.

**Memory ordering.** After writing a ring entry and before advancing the index, the producer must ensure the data is visible to the consumer before the index update:

```c
ring->entries[ring->head % DMAPLANE_RING_SIZE] = entry;
smp_store_release(&ring->head, ring->head + 1);
```

`smp_store_release` guarantees that all preceding writes (the entry data) are visible to other CPUs before the store to head becomes visible. On x86 (Total Store Order), this compiles to a plain `mov` — the hardware already guarantees store ordering. On ARM64 (weakly ordered memory model), it compiles to `stlr`, a store-release instruction that is a real barrier. Without it, the consumer could see the advanced index before the entry contents are visible in memory, read garbage, and produce silent data corruption with no lockdep warning and no WARN_ON. The barrier is not defensive — it is required for architectural correctness on weakly ordered platforms.

**Cache line separation.** If head and tail share a cache line, every time the producer writes head, the consumer's cache line is invalidated — even though the consumer only reads tail. This is false sharing, and it can cut throughput in half on a multi-core machine.

`____cacheline_aligned_in_smp` pads each field to the kernel's compile-time `SMP_CACHE_BYTES`, which equals `L1_CACHE_BYTES` for the target architecture — 64 bytes on x86 and most ARM64 cores, 128 bytes on some Apple Silicon and ARM server parts. The kernel knows its own cache line size; the annotation does the right thing regardless of which architecture you compile for. On a single-CPU (UP) kernel, the annotation is a no-op — there is no other cache to invalidate.

---

## The Two-Ring Architecture

The driver uses two rings per channel. The submission ring carries commands from userspace to the kernel worker; the completion ring carries results back:

```
Submission ring:   userspace (producer → head)  ───→  worker (consumer → tail)
Completion ring:   worker (producer → head)     ───→  userspace (consumer → tail)
```

Each ring has its own spinlock. The submission ring lock serializes the ioctl submit path (which advances head) against the worker thread (which advances tail). The completion ring lock serializes the worker (advancing head) against the ioctl complete path (advancing tail). These locks are never nested — no code path ever holds both simultaneously.

Spinlocks rather than mutexes because each critical section is a few instructions: copy one entry, advance one index. A mutex would add sleep/wake overhead that exceeds the time spent in the critical section. Spinlocks are the right tool when the hold time is short and contention is low.

---

## Locking: What Each Lock Protects and Why

The concurrency model has three lock types, each protecting a distinct resource:

**Submission ring spinlock** (`sub_ring.lock`): Protects ring entries between the ioctl submit path (producer) and the worker thread (consumer). Both sides acquire, perform one ring operation, and release. Hold time is microseconds.

**Completion ring spinlock** (`comp_ring.lock`): Same pattern, opposite direction. The worker produces completions, userspace consumes them.

**Device mutex** (`dev_mutex`): Protects the channel slot array during allocation and deallocation. This is a sleeping lock because channel creation calls `kthread_create`, which may sleep. Mutexes are appropriate in process context (ioctl handlers run in the calling process's context); spinlocks would be wrong here because sleeping under a spinlock is a BUG — the kernel panics.

Every lock has a comment in the header documenting what it protects, who acquires it, and in what context:

```c
/*
 * sub_ring.lock — protects ring entries between the ioctl submit
 * path (producer, advances head) and the worker thread (consumer,
 * advances tail). Acquired from process context (ioctl) and
 * kthread context.
 */
```

This is not decoration. When the developer adding Phase 4 needs to know whether `dev_mutex` can be acquired from an RDMA completion callback (it cannot — callbacks run in softirq context, where sleeping is forbidden), the lock comment provides the answer immediately.

**Lock ordering:** `dev_mutex` → `channel->lock` → ring spinlocks. The current code never nests locks, but documenting the ordering prevents future phases from introducing deadlocks as the code grows. `lockdep`, the kernel's runtime lock validator, enforces consistency at runtime.

---

## Worker Threads: Lifecycle, Wait Queues, and Shutdown

Each channel gets a kernel thread named `dmaplane/N`. The thread sleeps on a wait queue until the submission ring is non-empty:

```c
wait_event_interruptible(chan->wait_queue,
    !dmaplane_ring_empty(sub) || kthread_should_stop());
```

`wait_event_interruptible` atomically checks the condition and goes to sleep if it is false. "Atomically" means there is no window where a wake-up can be lost between checking and sleeping — the wait queue mechanism handles this with internal spinlocks. Without this guarantee, the classic lost-wakeup race occurs: the worker checks the ring (empty), the producer adds an entry and calls `wake_up` (nobody sleeping yet), the worker goes to sleep (forever).

The condition checks two things: work available, or shutdown requested. The worker drains submissions one at a time: acquire the submission ring lock, re-check emptiness under the lock (the unlocked check in the while condition is an optimization — the authoritative check is under the lock), read the entry, advance tail, release the lock, process the entry, push to the completion ring.

---

## Backpressure: Why the Worker Must Yield, Not Drop

When the completion ring is full — userspace has not called `COMPLETE` to drain results — the worker has two options: drop the completion, or block until space opens.

Dropping seems reasonable. If userspace is not reading completions, that is userspace's problem. But under load, the worker outpaces the drain. In a multi-channel stress test (4 channels × 250K entries via pthreads), each thread submits in batches, then switches to draining. With 4 workers running concurrently, they fill the completion rings before the threads switch modes. Dropped entries cause the next completion read to be dozens of entries ahead of where the consumer expects — silent data loss, detectable only by payload mismatch.

The correct approach: the worker yields until space opens:

```c
for (;;) {
    spin_lock_irqsave(&comp->lock, flags);
    if (!dmaplane_ring_full(comp)) {
        comp->entries[comp->head % DMAPLANE_RING_SIZE] = entry;
        smp_store_release(&comp->head, comp->head + 1);
        spin_unlock_irqrestore(&comp->lock, flags);
        break;
    }
    spin_unlock_irqrestore(&comp->lock, flags);

    if (kthread_should_stop())
        goto done;
    cond_resched();
}
```

This creates natural backpressure through the ring chain. Completion ring full → worker stops draining → submission ring fills → `SUBMIT` ioctl returns `-ENOSPC` → userspace must call `COMPLETE` before submitting more. No data is lost, no entries are dropped, and the system self-regulates.

The `kthread_should_stop()` check inside the yield loop is critical. Without it, closing a file descriptor with a full completion ring hangs indefinitely: `kthread_stop` blocks waiting for the worker, but the worker is stuck yielding on a full ring that nobody will ever drain.

This same pattern — producer yields when the downstream queue is full — appears in RDMA credit-based flow control (Phase 6) and CQ overflow handling (Phase 4). The mechanism differs, but the principle is the same: never drop data silently.

---

## Channel Lifecycle and Reference Counting

The hardest part of the driver is not the data path — it is ensuring the channel struct is not freed while the worker thread still holds a pointer to it.

When userspace closes the file descriptor, `dmaplane_release` fires. It must signal the worker to stop, wait for it to exit, then free the channel. But the worker is asynchronous — it might be mid-operation when the shutdown signal arrives. Between "worker is told to stop" and "worker has actually stopped," any access to the channel struct is a use-after-free.

The solution is `kref` — the kernel's reference counting primitive:

```c
struct dmaplane_channel {
    struct kref refcount;
    /* ... */
};
```

Two holders take references: the file context (taken at channel creation, dropped at file close) and the worker thread (taken at kthread start, dropped at kthread exit). The channel struct is freed only when both references are dropped.

The shutdown sequence:

1. `dmaplane_release` sets `shutdown = true` and wakes the worker.
2. `kthread_stop()` blocks until the worker function returns.
3. The worker's last act is `kref_put()`.
4. `dmaplane_release` calls `kref_put()`.
5. When the refcount hits zero, the release callback cleans up.

This lifecycle management grows in later phases. When a channel has a DMA buffer mapped and an RDMA memory region registered, the release path must tear down in dependency order: deregister MR, unmap buffer, free buffer, stop worker, free channel. Reference counting enforces this ordering.

---

## Process Exit and the Release Path

The `release` file operation fires when the last reference to the fd drops — including process exit without an explicit `close`. The release path handles active channels with unprocessed entries:

1. Acquire `dev_mutex`.
2. Set `shutdown = true`.
3. Wake the worker.
4. `kthread_stop` — blocks until the worker returns.
5. Mark the channel slot inactive.
6. Free the file context.

`kthread_stop` is the synchronization point. After it returns, the worker is guaranteed to have exited. Without it, the worker could be mid-operation when the channel struct is freed.

---

## Stress Test Results

The test suite escalates from basic sanity to multi-channel stress:

| Test | Description | Key Metric |
|------|-------------|------------|
| 1. Open/close | Empty lifecycle | No crash |
| 2. Channel creation | Unique sequential IDs | Correct ID assignment |
| 3. Roundtrip | Submit → worker → complete | Payload incremented by 1 |
| 4. Ring full | Fill until `-ENOSPC`, drain | All entries recovered |
| 5. Single-channel stress | 1M submit/complete cycles | ~3.7M ops/sec |
| 6. Multi-channel stress | 4 channels × 250K entries | No cross-contamination |
| 7. Cleanup on close | Submit without completing, close fd | No kernel warnings |

Test 6 is the most important. Each thread's payloads are tagged with the channel ID, so any locking failure that leaks data across channels produces a detectable mismatch. Test 7 validates the shutdown path — it exercises the exact scenario where the worker's yield loop must check `kthread_should_stop()`.

The single-channel throughput of ~3.7M ops/sec is the overhead floor: two ioctls per operation (submit + complete), two spinlock acquisitions per ioctl, a context switch to the worker, and the worker's spinlock acquisitions. When real DMA or RDMA operations replace the trivial payload increment, the per-operation cost is dominated by actual data movement. But knowing the scaffolding cost tells us the minimum latency the framework adds on top of any operation.

---

## Connection to What Comes Next

The worker thread currently increments a payload by one. That is intentionally trivial. The value of Phase 1 is the infrastructure around it: rings, locking, kthread lifecycle, ioctl dispatch, backpressure, and clean shutdown.

Phase 2 adds DMA buffer allocation — coherent and page-backed — and mmap for zero-copy userspace access. The ring/worker/ioctl scaffolding does not change. The stub gets replaced; the plumbing remains. This is the pattern through all nine phases.

---

*Next: [Part 2 — DMA Memory: Two Allocation Paths and the Zero-Copy Chain](/docs/blog_02_dma_memory.md)*
