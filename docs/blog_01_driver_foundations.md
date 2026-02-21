# Building a Kernel Driver from Scratch

*Phase 1 of the dmaplane project — a Linux kernel module for learning the host-side data path between AI frameworks and hardware.*

---

Every system that moves tensors between GPUs over a network needs the same plumbing: a character device, ioctl dispatch, ring buffers for submission and completion, worker threads, and correct locking. Before we can allocate DMA memory, register RDMA memory regions, or pin GPU VRAM through PCIe BAR apertures, we need the scaffolding that all of it hangs on. That's what Phase 1 builds.

The result is a loadable kernel module that exposes `/dev/dmaplane`, a character device controlled via ioctl. Userspace opens the device, creates a channel (which spawns a kernel thread), submits work items through a ring buffer, and reads completions from another ring. The worker's "processing" is trivial — it increments the payload by one. The infrastructure is the point.

## Why a Character Device and Ioctl

There are several ways to build a kernel-userspace interface: sysfs for configuration knobs, netlink for event streams, procfs for stats, or `read`/`write` on a file descriptor for byte-oriented I/O. We use none of these.

Each dmaplane operation has structured input and output — a submission carries a `payload` and `flags`, a completion returns a transformed entry, stats return four counters. These don't map cleanly to byte streams. With `read`/`write`, you'd need framing, serialization, and a protocol. With ioctl, each command is a typed function call: the kernel knows the struct layout from the command number (which encodes the direction and size), copies the right number of bytes, and dispatches to the right handler.

More importantly, the ioctl interface extends. Phase 2 adds `IOCTL_ALLOC_BUFFER`. Phase 4 adds `IOCTL_RDMA_WRITE_IMM`. Phase 8 adds `IOCTL_GPU_PIN`. Each new command is a new case in the dispatch switch — the file operations vtable never changes. This is exactly how InfiniBand verbs work at the kernel level: a single character device, dozens of ioctl commands, each with its own typed parameter struct.

The ioctl command encoding carries safety properties that aren't obvious at first:

```c
#define DMAPLANE_IOCTL_SUBMIT \
    _IOW(DMAPLANE_IOC_MAGIC, 0x02, struct dmaplane_submit_params)
```

The `_IOW` macro encodes the direction (write from user to kernel), the magic number (`0xE4`), the command ordinal (`0x02`), and the struct size into a single 32-bit integer. If userspace passes the wrong struct (different size due to a version mismatch), the kernel returns `-ENOTTY` — wrong ioctl number — rather than silently corrupting memory. This fail-fast property matters more than you'd expect when the Python wrapper comes in Phase 9.

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

The key design choice: head and tail are monotonically increasing unsigned integers. They never wrap. When you need to index into the array, you compute `entries[head % RING_SIZE]`. When you need to check fullness, you compute `head - tail == RING_SIZE`.

Why not wrap head and tail at `RING_SIZE`? Because wrapped indices create an ambiguity: when `head == tail`, is the ring full or empty? Both states look the same. The classic workaround is to sacrifice one slot (full when `(head + 1) % SIZE == tail`), which wastes a slot and adds an extra modulo operation on every full check. With monotonic indices and unsigned arithmetic, the subtraction `head - tail` always gives the exact occupancy, even after the unsigned integers wrap past `UINT_MAX`. Full is `== RING_SIZE`, empty is `== 0`. No ambiguity, no wasted slot.

The `____cacheline_aligned_in_smp` annotation on head and tail places them on separate cache lines. Without this, the producer writing `head` and the consumer writing `tail` would bounce a single cache line between cores on every operation — false sharing. On x86 with a 64-byte cache line, this annotation adds padding that gives each index its own line. The effect is measurable under high-rate polling; Phase 7 will quantify it with `perf stat`.

## Locking: What Each Lock Protects and Why

The concurrency model has three locks, each protecting a distinct resource:

**Submission ring spinlock** (`sub_ring.lock`): Protects the ring entries between the ioctl submit path (producer, advances head) and the worker thread (consumer, advances tail). Both sides acquire the lock, do one ring operation, and release. The hold time is microseconds — a spinlock is appropriate because neither side sleeps while holding it.

**Completion ring spinlock** (`comp_ring.lock`): Same pattern, opposite direction. The worker produces completions, userspace consumes them.

