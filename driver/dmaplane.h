/* SPDX-License-Identifier: GPL-2.0 */
/*
 * dmaplane — Kernel-internal header
 * Copyright (c) 2026 Graziano Labs Corp.
 *
 * Internal data structures for the dmaplane kernel module.
 * This file includes the UAPI header for shared types (ioctl numbers,
 * parameter structs, constants) and adds kernel-only types: ring
 * buffers, channels, device context, and file context.
 *
 * Architecture:
 *   main.c includes this header.  Userspace test programs include only
 *   dmaplane_uapi.h (via -I../include).  Kernel-only types are behind
 *   #ifdef __KERNEL__.
 */
#ifndef _DMAPLANE_H
#define _DMAPLANE_H

#include "../include/dmaplane_uapi.h"

#ifdef __KERNEL__

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/cache.h>

/*
 * Ring buffer: fixed-size array with monotonically increasing head and tail.
 *
 * Full  when (head - tail) == DMAPLANE_RING_SIZE
 * Empty when head == tail
 *
 * head and tail only increment; modulo is applied when indexing:
 *   ring->entries[head % DMAPLANE_RING_SIZE]
 *
 * Unsigned arithmetic correctness:
 *   head and tail are unsigned int (32-bit).  They increment without
 *   bound and are never reset.  The difference (head - tail) is
 *   well-defined by C unsigned wraparound: even after 2^32 increments,
 *   (head - tail) still yields the correct occupancy as long as the
 *   ring size (1024) is a power of 2 and the occupancy never exceeds
 *   2^32.  The modulo index (head % RING_SIZE) maps naturally because
 *   RING_SIZE is a power of 2, making % equivalent to a bitmask.
 *
 * This avoids the classic "is the ring full or empty?" ambiguity of
 * wrapped-index designs, where head == tail could mean either state.
 *
 * head and tail are placed on separate cache lines to avoid false
 * sharing between the producer and consumer (see annotations below).
 */
struct dmaplane_ring {
	struct dmaplane_ring_entry entries[DMAPLANE_RING_SIZE];

	/*
	 * lock: Protects ring entries and head/tail indices.
	 * See per-ring comments in the channel struct for which
	 * producer/consumer pairs acquire this lock.
	 * Why spinlock: ring operations are very short (single entry
	 * copy + index increment); sleeping is not needed or desired.
	 */
	spinlock_t lock;

	/*
	 * ____cacheline_aligned_in_smp: kernel macro that pads to a
	 * separate cache line boundary on SMP builds (no-op on UP).
	 * Without this, producer (writing head) and consumer (writing
	 * tail) on different CPUs would bounce a single cache line
	 * between cores on every operation — false sharing.
	 */
	unsigned int head ____cacheline_aligned_in_smp;	/* Producer writes */
	unsigned int tail ____cacheline_aligned_in_smp;	/* Consumer reads  */
};

/*
 * Channel: one submission ring, one completion ring, one worker kthread.
 * Each open fd gets at most one channel (assigned via IOCTL_CREATE_CHANNEL).
 */
struct dmaplane_channel {
	/*
	 * sub_ring: Submission ring.
	 * sub_ring.lock protects ring entries between the ioctl submit
	 * path (producer, advances head) and the worker thread (consumer,
	 * advances tail).
	 * Why spinlock: see dmaplane_ring.lock comment above.
	 * Ordering: never held simultaneously with comp_ring.lock.
	 */
	struct dmaplane_ring sub_ring;

	/*
	 * comp_ring: Completion ring.
	 * comp_ring.lock protects ring entries between the worker thread
	 * (producer, advances head) and the ioctl complete path (consumer,
	 * advances tail).
	 * Why spinlock: see dmaplane_ring.lock comment above.
	 * Ordering: never held simultaneously with sub_ring.lock.
	 */
	struct dmaplane_ring comp_ring;

	struct task_struct *worker;	/* kthread: "dmaplane/<id>" */
	wait_queue_head_t wait_queue;	/* Worker sleeps here; woken by
					 * ioctl submit and channel destroy */

	unsigned int id;		/* Channel index [0, MAX_CHANNELS) */

	/*
	 * in_flight: number of entries that have been submitted but not
	 * yet completed.  Atomic because it is incremented by the submit
	 * ioctl path (process context) and decremented by the worker
	 * kthread without holding a shared lock.
	 */
	atomic_t in_flight;

	/*
	 * shutdown: set to true by dmaplane_channel_destroy under
	 * dev_mutex before calling kthread_stop.  The worker checks
	 * kthread_should_stop() as its primary stop signal; this bool
	 * is a supplementary flag used to wake the worker out of
	 * wait_event_interruptible so it can see the stop flag.
	 * Single-writer (destroy path), so no lock needed on this field.
	 */
	bool shutdown;

	/*
	 * active: true when this channel slot is in use.  Protected by
	 * dev_mutex — set to true in create_channel and to false in
	 * channel_destroy, both of which hold the mutex.  Used by the
	 * linear scan in create_channel to find a free slot.
	 */
	bool active;

	/*
	 * stats: per-channel counters.  total_submissions is updated in
	 * the submit ioctl path; total_completions and dropped_count are
	 * updated in the worker thread; ring_high_watermark is updated
	 * under sub_ring.lock.  None of these fields are atomic —
	 * GET_STATS reads a racy snapshot, which is acceptable for
	 * observability purposes.
	 */
	struct dmaplane_stats stats;
};

/*
 * Device context: one per module instance (singleton).
 * Owns all channels and the character device registration.
 */
struct dmaplane_dev {
	struct dmaplane_channel channels[DMAPLANE_MAX_CHANNELS];

	/*
	 * dev_mutex: Protects channel slot allocation and deallocation.
	 * Acquired by: dmaplane_ioctl_create_channel, dmaplane_release,
	 *              dmaplane_exit.
	 * Why mutex: sleeping context OK — only acquired from process
	 *            context (ioctl and file release).  Never acquired
	 *            from interrupt or softirq context.
	 * Ordering: outermost lock; never held while acquiring ring locks
	 *           (current code never needs both simultaneously).
	 */
	struct mutex dev_mutex;

	struct cdev cdev;		/* Character device structure */
	struct class *class;		/* Device class for udev */
	dev_t devno;			/* Major/minor number pair */
	struct device *device;		/* /dev/dmaplane device node */
};

/*
 * File context: per-open-fd state.
 * Each open() allocates one; release() frees it.
 */
struct dmaplane_file_ctx {
	struct dmaplane_dev *dev;	/* Back-pointer to device singleton */
	struct dmaplane_channel *chan;	/* Assigned channel, or NULL if
					 * IOCTL_CREATE_CHANNEL has not been
					 * called on this fd yet */
};

/*
 * Ring buffer helpers.
 *
 * These rely on unsigned arithmetic (see correctness argument in the
 * dmaplane_ring struct comment above).  All three must be called either
 * under ring->lock or in a context where the caller can tolerate a
 * stale value (e.g., the worker's outer empty-check before acquiring
 * the lock — the authoritative check is repeated under the lock).
 */
static inline bool dmaplane_ring_full(const struct dmaplane_ring *ring)
{
	return (ring->head - ring->tail) == DMAPLANE_RING_SIZE;
}

static inline bool dmaplane_ring_empty(const struct dmaplane_ring *ring)
{
	return ring->head == ring->tail;
}

static inline unsigned int dmaplane_ring_count(const struct dmaplane_ring *ring)
{
	return ring->head - ring->tail;
}

#endif /* __KERNEL__ */
#endif /* _DMAPLANE_H */
