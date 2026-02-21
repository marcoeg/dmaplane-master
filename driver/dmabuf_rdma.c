// SPDX-License-Identifier: GPL-2.0
/*
 * dmabuf_rdma.c — Buffer management for DMA-mapped memory
 * Copyright (c) 2026 Graziano Labs Corp.
 *
 * This file allocates physical pages that will later be registered
 * with the RDMA NIC as Memory Regions (MRs) in Phase 4.  Kernel page
 * ownership is the foundation of the zero-copy architecture — the module
 * allocates, owns, and controls the lifecycle of all physical pages.
 *
 * Two allocation paths:
 *   BUF_TYPE_PAGES (preferred for RDMA): individual order-0 pages
 *     vmapped into a contiguous kernel VA.  These pages will be handed
 *     to ib_dma_map_sg() during MR registration (Phase 4).
 *   BUF_TYPE_COHERENT: dma_alloc_coherent for physically contiguous
 *     memory.  Useful for small control structures.  Cannot be used for
 *     MR registration (no page array for SG table construction).
 *
 * Both paths support mmap to userspace, completing the zero-copy
 * chain: userspace writes → same physical pages → NIC reads for TX.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>

#include "dmabuf_rdma.h"

/*
 * dmabuf_rdma_create_buffer — Allocate a DMA buffer.
 *
 * Validates size and alloc_type, finds a free slot in the buffer array,
 * allocates memory via the requested path, and returns the handle in
 * params->buf_id.
 *
 * Concurrency: acquires buf_lock for the duration of slot search,
 * allocation, and bookkeeping.  The lock is held during allocation
 * because we need the slot to remain ours — no other thread should
 * claim it between the free-slot scan and the in_use=true write.
 */
int dmabuf_rdma_create_buffer(struct dmaplane_dev *dev,
			      struct dmaplane_buf_params *params)
{
	struct dmaplane_buffer *buf;
	unsigned int nr_pages;
	unsigned int i;
	int slot;

	/* Validate inputs */
	if (params->size == 0 || params->size > DMAPLANE_MAX_BUF_SIZE)
		return -EINVAL;

	if (params->alloc_type != DMAPLANE_BUF_TYPE_COHERENT &&
	    params->alloc_type != DMAPLANE_BUF_TYPE_PAGES)
		return -EINVAL;

	mutex_lock(&dev->buf_lock);

	/* Linear scan for free slot — deliberate simplicity for 64 slots */
	slot = -1;
	for (i = 0; i < DMAPLANE_MAX_BUFFERS; i++) {
		if (!dev->buffers[i].in_use) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		mutex_unlock(&dev->buf_lock);
		return -ENOMEM;
	}

	buf = &dev->buffers[slot];

	/* Zero the slot to clear stale pointers from a previous occupant */
	memset(buf, 0, sizeof(*buf));

	/* Assign monotonically increasing handle, skip 0 on wrap */
	buf->id = dev->next_buf_id++;
	if (dev->next_buf_id == 0)
		dev->next_buf_id = 1;	/* 0 is reserved as "no buffer" sentinel */

	buf->alloc_type = params->alloc_type;
	buf->size = params->size;

	switch (params->alloc_type) {
	case DMAPLANE_BUF_TYPE_COHERENT:
		/*
		 * dma_alloc_coherent returns physically contiguous, cache-coherent
		 * memory with a single DMA address.  The kernel and device see
		 * consistent data without explicit cache flushes.
		 *
		 * Uses &dev->pdev->dev — the platform device, not the char device.
		 * The DMA API programs IOMMU entries for this device's DMA context.
		 */
		buf->vaddr = dma_alloc_coherent(&dev->pdev->dev, buf->size,
						&buf->dma_handle, GFP_KERNEL);
		if (!buf->vaddr) {
			mutex_unlock(&dev->buf_lock);
			return -ENOMEM;
		}
		/*
		 * Zero the memory — prevents leaking kernel data to userspace
		 * via mmap.  dma_alloc_coherent does not guarantee zeroing.
		 */
		memset(buf->vaddr, 0, buf->size);
		break;

	case DMAPLANE_BUF_TYPE_PAGES:
		/*
		 * Allocate individual order-0 pages.  In Phase 2 we use plain
		 * alloc_page(GFP_KERNEL | __GFP_ZERO).  Phase 3 upgrades this
		 * to alloc_pages_node() for NUMA-aware placement.
		 *
		 * __GFP_ZERO: critical — prevents leaking kernel memory to
		 * userspace via mmap.  Without this, stale data from previously
		 * freed pages would be visible to the mapping process.
		 */
		nr_pages = DIV_ROUND_UP(buf->size, PAGE_SIZE);
		buf->pages = kvcalloc(nr_pages, sizeof(struct page *), GFP_KERNEL);
		if (!buf->pages) {
			mutex_unlock(&dev->buf_lock);
			return -ENOMEM;
		}

		for (i = 0; i < nr_pages; i++) {
			buf->pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
			if (!buf->pages[i]) {
				/* Unwind: free all pages allocated so far */
				while (i > 0)
					__free_page(buf->pages[--i]);
				kvfree(buf->pages);
				buf->pages = NULL;
				mutex_unlock(&dev->buf_lock);
				return -ENOMEM;
			}
		}
		buf->nr_pages = nr_pages;

		/*
		 * vmap: create a contiguous kernel virtual address from scattered
		 * physical pages.  Without this, the kernel would need kmap_page()
		 * for each page individually.  The vmapped address is used by the
		 * RDMA engine (Phase 4) as the SGE virtual address for rxe's
		 * memcpy-based send path.
		 */
		buf->vaddr = vmap(buf->pages, nr_pages, VM_MAP, PAGE_KERNEL);
		if (!buf->vaddr) {
			for (i = 0; i < nr_pages; i++)
				__free_page(buf->pages[i]);
			kvfree(buf->pages);
			buf->pages = NULL;
			mutex_unlock(&dev->buf_lock);
			return -ENOMEM;
		}
		break;

	default:
		/* Should not reach here — validated above */
		mutex_unlock(&dev->buf_lock);
		return -EINVAL;
	}

