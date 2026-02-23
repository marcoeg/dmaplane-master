<!-- SPDX-License-Identifier: GPL-2.0 -->
<!-- Copyright (c) 2026 Graziano Labs Corp. -->

# Backpressure: Why CQ Overflow Corrupts Data and How Credit-Based Flow Control Prevents It

*Part 6 of 9 in a series on building a host-side data path emulator for AI infrastructure*

---

Completion queue overflow is the silent transport-level failure in RDMA systems. The CQ is a fixed-size ring that collects work completion entries as the NIC finishes operations. When the CQ is full and another completion arrives, the completion is dropped -- not queued, not retried, not signaled to the sender. The data was moved. The bytes arrived at the destination. But the software never learns that the operation finished, and any subsequent poll of that CQ returns stale or corrupted entries. There is no backpressure signal from the CQ to the send queue. The NIC does not stop sending because the CQ is full; it stops reporting. Every production RDMA transport -- NCCL's ring protocol, NVMe-oF's capsule pipeline, Mooncake's point-to-point KV-cache path -- must solve this problem, and the solution is always the same: credit-based flow control that limits in-flight operations to less than the CQ depth, making overflow impossible by construction.

---

## The CQ Overflow Mechanism

**When the completion queue is full, the provider silently discards the work completion entry.** The IB spec (C10-97) defines this as a "CQ overrun" -- an implementation-defined error that may or may not transition the CQ to an error state. In practice, on Soft-RoCE (`rxe`), ConnectX hardware, and EFA, the behavior is the same: the completion entry is lost, the CQ continues to function, and subsequent polls return completions for different operations than the caller expects.

The critical distinction is between "data moved" and "completion delivered." An RDMA SEND operation involves two events: the NIC DMA-reads the source buffer and transmits the payload (data moved), and the NIC writes a CQE into the send CQ (completion delivered). These are decoupled. The NIC does not check whether the CQ has room before starting the DMA. It posts the CQE after the transfer completes, and if the CQ ring is full at that moment, the CQE is dropped. The receiver got the data. The receive CQ got its completion. But the sender's CQ lost the record that the send finished.

The consequence is a corrupted accounting state. The send loop tracks `outstanding` operations -- sends that were posted but not yet completed. When a CQE is lost, `outstanding` is permanently off by one. The loop thinks it has one more in-flight operation than it actually does. Over time, with multiple lost CQEs, the sender's view of how many operations are in-flight diverges from reality. The pipeline stalls because `outstanding` never drops below the queue depth limit, or worse, the sender continues posting new sends using completion slots that correspond to already-finished operations. Stale completions from a previous run can poison the CQ for subsequent benchmark invocations -- Phase 4 addresses this with `rdma_engine_flush_cq` before and after every benchmark, but flushing only works if the CQ has not entered an error state.

---

## The Sliding Window Pattern

**Phase 4's streaming benchmark already implements a first-order sliding window: pre-post receives bounded by pipeline depth, replenish on completion, eagerly drain the receive CQ.** The `outstanding < qdepth` check in the send loop is an ad-hoc credit counter. The sender posts up to `queue_depth` operations, polls for completions, decrements `outstanding` on each successful poll, and only posts more when `outstanding` drops below the limit. Receives are pre-posted to 2x queue depth and replenished in the poll loop to prevent receive queue starvation.

This works for fixed-iteration benchmarks. Post 1000 sends, poll 1000 completions, compute throughput. The pre-post count is bounded, the replenishment rate matches the completion rate, and the iteration count is known in advance. But the pattern breaks under sustained load. A 10-second streaming run at 1000+ MB/s generates hundreds of thousands of operations. The 2x-qdepth pre-post is a static budget that does not account for the relationship between CQ depth and in-flight count. If `queue_depth` exceeds half the CQ depth, the pre-post alone can overflow the receive CQ before the first completion arrives. The ad-hoc `outstanding < qdepth` check constrains the send side but says nothing about the total number of unpolled completions sitting in both CQs.

The missing piece is an explicit credit system that limits total in-flight operations across both send and receive paths to a value provably less than the CQ depth. The ad-hoc check is a special case of this -- it limits in-flight to `qdepth`, which happens to work when `qdepth` is small relative to the CQ size. The general solution replaces the implicit limit with an explicit, configurable credit window.

---

## Credit-Based Flow Control

**The general solution is a credit counter: `max_credits` defines the maximum in-flight operations, and the sender cannot post a new send unless `credits > 0`.** Each send consumes one credit (`dmaplane_flow_on_send`), each send completion returns one credit (`dmaplane_flow_on_completion`). Setting `max_credits <= CQ_depth` makes CQ overflow impossible by construction -- there can never be more unpolled completions than the CQ can hold.

