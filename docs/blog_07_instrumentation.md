<!-- SPDX-License-Identifier: GPL-2.0 -->
<!-- Copyright (c) 2026 Graziano Labs Corp. -->

# Instrumenting the Kernel Data Path: Tracepoints, Histograms, and Where the Time Goes

*Part 7 of 9 in a series on building a host-side data path emulator for AI infrastructure*

---

You cannot optimize what you cannot measure. Six phases of dmaplane build the data path -- rings, DMA allocation, NUMA placement, RDMA transport, zero-copy sharing, flow control -- but every throughput number reported so far is an aggregate. Phase 6's sustained streaming reports 1037 MB/s average, but it does not answer the question that matters for optimization: where does the time go? Is the bottleneck in `ib_post_send`? In CQ polling? In the `cond_resched()` yield during credit stalls? In cache line bouncing between the send thread and the completion path? Phase 7 adds the measurement layer that turns these questions into data: kernel tracepoints with zero disabled-state overhead, debugfs files for live inspection without rebooting, log2 latency histograms that capture distribution shape in constant space, and cacheline alignment that eliminates false sharing on hot data structures.

---

## Kernel Tracepoints

**Static tracepoints compile to a single NOP instruction when disabled and a ring-buffer write when enabled, making them safe to leave in production code permanently.** The `TRACE_EVENT` macro from `<linux/tracepoint.h>` generates the tracepoint definition, the argument marshalling code, and the `trace_*()` call site in one declaration. When tracing is disabled (the default), the call site is a static branch that the CPU's branch predictor resolves in under a nanosecond. When enabled via ftrace, each event writes a structured record to the per-CPU trace ring buffer at roughly 100-200 ns per event.

The alternative -- `printk` or `pr_debug` -- is worse in every dimension. `printk` acquires a global lock, formats a string, and writes to the log buffer on every call regardless of whether anyone is reading. `pr_debug` compiles to nothing when `DEBUG` is undefined, but enabling it requires recompilation. Tracepoints are always compiled in, always zero-cost when disabled, and toggled at runtime without module reload.

dmaplane defines six tracepoints covering the core data paths:

```
dmaplane:dmaplane_ring_submit
dmaplane:dmaplane_ring_complete
dmaplane:dmaplane_rdma_post
dmaplane:dmaplane_rdma_completion
dmaplane:dmaplane_buf_alloc
dmaplane:dmaplane_flow_stall
```

The RDMA post tracepoint captures the operation type, message size, and work request ID:

```c
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
```

The build constraint for tracepoints is strict: exactly one `.c` file per module must define `CREATE_TRACE_POINTS` before including the trace header. Defining it in two files produces duplicate symbol errors. All other `.c` files include the header without the define to get the `trace_dmaplane_*()` call stubs. dmaplane uses a dedicated `dmaplane_trace.c` as the single compilation unit.

Enabling a tracepoint at runtime requires no module reload:

```bash
# Enable the RDMA post tracepoint
echo 1 > /sys/kernel/debug/tracing/events/dmaplane/dmaplane_rdma_post/enable

# Run a benchmark, then read the trace
cat /sys/kernel/debug/tracing/trace_pipe
#  dmaplane-bench-1234 [003] .... 12345.678: dmaplane_rdma_post: op=send size=4096 wr_id=0
```

---

## debugfs

**debugfs provides live visibility into driver state without rebooting, recompiling, or re-running tests.** After loading the module, five files appear under `/sys/kernel/debug/dmaplane/`: `stats`, `buffers`, `rdma`, `flow`, and `histogram`. Each is a read-only seq_file that walks the driver's internal data structures and formats them for human consumption.

The stats file aggregates every atomic counter the driver maintains -- channels, opens, buffers, NUMA placement, dma-buf lifecycle, RDMA operations, and flow control events:

