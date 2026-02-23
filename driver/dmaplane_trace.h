/* SPDX-License-Identifier: GPL-2.0 */
/*
 * dmaplane_trace.h — Kernel tracepoint definitions for dmaplane
 * Copyright (c) 2026 Graziano Labs Corp.
 *
 * Defines 6 tracepoints covering the core data paths:
 *   dmaplane_ring_submit     — ioctl submit to submission ring
 *   dmaplane_ring_complete   — ioctl complete from completion ring
 *   dmaplane_rdma_post       — RDMA work request posted (send or recv)
 *   dmaplane_rdma_completion — CQ completion polled
 *   dmaplane_buf_alloc       — DMA buffer allocated
 *   dmaplane_flow_stall      — credit stall in sustained streaming
 *
 * When tracing is disabled (default), each tracepoint compiles to a
 * single static-branch NOP (<1 ns).  When enabled via ftrace, each
 * event writes to the ring buffer (~100-200 ns).
 *
 * Build requirements:
 *   - Exactly one .c file must define CREATE_TRACE_POINTS before
 *     including this header (dmaplane_trace.c).
 *   - All other .c files include this header without CREATE_TRACE_POINTS.
 *   - TRACE_INCLUDE_PATH must be . (dot) for same-directory resolution.
 *   - ccflags-y += -I$(src) in the Makefile for out-of-tree builds.
 *
 * Kernel 6.5: __assign_str takes two arguments (field, source).
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM dmaplane

#if !defined(_DMAPLANE_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _DMAPLANE_TRACE_H

#include <linux/tracepoint.h>
#include <linux/types.h>

/*
 * dmaplane_ring_submit — fires on each successful ioctl submit.
 * Records the channel, payload, and current queue depth.
 */
TRACE_EVENT(dmaplane_ring_submit,
	TP_PROTO(unsigned int channel_id, __u64 payload,
		 unsigned int queue_depth),
	TP_ARGS(channel_id, payload, queue_depth),
	TP_STRUCT__entry(
		__field(unsigned int, channel_id)
		__field(__u64, payload)
		__field(unsigned int, queue_depth)
	),
	TP_fast_assign(
		__entry->channel_id = channel_id;
		__entry->payload = payload;
		__entry->queue_depth = queue_depth;
	),
	TP_printk("chan=%u payload=%llu qdepth=%u",
		__entry->channel_id, __entry->payload, __entry->queue_depth)
);

/*
 * dmaplane_ring_complete — fires on each successful ioctl complete.
 * Records the channel and dequeued payload.
 */
TRACE_EVENT(dmaplane_ring_complete,
	TP_PROTO(unsigned int channel_id, __u64 payload),
	TP_ARGS(channel_id, payload),
	TP_STRUCT__entry(
		__field(unsigned int, channel_id)
		__field(__u64, payload)
	),
	TP_fast_assign(
		__entry->channel_id = channel_id;
		__entry->payload = payload;
	),
	TP_printk("chan=%u payload=%llu",
		__entry->channel_id, __entry->payload)
);

/*
 * dmaplane_rdma_post — fires on each RDMA work request post.
 * op is "send" or "recv".  msg_size is the SGE length.
 */
TRACE_EVENT(dmaplane_rdma_post,
	TP_PROTO(const char *op, unsigned int msg_size, __u64 wr_id),
	TP_ARGS(op, msg_size, wr_id),
	TP_STRUCT__entry(
		__string(op, op)
		__field(unsigned int, msg_size)
		__field(__u64, wr_id)
	),
	TP_fast_assign(
		__assign_str(op, op);	/* kernel 6.5: two-arg form required */
		__entry->msg_size = msg_size;
		__entry->wr_id = wr_id;
	),
	TP_printk("op=%s size=%u wr_id=%llu",
		__get_str(op), __entry->msg_size, __entry->wr_id)
);

/*
 * dmaplane_rdma_completion — fires on each CQ poll that returns
 * a completion.  latency_ns is 0 at the engine layer; per-operation
 * latency is tracked in the benchmark/histogram layer.
 */
TRACE_EVENT(dmaplane_rdma_completion,
	TP_PROTO(int status, unsigned int byte_len, __u64 latency_ns),
	TP_ARGS(status, byte_len, latency_ns),
	TP_STRUCT__entry(
		__field(int, status)
		__field(unsigned int, byte_len)
		__field(__u64, latency_ns)
	),
	TP_fast_assign(
		__entry->status = status;
		__entry->byte_len = byte_len;
		__entry->latency_ns = latency_ns;
	),
	TP_printk("status=%d bytes=%u lat_ns=%llu",
		__entry->status, __entry->byte_len, __entry->latency_ns)
);

/*
 * dmaplane_buf_alloc — fires on buffer allocation completion.
 * duration_ns includes lock contention and allocation time.
 */
TRACE_EVENT(dmaplane_buf_alloc,
	TP_PROTO(unsigned int buf_id, __u64 size, int alloc_type,
		 int numa_node, __u64 duration_ns),
	TP_ARGS(buf_id, size, alloc_type, numa_node, duration_ns),
	TP_STRUCT__entry(
		__field(unsigned int, buf_id)
		__field(__u64, size)
		__field(int, alloc_type)
		__field(int, numa_node)
		__field(__u64, duration_ns)
	),
	TP_fast_assign(
		__entry->buf_id = buf_id;
		__entry->size = size;
		__entry->alloc_type = alloc_type;
		__entry->numa_node = numa_node;
		__entry->duration_ns = duration_ns;
	),
	TP_printk("buf=%u size=%llu type=%d node=%d dur_ns=%llu",
		__entry->buf_id, __entry->size, __entry->alloc_type,
		__entry->numa_node, __entry->duration_ns)
);

/*
 * dmaplane_flow_stall — fires on each credit stall in sustained
 * streaming.  stall_ns measures the cond_resched() yield time.
 */
TRACE_EVENT(dmaplane_flow_stall,
	TP_PROTO(int credits, unsigned int in_flight, __u64 stall_ns),
	TP_ARGS(credits, in_flight, stall_ns),
	TP_STRUCT__entry(
		__field(int, credits)
		__field(unsigned int, in_flight)
		__field(__u64, stall_ns)
	),
	TP_fast_assign(
		__entry->credits = credits;
		__entry->in_flight = in_flight;
		__entry->stall_ns = stall_ns;
	),
	TP_printk("credits=%d in_flight=%u stall_ns=%llu",
		__entry->credits, __entry->in_flight, __entry->stall_ns)
);

#endif /* _DMAPLANE_TRACE_H */

/* This part must be outside the header guard */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE dmaplane_trace
#include <trace/define_trace.h>