	buf->in_use = true;
	atomic_set(&buf->mmap_count, 0);

	/* Fill output fields for userspace */
	params->buf_id = buf->id;

	atomic64_inc(&dev->stats.buffers_created);

	pr_debug("buffer %u created: type=%d size=%zu nr_pages=%u\n",
		 buf->id, buf->alloc_type, buf->size, buf->nr_pages);

	mutex_unlock(&dev->buf_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(dmabuf_rdma_create_buffer);

/*
 * dmabuf_rdma_destroy_buffer — Free a buffer and its backing memory.
 *
 * Refuses destruction if userspace has active mmap references (mmap_count > 0)
 * to prevent use-after-free.  Cleanup is reverse-order of allocation.
 *
 * Concurrency: acquires buf_lock.  The lock protects the buffer lookup
 * and the in_use flag transition.  The actual memory freeing happens
 * under the lock because we need to prevent a concurrent mmap from
 * finding the buffer while its pages are being freed.
 */
int dmabuf_rdma_destroy_buffer(struct dmaplane_dev *dev, unsigned int buf_id)
{
	struct dmaplane_buffer *buf;
	unsigned int i;

	mutex_lock(&dev->buf_lock);

	buf = dmabuf_rdma_find_buffer(dev, buf_id);
	if (!buf) {
		mutex_unlock(&dev->buf_lock);
		return -ENOENT;
	}

	/* Refuse if userspace still has active mappings */
	if (atomic_read(&buf->mmap_count) > 0) {
		pr_debug("buffer %u: cannot destroy — %d active mmaps\n",
			 buf->id, atomic_read(&buf->mmap_count));
		mutex_unlock(&dev->buf_lock);
		return -EBUSY;
	}

	/* Reverse-order cleanup based on allocation type */
	switch (buf->alloc_type) {
	case DMAPLANE_BUF_TYPE_PAGES:
		if (buf->vaddr)
			vunmap(buf->vaddr);
		if (buf->pages) {
			for (i = 0; i < buf->nr_pages; i++)
				__free_page(buf->pages[i]);
			kvfree(buf->pages);
		}
		break;

	case DMAPLANE_BUF_TYPE_COHERENT:
		if (buf->vaddr)
			dma_free_coherent(&dev->pdev->dev, buf->size,
					  buf->vaddr, buf->dma_handle);
		break;
	}

	pr_debug("buffer %u destroyed\n", buf->id);

	buf->in_use = false;
	atomic64_inc(&dev->stats.buffers_destroyed);

	mutex_unlock(&dev->buf_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(dmabuf_rdma_destroy_buffer);

/*
 * dmabuf_rdma_find_buffer — Look up a buffer by ID.
 *
 * Caller MUST hold buf_lock for the lifetime of the returned pointer.
 * The buffer could be destroyed by another thread between find and use
 * if the lock is not held.
 */
struct dmaplane_buffer *dmabuf_rdma_find_buffer(struct dmaplane_dev *dev,
						unsigned int buf_id)
{
	unsigned int i;

	for (i = 0; i < DMAPLANE_MAX_BUFFERS; i++) {
		if (dev->buffers[i].in_use && dev->buffers[i].id == buf_id)
			return &dev->buffers[i];
	}
	return NULL;
}
EXPORT_SYMBOL_GPL(dmabuf_rdma_find_buffer);