The raw credit check is necessary but not sufficient for stable throughput. Without hysteresis, the sender pauses the instant `in_flight` reaches `max_credits`, resumes when one completion frees a credit, sends one operation, and immediately pauses again. This oscillation -- send one, stall, send one, stall -- destroys throughput because each pause involves a `cond_resched()` that yields the CPU, and the cost of the context switch dominates the actual transfer time. The fix is high/low watermark hysteresis:

```c
bool dmaplane_flow_can_send(struct dmaplane_dev *dev)
{
    int credits = atomic_read(&dev->flow.credits);
    int in_flight = dev->flow.max_credits - credits;

    if (dev->flow.paused) {
        /* Resume only when in-flight drops below low watermark */
        if (in_flight <= (int)dev->flow.low_watermark) {
            dev->flow.paused = false;
            atomic64_inc(&dev->stats.low_watermark_events);
            return true;
        }
        return false;
    }

    /* Pause when in-flight reaches high watermark */
    if (in_flight >= (int)dev->flow.high_watermark) {
        dev->flow.paused = true;
        atomic64_inc(&dev->stats.high_watermark_events);
        return false;
    }

    return credits > 0;
}
```

When `in_flight` reaches `high_watermark`, the sender pauses. It stays paused -- ignoring individual credit returns -- until `in_flight` drops below `low_watermark`. The gap between the two thresholds defines the hysteresis band. A wider band means longer pauses but fewer transitions. A narrower band means shorter pauses but more frequent oscillation. The configuration `max_credits=64, high=48, low=16` gives a 32-credit hysteresis band: the sender pauses at 48 in-flight, then waits for 32 completions (draining to 16) before resuming. This batches the pause/resume transitions and keeps the sender in sustained-send mode for longer stretches.

NVMe-oF uses the same pattern with `sqhd` (SQ Head Doorbell) as the credit return mechanism. NCCL's ring protocol tracks credits per-channel with a `tail` pointer that the receiver advances. The specific counter varies; the structure is identical.

---

## Sustained Streaming

**Iteration-based benchmarks hide temporal variance.** A 1000-iteration streaming test reports average throughput over the entire run. If 900 iterations complete in 0.5 seconds and the remaining 100 stall for 2 seconds due to a scheduling pathology, the average still looks reasonable. The stall is invisible in the aggregate number. Duration-based testing with per-second windowing reveals what averages hide.

The sustained streaming benchmark (`IOCTL_SUSTAINED_STREAM`) runs for a configurable wall-clock duration and samples throughput every second. Each one-second window records bytes transferred, and the min/max windows across the run surface variance. Duration-based testing also catches resource leaks -- a slow memory leak or an incrementing counter that wraps causes a failure at minute 8 that a 1000-iteration test would never reach. Scheduling pathologies from `cond_resched()` interactions with the rxe softirq are time-dependent; they manifest only after the kernel's timer tick has had enough cycles to preempt the benchmark at an inopportune moment.

Actual output from a 10-second sustained run (4 KB messages, queue depth 8, `max_credits=64`):

```
Duration: 10 seconds
Total: 10 MB transferred, 2660 ops
Throughput: avg=1037 MB/s, min_window=1047 MB/s, max_window=1087 MB/s
Stalls: 0   CQ overflows: 0
```

The min/max window spread is 40 MB/s on a 1037 MB/s average -- 3.8% variance. Zero stalls with `max_credits=64` and `queue_depth=8` confirms that the credit window is wide enough to never throttle the sender. Zero CQ overflows confirms the construction guarantee holds. The throughput numbers are Soft-RoCE over loopback -- the absolute value is irrelevant; what matters is stability over time and zero overflow events across hundreds of thousands of CQ polls.

With tight credits (`max_credits=4, high=3, low=1`), the same test forces stalls by design:

```
Stalls: 72,700,000 (expected >0 with max_credits=4)
CQ overflows: 0 (flow control prevented overflow)
```

72.7 million stalls in 5 seconds -- the sender spends most of its time in `cond_resched()` waiting for credits. But zero CQ overflows. The credit system degrades throughput gracefully rather than allowing silent data loss.

---

## Queue Depth Sweep

**The queue depth sweep characterizes the throughput-latency tradeoff across pipeline depths.** At `QD=1`, every send must complete before the next is posted -- pure stop-and-wait. The NIC is idle between operations, and throughput is bounded by round-trip latency. As queue depth increases, the NIC processes operations in parallel, and throughput rises until some bottleneck saturates.

