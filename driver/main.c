// SPDX-License-Identifier: GPL-2.0
/*
 * dmaplane main.c — Character device driver and ioctl dispatch
 * Copyright (c) 2026 Graziano Labs Corp.
 *
 * Phase 1: Driver foundations and concurrency.
 *
 * This is the entry point for the dmaplane kernel module.  It registers
 * a character device at /dev/dmaplane, dispatches ioctls to per-command
 * handlers, and manages per-file context (one channel per open fd).
 *
 * Architecture:
 *   userspace ioctl → dmaplane_ioctl (this file) → handler functions
 *   Each open fd gets a dmaplane_file_ctx.  Channel creation spawns a
 *   kthread that drains the submission ring and fills the completion ring.
 *   File close tears down the channel and stops the kthread.
 *
 * Data flow:
 *   userspace --[IOCTL_SUBMIT]--> sub_ring --[worker]--> comp_ring
 *             <--[IOCTL_COMPLETE]--
 *
 * Locking summary:
 *   dev_mutex (struct mutex)
 *     Protects: channel slot allocation and deallocation in
 *               dma_dev->channels[].
 *     Acquired by: dmaplane_ioctl_create_channel, dmaplane_release,
 *                  dmaplane_exit.
 *     Why mutex: only taken in process/ioctl context where sleeping is
 *                permitted; never on the submit/complete hot path.
 *     Ordering: outermost lock.
 *
 *   channel->lock (struct mutex)
 *     Protects: channel state (shutdown flag, worker pointer).
 *     Acquired by: dmaplane_release (shutdown sequence).
 *     Why mutex: sleeping context OK — only from process context.
 *     Ordering: dev_mutex → channel->lock → ring spinlocks.
 *
 *   sub_ring.lock / comp_ring.lock (spinlock_t, irqsave)
 *     Protects: ring entries[] array and head/tail index updates for
 *               the respective ring.
 *     Acquired by: ioctl submit/complete paths (producer or consumer)
 *                  and the worker thread (the other side).
 *     Why spinlock: ring operations are very short and may be called
 *                   from contexts where sleeping is not desired.
 *     Ordering: never nested; each ring lock is independent.
 *
 * Memory ordering:
 *   smp_store_release() is used on every head/tail advancement to
 *   ensure that the entry write (or read) is visible to the other side
 *   before the index update becomes visible.  The matching acquire
 *   semantics are provided by spin_lock_irqsave() on the consumer/
 *   producer side.  This is a no-op on x86 (TSO) but required for
 *   correctness on ARM and RISC-V.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/delay.h>

#include "dmaplane.h"

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Graziano Labs Corp.");
MODULE_DESCRIPTION("dmaplane — host-side data path for AI-scale systems");
MODULE_VERSION("0.1.0");

/* Singleton device context — allocated in dmaplane_init, freed in dmaplane_exit */
static struct dmaplane_dev *dma_dev;

/* ------------------------------------------------------------------ */
/* Ring buffer operations                                              */
/* ------------------------------------------------------------------ */

/*
 * dmaplane_ring_init - Zero-initialise a ring buffer and its lock.
 * @ring: ring to initialise.
 *
 * Called once per ring during channel creation.  No locking required
 * because the channel is not yet visible to other threads.
 */
static void dmaplane_ring_init(struct dmaplane_ring *ring)
{
	memset(ring->entries, 0, sizeof(ring->entries));
	spin_lock_init(&ring->lock);
	ring->head = 0;
	ring->tail = 0;
}

/* ------------------------------------------------------------------ */
/* Worker thread                                                       */
/* ------------------------------------------------------------------ */

/*
 * dmaplane_worker_fn — Per-channel worker thread.
 *
 * Drains submissions from the submission ring, "processes" each entry
 * (Phase 1: increments payload by 1 to prove the worker touched it),
 * and pushes results onto the completion ring.
 *
 * Locking:
 *   Acquires sub_ring.lock (consumer side) to read submissions and
 *   comp_ring.lock (producer side) to write completions.  No lock is
 *   held during entry processing or sleep, minimising contention.
 *
 * Backpressure:
 *   If the completion ring is full, the worker yields via cond_resched()
 *   in a spin-wait loop rather than dropping entries.  This applies
 *   backpressure upstream: the submission ring fills, and userspace
 *   gets -ENOSPC on the next submit, forcing it to drain completions.
 *
 * Shutdown:
 *   The outer loop checks kthread_should_stop().  The wait_event also
 *   checks kthread_should_stop() so the worker wakes up on stop.
 *   The completion ring yield loop additionally checks kthread_should_stop()
 *   to ensure the worker can exit even if the ring stays full.
 */