```
=== dmaplane device stats ===
channels:       active=0 created=10 destroyed=10
opens:          total=18 closes=18
buffers:        created=1102 destroyed=1101
  numa:         local=2 remote=0 anon=1100
dmabuf:         exported=11 released=11 attach=0 detach=0 map=0 unmap=0
rdma:           mrs_reg=3 mrs_dereg=3 sends=5067157 recvs=5067174 completions=10126280 errors=0
  bytes:        sent=20738617408 received=20738617408
flow:           stalls=54909297 high_wm=657088 low_wm=657087 overflows=0
  sustained:    bytes=20725575680 ops=5059955
```

The choice of debugfs over procfs or sysfs is deliberate. procfs is for process-related information. sysfs is for device attributes with stable ABI guarantees -- once you expose a sysfs attribute, you must maintain backward compatibility across kernel versions. debugfs is explicitly for debugging. It carries no ABI stability contract. The format can change between phases, fields can be added or removed, and no userspace tool should depend on parsing the output. This freedom is essential for an evolving project where the counter set grows with every phase.

The debugfs lifecycle is straightforward: `dmaplane_debugfs_init()` runs after all subsystems are initialized, creating the directory and files. `dmaplane_debugfs_exit()` runs before device teardown, removing everything atomically via `debugfs_remove_recursive()`. If debugfs is not mounted or `CONFIG_DEBUG_FS=n`, initialization returns silently -- debugfs is never fatal. The seq_file show functions acquire the appropriate locks (buf_lock for buffers, rdma_sem for RDMA state) to produce consistent snapshots.

---

## Latency Histograms

**Log2 histograms capture distribution shape in constant space -- 16 buckets, 128 bytes of counters, no allocation, O(1) per sample.** Each bucket covers a power-of-two range in microseconds: bucket 0 is [0, 1) us, bucket 4 is [16, 32) us, bucket 7 is [128, 256) us. A latency sample in nanoseconds is divided by 1000, then `ilog2` assigns it to a bucket. The result is a coarse but useful distribution that reveals whether latencies are clustered tightly (one dominant bucket) or spread across orders of magnitude (a scheduling or contention problem).

Why P999 matters at scale: a system running 10 million completions per second and experiencing P999 = 64 us means roughly 10,000 operations per second hit that tail latency. If each tail event triggers a cascade -- a credit stall, a CQ drain delay, a scheduling yield that blocks the next batch -- P999 becomes the throughput-determining factor, not P50.

The histogram from a 5-second sustained streaming run on rxe (4 KB messages, queue depth 8) reveals the actual RDMA pipe time:

```
=== rdma latency histogram ===
samples: 1237157  avg: 31818 ns  min: 4258 ns  max: 233788 ns
p50: 32000 ns  p99: 64000 ns  p999: 64000 ns

bucket        range_us     count    pct    cumulative
  2       [    4,     8)       30    0.0%     0.0%
  3       [    8,    16)       81    0.0%     0.0%
  4       [   16,    32)  1000189   80.8%    80.8%
  5       [   32,    64)   236540   19.1%    99.9%
  6       [   64,   128)      225    0.0%    99.9%
  7       [  128,   256)       92    0.0%   100.0%
```

The critical insight: this histogram measures send-to-completion latency -- the time between posting a work request and polling its completion from the CQ. The dominant bucket is [16, 32) microseconds, with an average of 31.8 us. But Phase 4's pingpong benchmark reported ~200 us per round trip. The difference is scheduling overhead. The pingpong benchmark measures the full cycle: post send, poll send CQ, post recv on the other QP, poll recv CQ, yield via `cond_resched()`, repeat. The ~200 us includes two `cond_resched()` yields, two CQ polls with `usleep_range(50, 200)` backoff, and the rxe softirq scheduling latency. The histogram strips all of that away and reveals the actual RDMA pipe time: 16-32 us for the NIC (rxe in software) to DMA-read the source buffer, process the work request, and write the CQE. This is exactly the kind of signal instrumentation should produce -- separating the mechanism from the overhead.

The implementation is lock-free. Bucket increments, count, and sum use `atomic64_inc` and `atomic64_add`. Min and max use `atomic64_try_cmpxchg` in a CAS loop:

```c
/* Update min — relaxed CAS loop */
old_min = atomic64_read(&hist->min_ns);
while (latency_ns < old_min) {
	if (atomic64_try_cmpxchg(&hist->min_ns, &old_min, latency_ns))
		break;
}
```

