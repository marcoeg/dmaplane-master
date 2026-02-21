The Phase 1 driver is a kernel-space producer-consumer pipeline exposed to userspace through four ioctls. Here's how it all fits together.

**The device is a singleton.** When the module loads, `dmaplane_init` creates one `struct dmaplane_dev` — a single global instance (`dma_dev`) that owns everything: the character device, the class, and an array of 4 channel slots. Every `open("/dev/dmaplane")` gets a `struct dmaplane_file_ctx` pointing back at this same device. The file context starts with `chan = NULL` — no channel assigned yet.

**A channel is a paired set of rings plus a worker.** When userspace calls `IOCTL_CREATE_CHANNEL`, the driver finds an empty slot in `dma_dev->channels[]` (under `dev_mutex`), initializes two rings (submission and completion), spawns a kthread, and stores the channel pointer in the file context. From this point, the fd is bound to that channel. One fd, one channel, one worker thread.

**The rings are the central data structures.** Each `struct dmaplane_ring` has a fixed-size array of 1024 `dmaplane_ring_entry` slots (each carrying a `u64 payload` and `u32 flags`), plus a `head` (where the producer writes next) and `tail` (where the consumer reads next). Head and tail are unsigned integers that only increment — they never wrap. To index into the array, you take `head % RING_SIZE`. Full when `head - tail == 1024`. Empty when `head == tail`. This monotonic design eliminates the classic ambiguity where wrapped indices can't distinguish full from empty.

Head and tail sit on separate cache lines (`____cacheline_aligned_in_smp`). This prevents false sharing: the producer hammering head doesn't invalidate the consumer's cache line for tail. Updates use `smp_store_release` so the other side sees the ring entry contents before seeing the advanced index — critical on ARM, a no-op on x86, but architecturally correct.

**The computational flow is a four-step loop:**

1. **Userspace submits** (`IOCTL_SUBMIT`). The ioctl copies a `dmaplane_ring_entry` from userspace, acquires the submission ring spinlock, writes the entry at `sub_ring.head % RING_SIZE`, advances head, releases the lock, and wakes the worker via `wake_up_interruptible`.

2. **The worker drains submissions.** The kthread sleeps on a wait queue until the submission ring is non-empty. When woken, it acquires the submission ring lock, reads the entry at `sub_ring.tail % RING_SIZE`, advances tail, releases the lock. It now holds one entry.

3. **The worker "processes" and posts a completion.** Processing is trivial in Phase 1: increment the payload by 1. This proves the worker touched the data. The worker then acquires the completion ring lock and writes the result at `comp_ring.head % RING_SIZE`. If the completion ring is full (userspace hasn't drained it), the worker enters a yield loop — `cond_resched()`, recheck, repeat — rather than dropping the entry. This is Phase 1's backpressure mechanism: a full completion ring stalls the worker, which stops draining the submission ring, which causes `IOCTL_SUBMIT` to get `-ENOSPC`, which forces userspace to call `IOCTL_COMPLETE` before it can submit more.

4. **Userspace completes** (`IOCTL_COMPLETE`). The ioctl acquires the completion ring lock, reads the entry at `comp_ring.tail % RING_SIZE`, advances tail, releases the lock, copies the result to userspace. If the ring is empty, returns `-EAGAIN` immediately (non-blocking).

**The two rings have opposite producer/consumer roles:**

```
Submission ring:   userspace (producer, writes head)  →  worker (consumer, reads tail)
Completion ring:   worker (producer, writes head)     →  userspace (consumer, reads tail)
```

Each ring has its own spinlock. The submission lock serializes the ioctl submit path against the worker reading the same ring. The completion lock serializes the worker writing against the ioctl complete path. The device mutex (`dev_mutex`) is only for channel slot allocation — it's never held during the submit/complete hot path.

**Lifecycle management is the other critical piece.** When userspace closes the fd (or the process exits), `dmaplane_release` fires. It sets `channel->shutdown = true`, wakes the worker (so it sees the flag), and calls `kthread_stop` (which blocks until the worker returns). The worker's wait condition checks `kthread_should_stop()`, so it exits cleanly even if the submission ring is empty. The completion ring yield loop also checks `kthread_should_stop()` to avoid deadlock — without this, a worker stuck yielding on a full completion ring would never see the shutdown signal if userspace is gone and nobody is draining completions.

**What the ioctls don't do yet is the point.** The worker increments a payload. No DMA. No device interaction. No real work. But the machinery — ring-based command queuing, kernel worker threads, backpressure through ring occupancy, clean lifecycle on process exit — is the exact same pattern used by real device drivers. In Phase 2, the worker starts touching DMA buffers. In Phase 4, it posts RDMA work requests. In Phase 8, it reads GPU VRAM through BAR mappings. The scaffolding doesn't change. The processing stub gets replaced.

