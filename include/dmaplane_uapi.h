/* SPDX-License-Identifier: GPL-2.0 */
/*
 * dmaplane — Userspace API header
 * Copyright (c) 2026 Graziano Labs Corp.
 *
 * Ioctl numbers, parameter structs, and constants shared between
 * the kernel module and userspace programs. Includable from both
 * kernel (__KERNEL__) and userspace.
 */
#ifndef _DMAPLANE_UAPI_H
#define _DMAPLANE_UAPI_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>

/* Map kernel fixed-width types to userspace equivalents */
typedef uint8_t  __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef int32_t  __s32;
#endif

/* Module name */
#define DMAPLANE_NAME		"dmaplane"

/* Ring buffer constants */
#define DMAPLANE_MAX_CHANNELS	4
/*
 * Must be a power of 2 so that (index % DMAPLANE_RING_SIZE) reduces to
 * a bitmask, and unsigned wrap-around arithmetic on head/tail yields
 * correct occupancy counts.
 */
#define DMAPLANE_RING_SIZE	1024

/* Ioctl magic number */
#define DMAPLANE_IOC_MAGIC	0xE4

/*
 * Ring entry: the fundamental unit passed through submission and
 * completion rings. payload carries the work item data; flags is
 * reserved for future per-entry metadata.
 */
struct dmaplane_ring_entry {
	__u64 payload;	/* in/out — work item data; worker adds 1 in Phase 1 */
	__u32 flags;	/* in/out — reserved for future per-entry metadata */
	__u32 _pad;	/* padding — explicit pad to 16-byte struct alignment */
};

/*
 * Submit parameters: userspace passes this to IOCTL_SUBMIT.
 * The entry is copied into the submission ring.
 */
struct dmaplane_submit_params {
	struct dmaplane_ring_entry entry;	/* in — entry to enqueue */
};

/*
 * Complete parameters: userspace passes this to IOCTL_COMPLETE.
 * On success, the entry is filled with the next completion.
 */
struct dmaplane_complete_params {
	struct dmaplane_ring_entry entry;	/* out — dequeued completion */
};

/*
 * Channel creation parameters: returned to userspace after
 * IOCTL_CREATE_CHANNEL. channel_id is the assigned channel index.
 */
struct dmaplane_channel_params {
	__u32 channel_id;	/* out — assigned channel index [0, MAX_CHANNELS) */
	__u32 _pad;			/* padding — explicit pad for alignment */
};

/*
 * Per-channel statistics returned by IOCTL_GET_STATS.
 * This is a racy snapshot — fields are updated without locks on the
 * hot path.  Values are monotonically increasing and individually
 * consistent, but the set may be momentarily inconsistent.
 */
struct dmaplane_stats {
	__u64 total_submissions;	/* Total entries submitted */
	__u64 total_completions;	/* Total entries completed by the worker */
	__u32 ring_high_watermark;	/* Max submission ring occupancy observed */
	__u32 dropped_count;		/* Entries dropped due to completion ring
								* full.  Phase 1 never drops — the worker
								* yields until space is available — so
								* this reads 0.  Retained as a placeholder
								* for future backpressure policies. */
};

/* Phase 1 ioctl commands: 0x01–0x04 */

/* Allocate a channel and start its worker kthread; returns channel_id. */
#define DMAPLANE_IOCTL_CREATE_CHANNEL \
	_IOR(DMAPLANE_IOC_MAGIC, 0x01, struct dmaplane_channel_params)

/* Enqueue one entry into the channel's submission ring. */
#define DMAPLANE_IOCTL_SUBMIT \
	_IOW(DMAPLANE_IOC_MAGIC, 0x02, struct dmaplane_submit_params)

/* Dequeue one entry from the channel's completion ring (non-blocking). */
#define DMAPLANE_IOCTL_COMPLETE \
	_IOWR(DMAPLANE_IOC_MAGIC, 0x03, struct dmaplane_complete_params)

/* Copy per-channel statistics to userspace (racy snapshot). */
#define DMAPLANE_IOCTL_GET_STATS \
	_IOR(DMAPLANE_IOC_MAGIC, 0x04, struct dmaplane_stats)

/* ── Phase 2: Buffer management ──────────────────────────── */

/*
 * Buffer allocation types.
 *
 * BUF_TYPE_COHERENT: dma_alloc_coherent — physically contiguous, cache-
 *   coherent memory with a single DMA address.  Used for small, hot control
 *   structures (CQ entries, doorbells) where CPU and device need coherent
 *   access without explicit sync barriers.
 *
 * BUF_TYPE_PAGES: alloc_pages + vmap — scattered physical pages vmapped
 *   into a contiguous kernel VA.  Used for large streaming buffers
 *   (gradients, weights, KV-cache).  NUMA-steerable in Phase 3 via
 *   alloc_pages_node().  These pages are later handed to ib_dma_map_sg()
 *   during MR registration (Phase 4).
 */
