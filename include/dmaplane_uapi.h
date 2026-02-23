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

/* NUMA placement: -1 = allocate on current CPU's local node (default) */
#define DMAPLANE_NUMA_ANY		(-1)

/*
 * Buffer creation parameters: passed to IOCTL_CREATE_BUFFER.
 * On success, buf_id and actual_numa_node are filled.
 *
 * numa_node controls NUMA placement:
 *   DMAPLANE_NUMA_ANY (-1): allocate on the current CPU's local node
 *     (same as plain alloc_page behavior).
 *   0..N: request allocation on the specified NUMA node.  This is
 *     best-effort — the kernel may fall back silently to other nodes
 *     if the target node is under memory pressure.  actual_numa_node
 *     reports where pages actually landed.
 */
struct dmaplane_buf_params {
	__u32 buf_id;		/* out — assigned buffer handle (never 0) */
	__u32 alloc_type;	/* in  — DMAPLANE_BUF_TYPE_* */
	__u64 size;		/* in  — buffer size in bytes */
	__s32 numa_node;	/* in  — NUMA node (-1 = any/local) */
	__s32 actual_numa_node;	/* out — node where pages were placed */
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
	__u64 buffers_created;		/* out — Lifetime buffers created */
	__u64 buffers_destroyed;	/* out — Lifetime buffers destroyed */
	__u64 numa_local_allocs;	/* out — alloc landed on requested node */
	__u64 numa_remote_allocs;	/* out — alloc fell back to another node */
	__u64 numa_anon_allocs;		/* out — alloc with DMAPLANE_NUMA_ANY */
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

/* ── Phase 4: RDMA integration ───────────────────────────── */

/*
 * RDMA setup parameters: passed to IOCTL_SETUP_RDMA.
 * Creates the full RDMA resource hierarchy: IB device → PD → CQs → QPs.
 * Zero values for cq_depth, max_send_wr, max_recv_wr use defaults.
 */
struct dmaplane_rdma_setup {
	char ib_dev_name[32];	/* in  — IB device name (e.g., "rxe_eth0") */
	__u32 port;		/* in  — IB port number (usually 1; 0 = default) */
	__u32 cq_depth;		/* in  — CQ depth (0 = default 128) */
	__u32 max_send_wr;	/* in  — max send WRs (0 = default 64) */
	__u32 max_recv_wr;	/* in  — max recv WRs (0 = default 64) */
	__u32 status;		/* out — 0 on success */
};

/*
 * MR registration parameters: passed to IOCTL_REGISTER_MR.
 * Registers buffer pages as an RDMA memory region.  For local-only
 * access (IB_ACCESS_LOCAL_WRITE), uses pd->local_dma_lkey (rkey=0).
 * For remote access (REMOTE_WRITE, REMOTE_READ), allocates a fast-reg
 * MR with a real rkey.
 */
struct dmaplane_mr_params {
	__u32 mr_id;		/* out — assigned MR ID */
	__u32 buf_id;		/* in  — buffer to register */
	__u32 access_flags;	/* in  — IB_ACCESS_* flags */
	__u32 lkey;		/* out — local key */
	__u32 rkey;		/* out — remote key (0 for local-only) */
	__u32 _pad;		/* padding — explicit pad for alignment */
	__u64 addr;		/* out — MR base address */
};

/*
 * Loopback test parameters: passed to IOCTL_LOOPBACK_TEST.
 * Single send from QP-A, recv on QP-B.  Validates the RDMA data path
 * works end-to-end; not a throughput measurement.
 */
struct dmaplane_loopback_params {
	__u32 mr_id;		/* in  — MR to use */
	__u32 size;		/* in  — message size in bytes */
	__u32 status;		/* out — 0 on success */
	__u32 pad;		/* padding — explicit pad for alignment */
	__u64 latency_ns;	/* out — round-trip latency in nanoseconds */
};

/*
 * Benchmark parameters: passed to IOCTL_PINGPONG_BENCH and
 * IOCTL_STREAMING_BENCH.  queue_depth is only used by streaming
 * (ignored by ping-pong).
 */
struct dmaplane_bench_params {
	__u32 mr_id;		/* in  — MR to use */
	__u32 msg_size;		/* in  — message size per operation */
	__u32 iterations;	/* in  — number of iterations */
	__u32 queue_depth;	/* in  — outstanding WRs (streaming only; 0 = 16) */
	__u64 total_ns;		/* out — total elapsed time */
	__u64 avg_latency_ns;	/* out — average latency per operation */
	__u64 p99_latency_ns;	/* out — 99th percentile latency */
	__u64 throughput_mbps;	/* out — throughput in MB/s */
	__u64 mr_reg_ns;	/* out — MR registration time */
};

/*
 * RDMA statistics: returned by IOCTL_GET_RDMA_STATS.
 * Lifetime counters — individually consistent, collectively approximate.
 */
struct dmaplane_rdma_stats {
	__u64 mrs_registered;		/* out — lifetime MRs registered */
	__u64 mrs_deregistered;		/* out — lifetime MRs deregistered */
	__u64 sends_posted;		/* out — lifetime send WRs posted */
	__u64 recvs_posted;		/* out — lifetime recv WRs posted */
	__u64 completions_polled;	/* out — lifetime CQ completions polled */
	__u64 completion_errors;	/* out — lifetime CQ completion errors */
	__u64 bytes_sent;		/* out — lifetime bytes sent */
	__u64 bytes_received;		/* out — lifetime bytes received */
};

/* Phase 4 ioctl commands: RDMA 0x10–0x11, MR 0x20–0x21, benchmarks 0x30–0x33 */

/* Initialize RDMA subsystem: IB device → PD → CQs → QPs → loopback. */
#define DMAPLANE_IOCTL_SETUP_RDMA \
	_IOWR(DMAPLANE_IOC_MAGIC, 0x10, struct dmaplane_rdma_setup)

/* Tear down RDMA subsystem; deregisters all MRs first. */
#define DMAPLANE_IOCTL_TEARDOWN_RDMA \
	_IO(DMAPLANE_IOC_MAGIC, 0x11)

/* Register a page-backed buffer as an RDMA MR; returns lkey/rkey. */
#define DMAPLANE_IOCTL_REGISTER_MR \
	_IOWR(DMAPLANE_IOC_MAGIC, 0x20, struct dmaplane_mr_params)

/* Deregister an MR by ID (bare __u32, not a struct). */
#define DMAPLANE_IOCTL_DEREGISTER_MR \
	_IOW(DMAPLANE_IOC_MAGIC, 0x21, __u32)

/* Run single-message loopback test (QP-A → QP-B). */
#define DMAPLANE_IOCTL_LOOPBACK_TEST \
	_IOWR(DMAPLANE_IOC_MAGIC, 0x30, struct dmaplane_loopback_params)

/* Run ping-pong latency benchmark (N iterations). */
#define DMAPLANE_IOCTL_PINGPONG_BENCH \
	_IOWR(DMAPLANE_IOC_MAGIC, 0x31, struct dmaplane_bench_params)

/* Run streaming throughput benchmark (pipelined sends). */
#define DMAPLANE_IOCTL_STREAMING_BENCH \
	_IOWR(DMAPLANE_IOC_MAGIC, 0x32, struct dmaplane_bench_params)

/* Get RDMA statistics (racy snapshot). */
#define DMAPLANE_IOCTL_GET_RDMA_STATS \
	_IOR(DMAPLANE_IOC_MAGIC, 0x33, struct dmaplane_rdma_stats)

/* ── Phase 5: NUMA topology & optimization ───────────────── */

/* Maximum NUMA nodes supported in topology/benchmark structs.
 * 8 covers all practical server topologies (dual=2, quad=4, 8-socket=8). */
#define DMAPLANE_MAX_NUMA_NODES	8

/*
 * NUMA topology: returned by IOCTL_QUERY_NUMA_TOPO.
 *
 * Reports online NUMA nodes, per-node CPU counts, memory sizes,
 * and the ACPI SLIT distance matrix.  Distance values:
 *   10 = same node (local)
 *   20–21 = one QPI/UPI hop (adjacent socket)
 *   30+ = further hops
 *
 * Node IDs may be sparse (e.g., nodes 0 and 2 online, 1 offline).
 * This struct compacts into contiguous indices [0..nr_nodes-1].
 */
struct dmaplane_numa_topo {
	__u32 nr_nodes;		/* out — number of online NUMA nodes */
	__u32 nr_cpus;		/* out — number of online CPUs */
	__u32 node_cpu_count[DMAPLANE_MAX_NUMA_NODES];	/* out — CPUs per node */
	__u32 node_online[DMAPLANE_MAX_NUMA_NODES];	/* out — 1 if online */
	__u64 node_mem_total_kb[DMAPLANE_MAX_NUMA_NODES]; /* out — total mem */
	__u64 node_mem_free_kb[DMAPLANE_MAX_NUMA_NODES];  /* out — free mem */
	/* out — Distance matrix: distances[i][j] = ACPI SLIT distance i to j */
	__u32 distances[DMAPLANE_MAX_NUMA_NODES][DMAPLANE_MAX_NUMA_NODES];
};

/*
 * NUMA bandwidth benchmark: passed to IOCTL_NUMA_BENCH.
 *
 * For each (cpu_node, buf_node) pair, spawns a kthread pinned to
 * cpu_node, measures memcpy throughput to a buffer on buf_node.
 * Result is an NxN bandwidth matrix quantifying cross-node penalty.
 */
struct dmaplane_numa_bench_params {
	__u64 buffer_size;	/* in  — test buffer size per node (bytes) */
	__u32 iterations;	/* in  — memcpy iterations per cell */
	__u32 pad;		/* padding — explicit pad for alignment */
	/* Output: NxN bandwidth matrix in MB/s.
	 * bw_mbps[cpu_node][buf_node] = throughput when a thread on
	 * cpu_node writes to a buffer allocated on buf_node. */
	__u64 bw_mbps[DMAPLANE_MAX_NUMA_NODES][DMAPLANE_MAX_NUMA_NODES];
	/* Output: NxN latency matrix in nanoseconds per iteration. */
	__u64 lat_ns[DMAPLANE_MAX_NUMA_NODES][DMAPLANE_MAX_NUMA_NODES];
	__u32 nr_nodes;		/* out — nodes measured */
	__u32 status;		/* out — 0 on success */
};

/* Phase 5 ioctl commands: NUMA 0x50–0x51 */

/* Query NUMA topology: nodes, CPUs, memory, distance matrix. */
#define DMAPLANE_IOCTL_QUERY_NUMA_TOPO \
	_IOR(DMAPLANE_IOC_MAGIC, 0x50, struct dmaplane_numa_topo)

/* Run NxN cross-node bandwidth benchmark. */
#define DMAPLANE_IOCTL_NUMA_BENCH \
	_IOWR(DMAPLANE_IOC_MAGIC, 0x51, struct dmaplane_numa_bench_params)

/* ── Phase 6: Backpressure & throughput modeling ─────────── */

/*
 * Flow control configuration: passed to IOCTL_CONFIGURE_FLOW.
 *
 * Establishes credit-based backpressure for the RDMA send path.
 * max_credits limits outstanding in-flight operations — setting it
 * at or below CQ depth makes CQ overflow impossible by construction.
 *
 * Watermark hysteresis prevents oscillation:
 *   - Sender pauses when in-flight count reaches high_watermark.
 *   - Sender resumes only when in-flight drops below low_watermark.
 * Without hysteresis, the sender would toggle between send/stall on
 * every single credit change, destroying throughput.
 */
struct dmaplane_flow_params {
	__u32 max_credits;	/* in  — total credits (max in-flight ops) */
	__u32 high_watermark;	/* in  — in-flight count to pause sending */
	__u32 low_watermark;	/* in  — in-flight count to resume sending */
	__u32 status;		/* out — 0 on success */
};

/*
 * Sustained streaming parameters: passed to IOCTL_SUSTAINED_STREAM.
 *
 * Runs a pipelined send/recv benchmark for a configurable wall-clock
 * duration (not iteration count).  Per-second windowing tracks throughput
 * stability — min/max windows reveal variance that averages hide.
 *
 * If CONFIGURE_FLOW was not called, auto-configures with conservative
 * defaults: max_credits = min(2*queue_depth, 128), high = 3/4, low = 1/4.
 */
struct dmaplane_sustained_params {
	__u32 mr_id;			/* in  — memory region to stream */
	__u32 msg_size;			/* in  — bytes per send */
	__u32 queue_depth;		/* in  — pipeline depth */
	__u32 duration_secs;		/* in  — how long to stream (1–600) */
	__u64 total_bytes;		/* out — total bytes transferred */
	__u64 total_ops;		/* out — total send/recv completions */
	__u64 avg_throughput_mbps;	/* out — bytes * 1000 / elapsed_ns */
	__u64 min_window_mbps;		/* out — worst 1-second window */
	__u64 max_window_mbps;		/* out — best 1-second window */
	__u64 stall_count;		/* out — times sender blocked on credits */
	__u64 cq_overflow_count;	/* out — should be 0 */
	__u32 status;			/* out — 0 on success */
	__u32 pad;			/* padding — explicit pad for alignment */
};

/*
 * Queue depth sweep parameters: passed to IOCTL_QDEPTH_SWEEP.
 *
 * Iterates across queue depths from min_qdepth to max_qdepth,
 * running a fixed-iteration streaming benchmark at each point.
 * Produces throughput/latency curves and detects the saturation
 * point — the smallest queue depth where throughput reaches 95%
 * of the maximum observed.
 */
#define DMAPLANE_MAX_SWEEP_POINTS	32

struct dmaplane_sweep_params {
	__u32 mr_id;		/* in  — MR to use */
	__u32 msg_size;		/* in  — message size per operation */
	__u32 iterations;	/* in  — iterations per queue depth point */
	__u32 min_qdepth;	/* in  — starting queue depth */
	__u32 max_qdepth;	/* in  — ending queue depth */
	__u32 step;		/* in  — queue depth increment */
	__u64 throughput_mbps[DMAPLANE_MAX_SWEEP_POINTS];	/* out — MB/s per point */
	__u64 avg_latency_ns[DMAPLANE_MAX_SWEEP_POINTS];	/* out — avg latency per point */
	__u64 p99_latency_ns[DMAPLANE_MAX_SWEEP_POINTS];	/* out — P99 latency per point */
	__u32 nr_points;		/* out — actual points measured */
	__u32 saturation_qdepth;	/* out — QD where throughput plateaus */
	__u32 status;			/* out — 0 on success */
	__u32 pad;			/* padding — explicit pad for alignment */
};

/*
 * Flow control statistics: returned by IOCTL_GET_FLOW_STATS.
 * Lifetime counters — cumulative across all sustained streaming runs.
 * Separately tracked from RDMA stats to avoid ABI changes.
 */
struct dmaplane_flow_stats {
	__u64 credit_stalls;		/* out — sender blocked on zero credits */
	__u64 high_watermark_events;	/* out — times high watermark reached */
	__u64 low_watermark_events;	/* out — times low watermark crossed */
	__u64 cq_overflows;		/* out — CQ full events (should be 0) */
	__u64 total_sustained_bytes;	/* out — cumulative sustained bytes */
	__u64 total_sustained_ops;	/* out — cumulative sustained ops */
};

/* Phase 6 ioctl commands: flow control 0x60–0x63 */

/* Configure credit-based flow control parameters. */
#define DMAPLANE_IOCTL_CONFIGURE_FLOW \
	_IOWR(DMAPLANE_IOC_MAGIC, 0x60, struct dmaplane_flow_params)

/* Run sustained streaming benchmark for a wall-clock duration. */
#define DMAPLANE_IOCTL_SUSTAINED_STREAM \
	_IOWR(DMAPLANE_IOC_MAGIC, 0x61, struct dmaplane_sustained_params)

/* Run queue depth sweep: throughput/latency across queue depths. */
#define DMAPLANE_IOCTL_QDEPTH_SWEEP \
	_IOWR(DMAPLANE_IOC_MAGIC, 0x62, struct dmaplane_sweep_params)

/* Get flow control statistics (racy snapshot). */
#define DMAPLANE_IOCTL_GET_FLOW_STATS \
	_IOR(DMAPLANE_IOC_MAGIC, 0x63, struct dmaplane_flow_stats)

/* ── Phase 7: Instrumentation ─────────────────────────────── */

/*
 * Latency histogram: log₂ buckets in microseconds.
 * 16 buckets cover [0, 1), [1, 2), [2, 4), ... [32768, ∞) µs.
 * For rxe (~200 µs per op), most samples land in buckets 7-8.
 * For ConnectX hardware (~1-5 µs), buckets 0-2.
 *
 * Coarse but constant-space: 128 bytes for bucket counters plus
 * metadata.  Trade precision for zero-allocation O(1) recording.
 * Exact percentiles require sorting N samples (what QD sweep does);
 * the histogram gives approximate percentiles with O(1) memory.
 */
#define DMAPLANE_HIST_BUCKETS	16

/*
 * Histogram output: returned by IOCTL_GET_HISTOGRAM.
 * Bucket counts + computed percentiles + summary statistics.
 * Set reset=1 to atomically read-and-clear the histogram.
 */
struct dmaplane_hist_params {
	__u64 buckets[DMAPLANE_HIST_BUCKETS];	/* out — sample count per bucket */
	__u64 count;		/* out — total samples recorded */
	__u64 p50_ns;		/* out — 50th percentile (bucket upper bound) */
	__u64 p99_ns;		/* out — 99th percentile (bucket upper bound) */
	__u64 p999_ns;		/* out — 99.9th percentile (bucket upper bound) */
	__u64 avg_ns;		/* out — arithmetic mean of all samples */
	__u64 min_ns;		/* out — minimum latency observed */
	__u64 max_ns;		/* out — maximum latency observed */
	__u32 reset;		/* in  — 1 = reset histogram after reading */
	__u32 pad;		/* padding — explicit pad for alignment */
};

/* Phase 7 ioctl commands: instrumentation 0x70 */

/* Read latency histogram; optionally reset after reading. */
#define DMAPLANE_IOCTL_GET_HISTOGRAM \
	_IOWR(DMAPLANE_IOC_MAGIC, 0x70, struct dmaplane_hist_params)

#endif /* _DMAPLANE_UAPI_H */
