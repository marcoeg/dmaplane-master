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
#define DMAPLANE_RING_SIZE	1024	/* Must be power of 2 */

/* Ioctl magic number */
#define DMAPLANE_IOC_MAGIC	0xE4

/*
 * Ring entry: the fundamental unit passed through submission and
 * completion rings. payload carries the work item data; flags is
 * reserved for future per-entry metadata.
 */
struct dmaplane_ring_entry {
	__u64 payload;
	__u32 flags;
	__u32 _pad;	/* Explicit padding for alignment */
};

/*
 * Submit parameters: userspace passes this to IOCTL_SUBMIT.
 * The entry is copied into the submission ring.
 */
struct dmaplane_submit_params {
	struct dmaplane_ring_entry entry;
};

/*
 * Complete parameters: userspace passes this to IOCTL_COMPLETE.
 * On success, the entry is filled with the next completion.
 */
struct dmaplane_complete_params {
	struct dmaplane_ring_entry entry;
};

/*
 * Channel creation parameters: returned to userspace after
 * IOCTL_CREATE_CHANNEL. channel_id is the assigned channel index.
 */
struct dmaplane_channel_params {
	__u32 channel_id;
	__u32 _pad;
};

/*
 * Per-channel statistics returned by IOCTL_GET_STATS.
 */
struct dmaplane_stats {
	__u64 total_submissions;	/* Total entries submitted */
	__u64 total_completions;	/* Total entries completed */
	__u32 ring_high_watermark;	/* Max occupancy seen */
	__u32 dropped_count;		/* Entries dropped (ring full) */
};

/* Phase 1 ioctl commands: 0x01–0x04 */
#define DMAPLANE_IOCTL_CREATE_CHANNEL \
	_IOR(DMAPLANE_IOC_MAGIC, 0x01, struct dmaplane_channel_params)

#define DMAPLANE_IOCTL_SUBMIT \
	_IOW(DMAPLANE_IOC_MAGIC, 0x02, struct dmaplane_submit_params)

#define DMAPLANE_IOCTL_COMPLETE \
	_IOWR(DMAPLANE_IOC_MAGIC, 0x03, struct dmaplane_complete_params)

#define DMAPLANE_IOCTL_GET_STATS \
	_IOR(DMAPLANE_IOC_MAGIC, 0x04, struct dmaplane_stats)

#endif /* _DMAPLANE_UAPI_H */