**Device mutex** (`dev_mutex`): Protects the channel slot array during allocation and deallocation. This is a sleeping lock because channel creation involves `kthread_create`, which may sleep. Mutexes are appropriate in process context (ioctl handlers run in the calling process's context); spinlocks would be wrong here because sleeping under a spinlock is a BUG.

Every lock has a comment in the header explaining what it protects:

```c
struct dmaplane_channel {
    /*
     * sub_ring: Submission ring.
     * sub_ring.lock protects ring entries between the ioctl submit
     * path (producer, advances head) and the worker thread (consumer,
     * advances tail).
     */
    struct dmaplane_ring sub_ring;
    /* ... */
};
```

This convention isn't decorative. When you're debugging a deadlock at 2 AM, the comment that says "only acquired from ioctl (process context)" tells you the lock can sleep, which tells you the context requirements for any code that acquires it. Lock comments are documentation for the developer who adds Phase 4 and needs to know whether they can acquire `dev_mutex` from an RDMA completion callback (they can't — callbacks run in softirq context).

## Worker Threads: Lifecycle, Wait Queues, and Shutdown

Each channel gets a kernel thread named `dmaplane/N`. The thread's lifecycle:

1. **Creation**: `kthread_create` allocates the task but doesn't run it. `wake_up_process` starts it.
2. **Running**: The worker sleeps on a wait queue until the submission ring is non-empty.
3. **Shutdown**: `dmaplane_channel_destroy` sets `shutdown = true`, wakes the thread, then calls `kthread_stop` — which sets an internal flag and blocks until the thread returns from its function.

The wait queue pattern is standard but subtle:

```c
wait_event_interruptible(chan->wait_queue,
    !dmaplane_ring_empty(sub) || kthread_should_stop());
```

`wait_event_interruptible` atomically checks the condition and goes to sleep if it's false. "Atomically" here means there's no window where a wake-up can be lost between checking and sleeping — the wait queue mechanism handles this with spinlocks internally. Without this guarantee, you'd have a classic lost-wakeup race: the worker checks the ring (empty), the producer adds an entry and calls `wake_up` (nobody sleeping), the worker goes to sleep (forever).

The worker drains submissions one at a time. For each entry, it acquires the submission ring lock, re-checks emptiness under the lock (the unlocked check in the while condition is an optimization — the authoritative check is under the lock), reads the entry, advances `tail`, and releases the lock. Then it processes the entry and pushes to the completion ring.

## What Surprised Me: Completion Ring Backpressure

The original implementation dropped entries when the completion ring was full:

```c
if (!dmaplane_ring_full(comp)) {
    comp->entries[comp->head % DMAPLANE_RING_SIZE] = entry;
    smp_store_release(&comp->head, comp->head + 1);
} else {
    chan->stats.dropped_count++;
}
```

This seemed reasonable — if userspace isn't reading completions, that's userspace's problem. The multi-channel stress test (4 channels × 250K entries via pthreads) disagreed. The failure was:

```
ch0 mismatch at 39680: expected 0x9b01 got 0x9b47
```

The worker was outpacing the userspace drain. The test submits in batches of 256, then tries to complete. With 4 threads on 4 channels all submitting concurrently, the workers had time to fill the completion rings before the threads switched to draining. Entries 39681 through 39750 — about 70 entries — were dropped silently. The next completion the test read was 70 entries ahead of where it expected.

The fix: the worker yields instead of dropping:

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

This creates natural backpressure: if the completion ring is full, the worker blocks, the submission ring fills up, and userspace gets `-ENOSPC` on submit, forcing it to drain completions. The same pattern — producer yields when the downstream queue is full — is exactly how RDMA credit-based flow control works in Phase 6. We discovered it here, at the ring buffer level, because the failure mode is the same: silent data loss when the producer outpaces the consumer.

## What Process Exit Does to Your Driver

The `release` file operation fires when the last reference to the fd drops — which includes the process exiting without calling `close`. Test 7 verifies this: submit 100 entries, close the fd, check dmesg for warnings.

The release path must handle the case where a channel is active, the worker is running, and the submission ring has unprocessed entries. The sequence:

1. Acquire `dev_mutex` (sleeping context is fine — we're in the process's exit path)
2. Set `shutdown = true`
3. Wake the worker (in case it's sleeping on an empty submission ring)
4. `kthread_stop` — block until the worker returns
5. Mark the channel slot as inactive
6. Free the file context

The `kthread_stop` call is the crucial synchronization point. After it returns, the worker is guaranteed to have exited. Without it, the worker could be mid-operation when we free the channel struct — a use-after-free that might not crash immediately but would corrupt memory.

This lifecycle management gets harder in later phases. When a channel has a DMA buffer mapped and an RDMA memory region registered, the release path must tear down resources in dependency order: deregister MR, unmap buffer, free buffer, stop worker, free channel. Get the order wrong and the NIC DMA-writes into freed memory. Phase 5 will add reference counting to enforce this ordering automatically.

## Connection to What Comes Next

The ring buffer, worker thread, and ioctl dispatch are the foundation. In Phase 2, the worker will allocate DMA-mapped memory instead of incrementing a counter. In Phase 4, it will post RDMA send work requests. In Phase 8, it will read from GPU VRAM through PCIe BAR mappings. But the structure — userspace submits via ring, worker processes, userspace reads completions — stays the same.

The throughput numbers from the stress test provide a baseline: approximately 3.7 million ops/sec single-channel, which is the overhead of two ioctls (submit + complete), two spinlock acquisitions per ioctl, a context switch to the worker, and the worker's spinlock acquisitions. This is the cost of the scaffolding. When real DMA or RDMA operations replace the trivial payload increment, the per-operation overhead will be dwarfed by the actual data movement — but knowing the scaffolding cost tells us the floor.

The completion ring backpressure pattern is the one I'll carry forward most directly. It's the same problem as RDMA CQ overflow (Phase 6), but at a smaller scale. The principle: never drop data silently. If the consumer can't keep up, the producer must slow down. In RDMA, this is credit-based flow control with pre-posted receive work requests. In our ring buffer, it's `cond_resched` in a retry loop. Same idea, different mechanism.

Phase 2 adds DMA memory allocation behind a single ioctl — coherent for small hot control structures, scatter-gather for large streaming buffers. The ring/worker infrastructure we built here becomes the dispatch mechanism for those allocation requests.
