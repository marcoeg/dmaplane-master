/* SPDX-License-Identifier: GPL-2.0 */
/*
 * dmaplane — Kernel-internal header
 * Copyright (c) 2026 Graziano Labs Corp.
 *
 * Internal data structures for the dmaplane kernel module.
 * This file includes the UAPI header and adds kernel-only types.
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
 * head and tail are placed on separate cache lines to avoid false
 * sharing between the producer and consumer.
 */
struct dmaplane_ring {
	struct dmaplane_ring_entry entries[DMAPLANE_RING_SIZE];

	/*
	 * lock: Protects ring entries and head/tail indices.
	 * See per-ring comments in the channel struct for which
	 * producer/consumer pairs acquire this lock.
	 */
	spinlock_t lock;

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
	 */
	struct dmaplane_ring sub_ring;

	/*
	 * comp_ring: Completion ring.
	 * comp_ring.lock protects ring entries between the worker thread
	 * (producer, advances head) and the ioctl complete path (consumer,
	 * advances tail).
	 */
	struct dmaplane_ring comp_ring;

	struct task_struct *worker;	/* kthread: "dmaplane/<id>" */
	wait_queue_head_t wait_queue;	/* Worker sleeps here */

	unsigned int id;		/* Channel index [0, MAX_CHANNELS) */
	atomic_t in_flight;		/* Submissions not yet completed */
	bool shutdown;			/* Signals worker to exit */
	bool active;			/* Slot is in use */

	struct dmaplane_stats stats;	/* Per-channel statistics */
};

/*
 * Device context: one per module instance (singleton).
 * Owns all channels and the character device registration.
 */
struct dmaplane_dev {
	struct dmaplane_channel channels[DMAPLANE_MAX_CHANNELS];

	/*
	 * dev_mutex: Protects channel slot allocation/deallocation.
	 * Sleeping context OK — only acquired from ioctl (process context).
	 */
	struct mutex dev_mutex;

	struct cdev cdev;
	struct class *class;
	dev_t devno;
	struct device *device;
};

/*
 * File context: per-open-fd state.
 * Each open() allocates one; release() frees it.
 */
struct dmaplane_file_ctx {
	struct dmaplane_dev *dev;	/* Back-pointer to device */
	struct dmaplane_channel *chan;	/* Assigned channel (NULL until CREATE_CHANNEL) */
};

/* Ring buffer helpers */
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