static int dmaplane_worker_fn(void *data)
{
	struct dmaplane_channel *chan = data;
	struct dmaplane_ring *sub = &chan->sub_ring;
	struct dmaplane_ring *comp = &chan->comp_ring;
	unsigned int batch;

	pr_debug("worker %u started\n", chan->id);

	while (!kthread_should_stop()) {
		/*
		 * Sleep until the submission ring is non-empty or the
		 * kthread stop flag is set.  wait_event_interruptible
		 * puts the worker to sleep on chan->wait_queue and
		 * re-checks the condition atomically on each wakeup,
		 * preventing the lost-wakeup race where a wake_up
		 * arrives between the condition check and the sleep.
		 */
		wait_event_interruptible(chan->wait_queue,
			!dmaplane_ring_empty(sub) || kthread_should_stop());

		/* If stopped and ring is drained, exit cleanly */
		if (kthread_should_stop() && dmaplane_ring_empty(sub))
			break;

		/* Drain submissions */
		batch = 0;
		while (!dmaplane_ring_empty(sub)) {
			struct dmaplane_ring_entry entry;
			unsigned long flags;

			/* Read one submission under the lock */
			spin_lock_irqsave(&sub->lock, flags);
			/* Re-check under lock — the unlocked check above
			 * is an optimisation; this is the authoritative one */
			if (dmaplane_ring_empty(sub)) {
				spin_unlock_irqrestore(&sub->lock, flags);
				break;
			}
			entry = sub->entries[sub->tail % DMAPLANE_RING_SIZE];
			/* smp_store_release: ensure the entry read above
			 * is complete before the tail advance becomes
			 * visible to the submit-side producer. */
			smp_store_release(&sub->tail, sub->tail + 1);
			spin_unlock_irqrestore(&sub->lock, flags);

			/*
			 * Phase 1 processing: increment payload by 1 to
			 * prove the worker touched the entry.  Future phases
			 * replace this with DMA ops, RDMA posts, or GPU
			 * transfers.
			 */
			entry.payload += 1;

			/*
			 * Push to completion ring with backpressure.
			 *
			 * If the completion ring is full (userspace is not
			 * draining completions fast enough), we spin-yield
			 * rather than dropping the entry:
			 *  1. Acquire comp_ring lock.
			 *  2. If room, write entry, advance head with
			 *     smp_store_release, and break.
			 *  3. If full, release lock, cond_resched() to
			 *     yield CPU, then retry.
			 *  4. If kthread_should_stop() during wait, bail
			 *     to done — entry is lost, acceptable during
			 *     channel teardown.
			 */
			for (;;) {
				spin_lock_irqsave(&comp->lock, flags);
				if (!dmaplane_ring_full(comp)) {
					comp->entries[comp->head % DMAPLANE_RING_SIZE] = entry;
					/* smp_store_release: entry store must
					 * be visible before head advance
					 * reaches the complete-ioctl consumer */
					smp_store_release(&comp->head, comp->head + 1);
					spin_unlock_irqrestore(&comp->lock, flags);
					break;
				}
				spin_unlock_irqrestore(&comp->lock, flags);

				if (kthread_should_stop())
					goto done;
				cond_resched();
			}

			atomic_dec(&chan->in_flight);
			atomic64_inc(&chan->stats.total_completions);
			batch++;

			/* Yield every 64 entries to avoid hogging CPU */
			if (batch % 64 == 0)
				cond_resched();
		}
	}

done:
	pr_debug("worker %u stopped\n", chan->id);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Channel management                                                  */
/* ------------------------------------------------------------------ */

/*
 * dmaplane_channel_init - Reset all channel state to initial values.
 * @chan: channel to initialise.
 * @id:  channel index in dma_dev->channels[].
 *
 * Called under dev_mutex during IOCTL_CREATE_CHANNEL.  Initialises both
 * rings, the wait queue, kref (starts at 1 for the file_ctx holder),
 * per-channel mutex, and atomic stats.  The channel is not yet marked
 * active, so no other thread can observe it.
 */
static void dmaplane_channel_init(struct dmaplane_channel *chan, unsigned int id)
{
	kref_init(&chan->refcount);	/* Initial ref for file_ctx */
	mutex_init(&chan->lock);
	dmaplane_ring_init(&chan->sub_ring);
	dmaplane_ring_init(&chan->comp_ring);
	init_waitqueue_head(&chan->wait_queue);
	chan->id = id;
	atomic_set(&chan->in_flight, 0);
	chan->shutdown = false;
	chan->active = false;
	chan->worker = NULL;
	atomic64_set(&chan->stats.total_submissions, 0);
	atomic64_set(&chan->stats.total_completions, 0);
	atomic_set(&chan->stats.ring_high_watermark, 0);
	atomic_set(&chan->stats.dropped_count, 0);
}

/*
 * dmaplane_channel_release - kref release callback.
 * @ref: pointer to the kref embedded in dmaplane_channel.
 *
 * Called when the last reference is dropped (kref hits zero).
 * Marks the channel slot inactive so it can be reused by
 * create_channel.  Also decrements the device-level active_channels
 * counter and increments total_channels_destroyed.
 *
 * At this point the worker has exited and the file_ctx has released
 * its reference — no other code path can access this channel.
 */
void dmaplane_channel_release(struct kref *ref)
{
	struct dmaplane_channel *chan =
		container_of(ref, struct dmaplane_channel, refcount);

	chan->active = false;
	atomic_dec(&dma_dev->active_channels);
	atomic64_inc(&dma_dev->total_channels_destroyed);
	pr_debug("channel %u released (kref=0)\n", chan->id);
}

/*
 * dmaplane_channel_destroy - Stop a channel's worker and drop file_ctx ref.
 * @chan: channel to destroy.
 *
 * Must be called under dev_mutex (from release or module exit).
 *
 * Teardown sequence:
 *  1. Acquire channel->lock.
 *  2. Set chan->shutdown = true to signal the worker.
 *  3. Release channel->lock (so the worker can check shutdown under
 *     lock if needed in future phases).
 *  4. Wake the worker so it can observe the flag and kthread_should_stop().
 *  5. kthread_stop() waits for the worker to exit.  Note: the kthread
 *     wrapper may skip threadfn entirely if KTHREAD_SHOULD_STOP is set
 *     before the kthread gets a timeslice (common when channels are
 *     created and destroyed quickly).
 *  6. kref_put drops the worker's ref (refcount 2 → 1).
 *  7. kref_put drops the file_ctx's ref (refcount 1 → 0), triggering
 *     dmaplane_channel_release which marks the slot inactive.
 *
 * After return, the worker is guaranteed stopped and ring memory is safe
 * to reuse (but not zeroed — stats remain readable until re-init).
 */
static void dmaplane_channel_destroy(struct dmaplane_channel *chan)
{
	if (!chan->active)
		return;

	mutex_lock(&chan->lock);
	chan->shutdown = true;
	mutex_unlock(&chan->lock);

	wake_up_interruptible(&chan->wait_queue);

	if (chan->worker) {
		kthread_stop(chan->worker);
		chan->worker = NULL;
	}

	/*
	 * Drop both refs.  kthread_stop guarantees the worker has exited,
	 * but threadfn may not have run — the kthread wrapper skips
	 * threadfn if KTHREAD_SHOULD_STOP is already set (happens when
	 * a channel is created and destroyed before the scheduler gives
	 * the kthread a timeslice).  We drop the worker's ref here
	 * unconditionally, then the file_ctx's ref triggers release.
	 */
	kref_put(&chan->refcount, dmaplane_channel_release);  /* worker ref */
	kref_put(&chan->refcount, dmaplane_channel_release);  /* file_ctx ref */
	pr_debug("channel %u destroyed\n", chan->id);
}

/* ------------------------------------------------------------------ */
/* Ioctl handlers                                                      */
/* ------------------------------------------------------------------ */

/*
 * dmaplane_ioctl_create_channel - Allocate a channel and start its worker.
 * @ctx: file context (carries the owning dev pointer and channel slot).
 * @arg: userspace pointer to struct dmaplane_channel_params (output).
 *
 * Finds the first free slot in dev->channels[], initialises it, creates
 * a kthread with kthread_create() (which does NOT start it), stores the
 * task pointer, then wake_up_process() makes it runnable.  The two-step
 * create/wake pattern ensures chan->worker is set before the thread can
 * run, preventing a race where the worker dereferences itself before
 * the pointer is stored.
 *
 * Concurrency: dev_mutex serialises slot search and active-flag set so
 * two concurrent CREATE_CHANNEL calls cannot claim the same slot.
 *
 * Returns:
 *   0       on success (channel_id written to userspace via @arg).
 *  -EBUSY   if this fd already owns a channel (one channel per fd).
 *  -ENOSPC  if all DMAPLANE_MAX_CHANNELS slots are in use.
 *  -EFAULT  if copy_to_user fails.
 *  -errno   if kthread_create fails (propagated via PTR_ERR).
 */
static long dmaplane_ioctl_create_channel(struct dmaplane_file_ctx *ctx,
					   unsigned long arg)
{
	struct dmaplane_dev *dev = ctx->dev;
	struct dmaplane_channel_params params;
	struct dmaplane_channel *chan;
	struct task_struct *worker;
	int i;

	/* One channel per fd — simplifies lifecycle; future phases may relax */
	if (ctx->chan)
		return -EBUSY;

	mutex_lock(&dev->dev_mutex);

	/*
	 * Linear scan of DMAPLANE_MAX_CHANNELS slots.  Deliberate
	 * simplicity — IDR/bitmap would be overkill for 4 slots.
	 */
	for (i = 0; i < DMAPLANE_MAX_CHANNELS; i++) {
		if (!dev->channels[i].active)
			break;
	}
	if (i == DMAPLANE_MAX_CHANNELS) {
		mutex_unlock(&dev->dev_mutex);
		return -ENOSPC;
	}

	chan = &dev->channels[i];
	dmaplane_channel_init(chan, i);
	chan->active = true;

	/* Take a second ref for the worker thread (init gave us 1 for file_ctx) */
	kref_get(&chan->refcount);

	/*
	 * kthread_create allocates the task but does NOT start it.
	 * We store chan->worker first, then wake_up_process makes it
	 * runnable, ensuring the pointer is visible before the thread runs.
	 */
	worker = kthread_create(dmaplane_worker_fn, chan, "dmaplane/%d", i);
	if (IS_ERR(worker)) {
		/*
		 * No other code path holds a reference — the channel was
		 * just initialised and never stored in file_ctx.  Reset
		 * directly; kref_init on next reuse will reinitialise.
		 */
		chan->active = false;
		mutex_unlock(&dev->dev_mutex);
		return PTR_ERR(worker);
	}
	chan->worker = worker;
	wake_up_process(worker);

	atomic_inc(&dev->active_channels);
	atomic64_inc(&dev->total_channels_created);

	ctx->chan = chan;

	mutex_unlock(&dev->dev_mutex);

	/* Return channel ID to userspace */
	params.channel_id = i;
	params._pad = 0;
	if (copy_to_user((void __user *)arg, &params, sizeof(params)))
		return -EFAULT;

	pr_debug("channel %d created\n", i);
	return 0;
}

/*
 * dmaplane_ioctl_submit - Enqueue one entry into the submission ring.
 * @ctx: file context (must have an assigned channel).
 * @arg: userspace pointer to struct dmaplane_submit_params (input).
 *
 * Copies the entry from userspace, acquires sub_ring.lock, writes the
 * entry at head % RING_SIZE, advances head with smp_store_release, and
 * wakes the worker.  When the ring is full, returns -ENOSPC — userspace
 * must drain completions to make progress (backpressure mechanism).
 *
 * High watermark tracking: after the entry is written but before the
 * lock is released, we snapshot the occupancy and update the atomic
 * stat if the new value is higher.  Safe because the lock serialises
 * writers; the atomic_set is relaxed (no ordering needed for stats).
 *
 * Returns:
 *   0       on success.
 *  -ENODEV  if no channel has been created on this fd.
 *  -EFAULT  if copy_from_user fails.
 *  -ENOSPC  if the submission ring is full.
 */
static long dmaplane_ioctl_submit(struct dmaplane_file_ctx *ctx,
				   unsigned long arg)
{
	struct dmaplane_channel *chan = ctx->chan;
	struct dmaplane_submit_params params;
	struct dmaplane_ring *ring;
	unsigned long flags;
	unsigned int occupancy;

	if (!chan)
		return -ENODEV;

	if (copy_from_user(&params, (void __user *)arg, sizeof(params)))
		return -EFAULT;

	ring = &chan->sub_ring;

	spin_lock_irqsave(&ring->lock, flags);

	if (dmaplane_ring_full(ring)) {
		spin_unlock_irqrestore(&ring->lock, flags);
		return -ENOSPC;
	}

	ring->entries[ring->head % DMAPLANE_RING_SIZE] = params.entry;
	/* smp_store_release: entry store must be visible before head advance */
	smp_store_release(&ring->head, ring->head + 1);

	/* Track high watermark under lock — compare-and-swap if new max */
	occupancy = dmaplane_ring_count(ring);
	{
		unsigned int old_wm = atomic_read(&chan->stats.ring_high_watermark);
		if (occupancy > old_wm)
			atomic_set(&chan->stats.ring_high_watermark, occupancy);
	}

	spin_unlock_irqrestore(&ring->lock, flags);

	atomic_inc(&chan->in_flight);
	atomic64_inc(&chan->stats.total_submissions);

	/* Wake the worker so it sees the new entry */
	wake_up_interruptible(&chan->wait_queue);

	return 0;
}

/*
 * dmaplane_ioctl_complete - Dequeue one entry from the completion ring.
 * @ctx: file context (must have an assigned channel).
 * @arg: userspace pointer to struct dmaplane_complete_params (output).
 *
 * Non-blocking: returns -EAGAIN immediately if the completion ring is
 * empty.  Userspace should poll/retry or interleave with submits.
 *
 * Returns:
 *   0       on success (entry copied to userspace).
 *  -ENODEV  if no channel has been created on this fd.
 *  -EAGAIN  if the completion ring is empty (try again later).
 *  -EFAULT  if copy_to_user fails.
 */
static long dmaplane_ioctl_complete(struct dmaplane_file_ctx *ctx,
				     unsigned long arg)
{
	struct dmaplane_channel *chan = ctx->chan;
	struct dmaplane_complete_params params;
	struct dmaplane_ring *ring;
	unsigned long flags;

	if (!chan)
		return -ENODEV;

	ring = &chan->comp_ring;

	spin_lock_irqsave(&ring->lock, flags);

	if (dmaplane_ring_empty(ring)) {
		spin_unlock_irqrestore(&ring->lock, flags);
		return -EAGAIN;
	}

	params.entry = ring->entries[ring->tail % DMAPLANE_RING_SIZE];
	/* smp_store_release: entry read must be complete before tail advance */
	smp_store_release(&ring->tail, ring->tail + 1);

	spin_unlock_irqrestore(&ring->lock, flags);

	if (copy_to_user((void __user *)arg, &params, sizeof(params)))
		return -EFAULT;

	return 0;
}

/*
 * dmaplane_ioctl_get_stats - Copy per-channel statistics to userspace.
 * @ctx: file context (must have an assigned channel).
 * @arg: userspace pointer to struct dmaplane_stats (output).
 *
 * Reads atomic counters from dmaplane_stats_kern into a plain
 * dmaplane_stats UAPI struct, then copies to userspace.  Each atomic
 * read is individually consistent, but the set may be momentarily
 * inconsistent (e.g., total_completions could briefly exceed
 * total_submissions if read between the two increments).
 * Acceptable for observability.
 *
 * Returns:
 *   0       on success.
 *  -ENODEV  if no channel has been created on this fd.
 *  -EFAULT  if copy_to_user fails.
 */
static long dmaplane_ioctl_get_stats(struct dmaplane_file_ctx *ctx,
				      unsigned long arg)
{
	struct dmaplane_channel *chan = ctx->chan;
	struct dmaplane_stats ustats;

	if (!chan)
		return -ENODEV;

	ustats.total_submissions  = atomic64_read(&chan->stats.total_submissions);
	ustats.total_completions  = atomic64_read(&chan->stats.total_completions);
	ustats.ring_high_watermark = atomic_read(&chan->stats.ring_high_watermark);
	ustats.dropped_count      = atomic_read(&chan->stats.dropped_count);

	if (copy_to_user((void __user *)arg, &ustats, sizeof(ustats)))
		return -EFAULT;

	return 0;
}

/* ------------------------------------------------------------------ */
/* File operations                                                     */
/* ------------------------------------------------------------------ */

/*
 * dmaplane_open - Allocate per-fd context for a new open().
 * @inode: unused (single device).
 * @filp:  file pointer; private_data will be set.
 *
 * Allocates a dmaplane_file_ctx with kzalloc (GFP_KERNEL — process
 * context, may sleep).  The ctx starts with chan == NULL; the user
 * must issue IOCTL_CREATE_CHANNEL before submit/complete are usable.
 *
 * Returns 0 on success, -ENOMEM on allocation failure.
 */
static int dmaplane_open(struct inode *inode, struct file *filp)
{
	struct dmaplane_file_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dev = dma_dev;
	ctx->chan = NULL;
	filp->private_data = ctx;

	atomic64_inc(&dma_dev->total_opens);
	pr_debug("device opened\n");
	return 0;
}

/*
 * dmaplane_release - Clean up per-fd state when the file is closed.
 * @inode: unused.
 * @filp:  file pointer carrying the dmaplane_file_ctx.
 *
 * If a channel was created on this fd, acquires dev_mutex and calls
 * dmaplane_channel_destroy to stop the worker and free the slot.
 * Then frees the file context.  This ensures that closing the fd
 * (including process exit without explicit close) releases all kernel
 * resources — no explicit "destroy channel" ioctl is needed.
 *
 * Returns 0 always.
 */
static int dmaplane_release(struct inode *inode, struct file *filp)
{
	struct dmaplane_file_ctx *ctx = filp->private_data;

	if (ctx->chan) {
		mutex_lock(&ctx->dev->dev_mutex);
		dmaplane_channel_destroy(ctx->chan);
		mutex_unlock(&ctx->dev->dev_mutex);
	}

	kfree(ctx);
	atomic64_inc(&dma_dev->total_closes);
	pr_debug("device closed\n");
	return 0;
}

/*
 * dmaplane_ioctl - Top-level ioctl dispatcher.
 * @filp: file pointer carrying the dmaplane_file_ctx.
 * @cmd:  ioctl command number (DMAPLANE_IOCTL_*).
 * @arg:  userspace pointer argument.
 *
 * Routes to the specific handler based on cmd.  Returns -ENOTTY for
 * unrecognised commands (standard convention for bad ioctl numbers).
 */
static long dmaplane_ioctl(struct file *filp, unsigned int cmd,
			    unsigned long arg)
{
	struct dmaplane_file_ctx *ctx = filp->private_data;

	switch (cmd) {
	case DMAPLANE_IOCTL_CREATE_CHANNEL:
		return dmaplane_ioctl_create_channel(ctx, arg);
	case DMAPLANE_IOCTL_SUBMIT:
		return dmaplane_ioctl_submit(ctx, arg);
	case DMAPLANE_IOCTL_COMPLETE:
		return dmaplane_ioctl_complete(ctx, arg);
	case DMAPLANE_IOCTL_GET_STATS:
		return dmaplane_ioctl_get_stats(ctx, arg);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations dmaplane_fops = {
	.owner		= THIS_MODULE,
	.open		= dmaplane_open,
	.release	= dmaplane_release,
	.unlocked_ioctl	= dmaplane_ioctl,
};

/* ------------------------------------------------------------------ */
/* Module init/exit                                                    */
/* ------------------------------------------------------------------ */

/*
 * dmaplane_init - Module entry point.
 *
 * Standard character device registration sequence:
 *  1. kzalloc the singleton dmaplane_dev.
 *  2. alloc_chrdev_region for a dynamic major number (kernel assigns
 *     an unused number; avoids the need to reserve a fixed major).
 *  3. cdev_init + cdev_add to register the file_operations.
 *  4. class_create so udev can see the device class.
 *  5. device_create to create the /dev/dmaplane node.
 *
 * On any failure, goto-based cleanup tears down in reverse order.
 * Channel slots are zero-initialised by kzalloc and are set up on
 * demand in create_channel — no explicit init here.
 */
static int __init dmaplane_init(void)
{
	int ret;

	dma_dev = kzalloc(sizeof(*dma_dev), GFP_KERNEL);
	if (!dma_dev)
		return -ENOMEM;

	mutex_init(&dma_dev->dev_mutex);

	/* Device-level counters — kzalloc zeroes them, but be explicit */
	atomic_set(&dma_dev->active_channels, 0);
	atomic64_set(&dma_dev->total_opens, 0);
	atomic64_set(&dma_dev->total_closes, 0);
	atomic64_set(&dma_dev->total_channels_created, 0);
	atomic64_set(&dma_dev->total_channels_destroyed, 0);

	/* Dynamic major number — kernel assigns unused number */
	ret = alloc_chrdev_region(&dma_dev->devno, 0, 1, DMAPLANE_NAME);
	if (ret < 0) {
		pr_err("alloc_chrdev_region failed: %d\n", ret);
		goto err_free_dev;
	}

	/* Character device — register our file_operations */
	cdev_init(&dma_dev->cdev, &dmaplane_fops);
	dma_dev->cdev.owner = THIS_MODULE;
	ret = cdev_add(&dma_dev->cdev, dma_dev->devno, 1);
	if (ret < 0) {
		pr_err("cdev_add failed: %d\n", ret);
		goto err_unreg_region;
	}

	/* Device class — lets udev see us */
	dma_dev->class = class_create(DMAPLANE_NAME);
	if (IS_ERR(dma_dev->class)) {
		ret = PTR_ERR(dma_dev->class);
		pr_err("class_create failed: %d\n", ret);
		goto err_cdev_del;
	}

	/* Device node: /dev/dmaplane */
	dma_dev->device = device_create(dma_dev->class, NULL,
					dma_dev->devno, NULL, DMAPLANE_NAME);
	if (IS_ERR(dma_dev->device)) {
		ret = PTR_ERR(dma_dev->device);
		pr_err("device_create failed: %d\n", ret);
		goto err_class_destroy;
	}

	pr_info("module loaded (major %d)\n", MAJOR(dma_dev->devno));
	return 0;

err_class_destroy:
	class_destroy(dma_dev->class);
err_cdev_del:
	cdev_del(&dma_dev->cdev);
err_unreg_region:
	unregister_chrdev_region(dma_dev->devno, 1);
err_free_dev:
	kfree(dma_dev);
	return ret;
}

/*
 * dmaplane_exit - Module cleanup.
 *
 * Acquires dev_mutex and destroys any channels that are still active
 * (e.g., if a user process was killed without closing the fd and the
 * release path has not yet run).  Then tears down device node, class,
 * cdev, region, and finally frees dmaplane_dev — exact reverse order
 * of dmaplane_init.
 */
static void __exit dmaplane_exit(void)
{
	int i;

	/* Stop any remaining active channels */
	mutex_lock(&dma_dev->dev_mutex);
	for (i = 0; i < DMAPLANE_MAX_CHANNELS; i++)
		dmaplane_channel_destroy(&dma_dev->channels[i]);
	mutex_unlock(&dma_dev->dev_mutex);

	/* Lifetime summary — visible in dmesg after rmmod */
	pr_info("lifetime: %lld opens, %lld closes, %lld channels created, %lld destroyed\n",
		atomic64_read(&dma_dev->total_opens),
		atomic64_read(&dma_dev->total_closes),
		atomic64_read(&dma_dev->total_channels_created),
		atomic64_read(&dma_dev->total_channels_destroyed));

	device_destroy(dma_dev->class, dma_dev->devno);
	class_destroy(dma_dev->class);
	cdev_del(&dma_dev->cdev);
	unregister_chrdev_region(dma_dev->devno, 1);
	kfree(dma_dev);

	pr_info("module unloaded\n");
}

module_init(dmaplane_init);
module_exit(dmaplane_exit);