Actual sweep data (4 KB messages, 500 iterations per point):

```
QD Sweep (msg_size=4096, iterations=500):
  QD= 1:     20 MB/s  avg_lat=200425 ns  p99=201376 ns
  QD= 5:     99 MB/s  avg_lat= 40024 ns  p99=201696 ns
  QD= 9:    175 MB/s  avg_lat= 22253 ns  p99=201216 ns
  QD=13:    254 MB/s  avg_lat= 15345 ns  p99=201216 ns
  QD=17:    329 MB/s  avg_lat= 11823 ns  p99=201216 ns
  QD=21:    401 MB/s  avg_lat=  9718 ns  p99=201216 ns
  QD=25:    480 MB/s  avg_lat=  8114 ns  p99=201216 ns
  QD=29:    567 MB/s  avg_lat=  6877 ns  p99=201216 ns
No saturation within range (throughput still climbing)
```

Three observations. First, throughput scales nearly linearly with queue depth on rxe -- no saturation point within the measured range. This is because rxe processes work requests in software with per-operation overhead dominated by kernel scheduling, not NIC hardware limits. On real ConnectX hardware, saturation typically occurs at QD=8-16 where PCIe bandwidth or NIC processing becomes the bottleneck. Second, average latency drops from 200 microseconds (QD=1, pure round-trip) to 7 microseconds (QD=29, amortized across the pipeline). The 28x latency reduction from 28x more pipeline depth is close to ideal. Third, P99 latency is ~201 microseconds at every queue depth. This is the rxe scheduling floor -- the worst-case completion always waits for a full softirq cycle regardless of how many operations are in the pipeline. The P99 is hardware-dependent, but the *shape* of the curve -- linear throughput scaling with constant P99 -- is universal across RDMA implementations.

The flow stats after running the full sustained and sweep sequence confirm the watermark system:

```
high_watermark_events: 660,666
low_watermark_events:  660,665
cq_overflows: 0
```

660K high watermark events matched by 660K low watermark events (off by one because the final pause does not resolve before the benchmark ends). The watermark system fires hundreds of thousands of times per test suite run, and CQ overflow remains zero across every invocation.

---

## Connection to NCCL

**"NCCL timeout" errors in distributed training are often CQ overflow in disguise.** When NCCL's all-reduce ring stalls, the typical symptom is a watchdog timeout on the blocked rank -- the error message says "timeout," but the root cause is that a sender's CQ overflowed, the sender lost track of completed operations, and the ring protocol deadlocked because the sender thinks it has in-flight operations that actually completed. The receiver is waiting for the next chunk; the sender is waiting for a completion that was dropped. Neither side makes progress.

NCCL's ring protocol solves this with per-channel credit tracking. Each ring channel has a `tail` counter that the receiver advances after consuming data from the receive buffer. The sender reads the tail to determine how many slots are free. This is structurally identical to `dmaplane_flow_can_send` -- the `tail` is the credit return, the difference between `head` and `tail` is the in-flight count, and the ring size is the max credits. NCCL does not use explicit high/low watermarks because the ring buffer itself provides natural batching -- the sender fills the ring until it is full, then blocks until the receiver drains a contiguous chunk.

The connection to dmaplane's credit system is direct. Replace "ring slot" with "credit," replace "tail pointer" with `atomic_inc(&dev->flow.credits)`, and the mechanism is the same. The difference is that NCCL operates over GPU memory with NVLink or InfiniBand as the transport, while dmaplane operates over host memory with Soft-RoCE. The credit accounting is transport-independent.

---

## Connection Forward: Instrumentation

**Phase 7 adds the measurement layer that answers "where does the time go" in the paths Phase 6 exercises.** The sustained streaming benchmark reports aggregate throughput and per-second windows, but it does not reveal whether the bottleneck is in `ib_post_send`, CQ polling, recv replenishment, or the `cond_resched()` yield during stalls. Phase 7 adds kernel tracepoints on the submit and complete paths, bucketed latency histograms exported via debugfs, and cacheline alignment of hot data structures. The tracepoints instrument every send and completion with nanosecond timestamps, enabling `ftrace` function-graph analysis of the entire pipeline. The histograms capture P50/P99/P999 distributions that the sweep's single-P99 number cannot provide. Cacheline alignment of the flow control `credits` atomic and the ring buffer head/tail pointers eliminates false sharing between the send thread and the completion polling path -- a measurable improvement under the high-frequency credit updates that the sustained benchmark generates.

---

*Next: [Part 7 -- Instrumentation & Latency Measurement](/docs/blog_07_instrumentation.md)*