The CAS loop may momentarily lose a race under extreme contention -- two threads recording nearly identical minimums -- but it converges to the correct value. For a diagnostic tool, this is the right tradeoff: no lock contention on the hot recording path, at the cost of a transient one-sample inaccuracy that corrects itself on the next update.

Percentile computation walks the 16 buckets once, accumulating counts until the cumulative fraction crosses the target threshold. P50 is the upper bound of the bucket where cumulative reaches 50%. This is coarse -- reporting 32000 ns when the true median might be 28000 ns -- but the alternative is sorting millions of samples, which is what the QD sweep's exact P99 calculation does. The histogram and the sweep are complementary: the histogram gives real-time approximate distribution at zero cost, the sweep gives exact percentiles at O(N log N) cost.

---

## Cacheline Alignment

**False sharing occurs when two independent variables share a cache line and are written by different CPUs, causing the line to bounce between L1 caches at every write.** On x86, a cache line is 64 bytes. If the ring buffer's `head` (written by the producer) and `tail` (written by the consumer) sit in the same 64-byte region, every producer write invalidates the consumer's cached copy of `tail`, and vice versa. The cost is a cache-to-cache transfer on every operation -- roughly 40-70 ns on modern hardware, invisible in profiling because it shows up as increased memory access latency rather than a discrete event.

The fix is `____cacheline_aligned_in_smp`, a kernel macro that pads a field to start at a cache line boundary on SMP builds (no-op on uniprocessor):

```c
struct dmaplane_ring {
	struct dmaplane_ring_entry entries[DMAPLANE_RING_SIZE];
	spinlock_t lock;

	unsigned int head ____cacheline_aligned_in_smp;  /* Producer writes */
	unsigned int tail ____cacheline_aligned_in_smp;  /* Consumer reads  */
};
```

Head and tail now live on separate cache lines. The producer can increment head without touching the consumer's cache line, and the consumer can read tail without bouncing head.

On rxe, the effect is invisible. The dominant latency per operation is `cond_resched()` (yielding the CPU for ~100-200 us) and the rxe softirq processing time (~16-32 us). A 50 ns cache line transfer is noise at the scale of microsecond operations. On real RDMA hardware (ConnectX-6, EFA), where per-operation latency is 1-5 us and CQ polling runs in a tight loop without yields, false sharing on head/tail would consume 5-10% of the per-operation budget. The alignment costs nothing (a few hundred bytes of padding per ring) and eliminates a class of performance bug that only manifests under high-rate polling on real hardware.

The same principle applies to the flow control `credits` atomic, which is incremented by the completion path and read by the send loop. In sustained streaming, these run on the same CPU (IB_POLL_DIRECT means synchronous polling), so false sharing does not arise in the current design. If the module were extended to use `IB_POLL_SOFTIRQ` or `IB_POLL_WORKQUEUE`, the completion callback would run on a different CPU, and false sharing on `credits` would become measurable.

---

## perf + ftrace Together

**`perf stat` measures hardware counters -- cache misses, TLB misses, branch mispredictions -- that explain *why* code is slow. ftrace measures function-level timing that shows *where* code is slow.** Using both together connects microarchitectural behavior to specific code paths.

Hardware counters via `perf stat`:

```bash
# Run the sustained streaming test under perf stat
sudo perf stat -e cache-misses,cache-references,dTLB-load-misses,dTLB-store-misses \
    ./tests/test_phase7_instrumentation
```

The TLB miss counter connects directly to Phase 2's hugepage work. A 16 MB buffer mapped with 4K pages produces 4096 TLB entries; mapped with 2M hugepages, it produces 8. Under sustained streaming where every send touches the buffer, the 4K path generates measurably more dTLB-load-misses. `perf stat` provides the number; Phase 2's allocation benchmark provides the explanation.

Function-level tracing via ftrace:

