// SPDX-License-Identifier: GPL-2.0
/*
 * dmaplane — Character device driver with submission/completion rings
 * Copyright (c) 2026 Graziano Labs Corp.
 *
 * Phase 1: Driver foundations and concurrency.
 * Character device at /dev/dmaplane, ioctl-driven, per-channel worker
 * threads with spinlock-protected ring buffers.
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

/* Singleton device context */
static struct dmaplane_dev *dma_dev;

/* ------------------------------------------------------------------ */
/* Ring buffer operations                                              */
/* ------------------------------------------------------------------ */

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
 */
static int dmaplane_worker_fn(void *data)
{
	struct dmaplane_channel *chan = data;
	struct dmaplane_ring *sub = &chan->sub_ring;
	struct dmaplane_ring *comp = &chan->comp_ring;
	unsigned int batch;

	pr_debug("worker %u started\n", chan->id);

	while (!kthread_should_stop()) {
		/* Sleep until there is work or we are told to stop */
		wait_event_interruptible(chan->wait_queue,
			!dmaplane_ring_empty(sub) || kthread_should_stop());

		if (kthread_should_stop() && dmaplane_ring_empty(sub))
			break;

		/* Drain submissions */
		batch = 0;
		while (!dmaplane_ring_empty(sub)) {
			struct dmaplane_ring_entry entry;
			unsigned long flags;

			/* Read one submission */
			spin_lock_irqsave(&sub->lock, flags);
			if (dmaplane_ring_empty(sub)) {
				spin_unlock_irqrestore(&sub->lock, flags);
				break;
			}
			entry = sub->entries[sub->tail % DMAPLANE_RING_SIZE];
			smp_store_release(&sub->tail, sub->tail + 1);
			spin_unlock_irqrestore(&sub->lock, flags);

			/* Process: increment payload by 1 */
			entry.payload += 1;

			/* Push to completion ring, yielding if full */
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

			atomic_dec(&chan->in_flight);
			chan->stats.total_completions++;
			batch++;

			/* Yield periodically to avoid hogging CPU */
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

static void dmaplane_channel_init(struct dmaplane_channel *chan, unsigned int id)
{
	dmaplane_ring_init(&chan->sub_ring);
	dmaplane_ring_init(&chan->comp_ring);
	init_waitqueue_head(&chan->wait_queue);
	chan->id = id;
	atomic_set(&chan->in_flight, 0);
	chan->shutdown = false;
	chan->active = false;
	chan->worker = NULL;
	memset(&chan->stats, 0, sizeof(chan->stats));
}

/*
 * Stop a channel's worker thread and mark the channel as inactive.
 * Called from release() or module exit.
 */
static void dmaplane_channel_destroy(struct dmaplane_channel *chan)
{
	if (!chan->active)
		return;

	chan->shutdown = true;
	wake_up_interruptible(&chan->wait_queue);

	if (chan->worker) {
		kthread_stop(chan->worker);
		chan->worker = NULL;
	}

	chan->active = false;
	pr_debug("channel %u destroyed\n", chan->id);
}

/* ------------------------------------------------------------------ */
/* Ioctl handlers                                                      */
/* ------------------------------------------------------------------ */

static long dmaplane_ioctl_create_channel(struct dmaplane_file_ctx *ctx,
					   unsigned long arg)
{
	struct dmaplane_dev *dev = ctx->dev;
	struct dmaplane_channel_params params;
	struct dmaplane_channel *chan;
	struct task_struct *worker;
	int i;

	/* One channel per fd */
	if (ctx->chan)
		return -EBUSY;

	mutex_lock(&dev->dev_mutex);

	/* Find a free channel slot */
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

	/* Create worker kthread */
	worker = kthread_create(dmaplane_worker_fn, chan, "dmaplane/%d", i);
	if (IS_ERR(worker)) {
		chan->active = false;
		mutex_unlock(&dev->dev_mutex);
		return PTR_ERR(worker);
	}
	chan->worker = worker;
	wake_up_process(worker);

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
	smp_store_release(&ring->head, ring->head + 1);

	/* Track high watermark */
	occupancy = dmaplane_ring_count(ring);
	if (occupancy > chan->stats.ring_high_watermark)
		chan->stats.ring_high_watermark = occupancy;

	spin_unlock_irqrestore(&ring->lock, flags);

	atomic_inc(&chan->in_flight);
	chan->stats.total_submissions++;

	/* Wake the worker */
	wake_up_interruptible(&chan->wait_queue);

	return 0;
}

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
	smp_store_release(&ring->tail, ring->tail + 1);

	spin_unlock_irqrestore(&ring->lock, flags);

	if (copy_to_user((void __user *)arg, &params, sizeof(params)))
		return -EFAULT;

	return 0;
}

static long dmaplane_ioctl_get_stats(struct dmaplane_file_ctx *ctx,
				      unsigned long arg)
{
	struct dmaplane_channel *chan = ctx->chan;

	if (!chan)
		return -ENODEV;

	if (copy_to_user((void __user *)arg, &chan->stats, sizeof(chan->stats)))
		return -EFAULT;

	return 0;
}

/* ------------------------------------------------------------------ */
/* File operations                                                     */
/* ------------------------------------------------------------------ */

static int dmaplane_open(struct inode *inode, struct file *filp)
{
	struct dmaplane_file_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dev = dma_dev;
	ctx->chan = NULL;
	filp->private_data = ctx;

	pr_debug("device opened\n");
	return 0;
}

static int dmaplane_release(struct inode *inode, struct file *filp)
{
	struct dmaplane_file_ctx *ctx = filp->private_data;

	if (ctx->chan) {
		mutex_lock(&ctx->dev->dev_mutex);
		dmaplane_channel_destroy(ctx->chan);
		mutex_unlock(&ctx->dev->dev_mutex);
	}

	kfree(ctx);
	pr_debug("device closed\n");
	return 0;
}

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

static int __init dmaplane_init(void)
{
	int ret;

	dma_dev = kzalloc(sizeof(*dma_dev), GFP_KERNEL);
	if (!dma_dev)
		return -ENOMEM;

	mutex_init(&dma_dev->dev_mutex);

	/* Dynamic major number */
	ret = alloc_chrdev_region(&dma_dev->devno, 0, 1, DMAPLANE_NAME);
	if (ret < 0) {
		pr_err("alloc_chrdev_region failed: %d\n", ret);
		goto err_free_dev;
	}

	/* Character device */
	cdev_init(&dma_dev->cdev, &dmaplane_fops);
	dma_dev->cdev.owner = THIS_MODULE;
	ret = cdev_add(&dma_dev->cdev, dma_dev->devno, 1);
	if (ret < 0) {
		pr_err("cdev_add failed: %d\n", ret);
		goto err_unreg_region;
	}

	/* Device class for udev */
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

static void __exit dmaplane_exit(void)
{
	int i;

	/* Stop any remaining active channels */
	mutex_lock(&dma_dev->dev_mutex);
	for (i = 0; i < DMAPLANE_MAX_CHANNELS; i++)
		dmaplane_channel_destroy(&dma_dev->channels[i]);
	mutex_unlock(&dma_dev->dev_mutex);

	device_destroy(dma_dev->class, dma_dev->devno);
	class_destroy(dma_dev->class);
	cdev_del(&dma_dev->cdev);
	unregister_chrdev_region(dma_dev->devno, 1);
	kfree(dma_dev);

	pr_info("module unloaded\n");
}

module_init(dmaplane_init);
module_exit(dmaplane_exit);