#define DMAPLANE_BUF_TYPE_COHERENT	0
#define DMAPLANE_BUF_TYPE_PAGES		1

/*
 * Buffer creation parameters: passed to IOCTL_CREATE_BUFFER.
 * On success, buf_id is filled with the assigned buffer handle.
 */
struct dmaplane_buf_params {
	__u32 buf_id;		/* out — assigned buffer handle (never 0) */
	__u32 alloc_type;	/* in  — DMAPLANE_BUF_TYPE_* */
	__u64 size;		/* in  — buffer size in bytes */
};

/*
 * mmap information: returned by IOCTL_GET_MMAP_INFO.
 * Userspace uses mmap_offset and mmap_size to call mmap(2):
 *   mmap(NULL, mmap_size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, mmap_offset)
 */
struct dmaplane_mmap_info {
	__u32 buf_id;		/* in  — buffer handle */
	__u32 _pad;		/* padding — explicit pad for natural alignment */
	__u64 mmap_offset;	/* out — offset to pass to mmap(2) */
	__u64 mmap_size;	/* out — mappable size in bytes */
};

/*
 * Buffer statistics: returned by IOCTL_GET_BUF_STATS.
 * Racy snapshot — individually consistent, collectively approximate.
 */
struct dmaplane_buf_stats {
	__u64 buffers_created;	/* out — Lifetime buffers created */
	__u64 buffers_destroyed;/* out — Lifetime buffers destroyed */
};

/* Phase 2 ioctl commands: 0x05–0x09 */

/* Allocate a DMA buffer; returns buffer handle in buf_id. */
#define DMAPLANE_IOCTL_CREATE_BUFFER \
	_IOWR(DMAPLANE_IOC_MAGIC, 0x05, struct dmaplane_buf_params)

/* Destroy a buffer by handle (bare __u32, not a struct). */
#define DMAPLANE_IOCTL_DESTROY_BUFFER \
	_IOW(DMAPLANE_IOC_MAGIC, 0x06, __u32)

/* Get mmap offset and size for a buffer. */
#define DMAPLANE_IOCTL_GET_MMAP_INFO \
	_IOWR(DMAPLANE_IOC_MAGIC, 0x08, struct dmaplane_mmap_info)

/* Get buffer allocation statistics. */
#define DMAPLANE_IOCTL_GET_BUF_STATS \
	_IOR(DMAPLANE_IOC_MAGIC, 0x09, struct dmaplane_buf_stats)

/* ── Phase 3: dma-buf export ─────────────────────────────── */

/*
 * dma-buf export parameters: passed to IOCTL_EXPORT_DMABUF.
 * Wraps an existing page-backed buffer as a dma-buf and returns
 * a file descriptor.  Coherent buffers cannot be exported (no
 * page array for SG table construction).
 */
struct dmaplane_export_dmabuf_arg {
	__u32 buf_id;		/* in  — buffer handle to export */
	__s32 fd;		/* out — dma-buf file descriptor */
};

/*
 * dma-buf statistics: returned by IOCTL_GET_DMABUF_STATS.
 * Lifetime counters — survive individual export/release cycles.
 */
struct dmaplane_dmabuf_stats {
	__u64 dmabufs_exported;		/* out — lifetime dma-bufs created */
	__u64 dmabufs_released;		/* out — lifetime dma-bufs released */
	__u64 attachments_total;	/* out — lifetime attach calls */
	__u64 detachments_total;	/* out — lifetime detach calls */
	__u64 maps_total;		/* out — lifetime map_dma_buf calls */
	__u64 unmaps_total;		/* out — lifetime unmap_dma_buf calls */
};

/* Phase 3 ioctl commands: 0x0A–0x0B */

/* Export a page-backed buffer as a dma-buf; returns fd. */
#define DMAPLANE_IOCTL_EXPORT_DMABUF \
	_IOWR(DMAPLANE_IOC_MAGIC, 0x0A, struct dmaplane_export_dmabuf_arg)

/* Get dma-buf export statistics. */
#define DMAPLANE_IOCTL_GET_DMABUF_STATS \
	_IOR(DMAPLANE_IOC_MAGIC, 0x0B, struct dmaplane_dmabuf_stats)

#endif /* _DMAPLANE_UAPI_H */