```bash
# Trace the sustained stream ioctl path
echo function_graph > /sys/kernel/debug/tracing/current_tracer
echo dmaplane_sustained_stream > /sys/kernel/debug/tracing/set_graph_function
echo 1 > /sys/kernel/debug/tracing/tracing_on

# Run the benchmark
sudo ./tests/test_phase7_instrumentation

echo 0 > /sys/kernel/debug/tracing/tracing_on
cat /sys/kernel/debug/tracing/trace
```

The function graph output shows call depth, duration per function, and nesting. It reveals whether time is spent in `ib_post_send` (the verbs call), `ib_poll_cq` (the completion poll), or `cond_resched` (the scheduling yield). On rxe, the answer is usually `cond_resched` -- the kernel's voluntary preemption point accounts for the majority of wall-clock time in sustained streaming, because rxe processes work requests synchronously in the softirq and the benchmark yields between operations.

Combining both: `perf stat` shows 50,000 cache misses during a sustained run. ftrace shows that `ib_post_send` accounts for 60% of execution time. The conclusion: the cache misses are in the verbs path, likely in the SG table walk where `ib_post_send` reads the page array to build DMA descriptors. Hugepage allocation reduces the page count (and SG entries), which reduces cache misses in that specific path. Without both tools, this connection is invisible.

---

## Connection to Production

**The same instrumentation patterns appear at every scale in production AI infrastructure.** NCCL embeds internal profiling counters that track per-channel bandwidth, stall cycles, and protocol-level retries. The `mlx5_core` driver for ConnectX hardware exposes tracepoints under `mlx5:mlx5_*` that record every doorbell ring, CQE processing, and EQ interrupt. Amazon EFA's driver publishes per-device statistics via sysfs that mirror dmaplane's debugfs counters: sends posted, completions polled, stalls, errors.

The scale differs by orders of magnitude. NCCL's profiling runs across thousands of GPUs and aggregates per-rank statistics into a global timeline. `mlx5_core` traces at line rate -- 200 Gbps generating millions of CQEs per second, each potentially triggering a tracepoint. dmaplane runs on a single laptop over loopback at a fraction of that rate. But the structure is identical: static-branch tracepoints for per-event recording, atomic counters for aggregate statistics, histogram-based distribution tracking for tail latency analysis. When you read an `mlx5` tracepoint definition, you recognize the `TRACE_EVENT` macro, the `TP_fast_assign` block, and the `TP_printk` format string -- because you wrote the same thing for dmaplane.

The histogram pattern is particularly universal. Every production RDMA library maintains some form of latency bucketing. The exact implementation varies -- DPDK uses linear buckets for sub-microsecond precision, HFI1 uses log2 buckets like dmaplane, NCCL tracks per-channel min/max/avg without buckets -- but the purpose is the same: capture distribution shape in constant space during a run that generates millions or billions of samples.

---

## Connection Forward: GPU Memory Integration

**Seven phases complete the arc from building the data path to measuring and understanding it.** Phase 1 built the driver skeleton and ring buffers. Phase 2 added DMA allocation. Phase 3 placed buffers on the right NUMA node. Phase 4 moved data over RDMA. Phase 5 shared buffers across devices with zero copies. Phase 6 prevented the sender from overwhelming the receiver. Phase 7 made the entire path observable: tracepoints instrument every operation, debugfs exposes live state, histograms capture latency distributions, and cacheline alignment eliminates a class of invisible performance bugs.

Phase 8 extends the data path to GPU memory. `nvidia_p2p_get_pages` pins GPU VRAM pages via the BAR aperture. Write-combining mapping (`ioremap_wc`) transforms 44 MB/s uncacheable writes into 10 GB/s streaming writes. GPU-backed pages export through dma-buf (extending Phase 5's host-page exporter to handle 64 KB GPU pages) and register as RDMA Memory Regions (extending Phase 4's MR path). The result: GPU VRAM on one machine travels over RDMA to arrive at another machine, byte-for-byte verified, without touching host DRAM. Every instrumentation tool from Phase 7 -- tracepoints on the post/completion path, debugfs for MR state, histograms for GPU-backed transfer latency -- carries forward unchanged.

---

*Next: [Part 8 -- GPU Memory Integration](/docs/blog_08_gpu_integration.md)*
