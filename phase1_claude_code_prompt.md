# Claude Code Prompt — dmaplane Phase 1: Driver Foundations & Concurrency

## Context

You are building Phase 1 of a 9-phase Linux kernel module project called **dmaplane**. The repo is `dmaplane-master` and it is currently empty. You will create the full directory skeleton for all 9 phases (with placeholder READMEs) and then implement Phase 1 completely.

dmaplane is an educational project for learning the kernel subsystems that sit between AI frameworks (PyTorch, NCCL) and the hardware (GPUs, NICs, DRAM). It is a loadable Linux kernel module that exposes a character device (`/dev/dmaplane`) controlled via ioctl. The project builds cumulatively — each phase adds to the repo without replacing previous work.

The development machine is:
- Ubuntu with kernel 6.5.0-1024-oem
- 32 CPUs, single NUMA node, 128 GB DRAM
- NVIDIA RTX 5000 Ada (16 GB GDDR6) — not used in Phase 1
- Soft-RoCE (rxe) available — not used in Phase 1

## What to Build

### 1. Repository skeleton

Create the full directory structure below. Directories that will be populated in later phases get a one-line `README.md` placeholder saying which phase populates them.

Three reference documents are provided alongside this prompt. Copy them into the repo exactly as specified:
- **CLAUDE.md** → repo root (Claude Code reads this automatically on every session)
- **MASTER_PLAN.md** → `docs/reference/MASTER_PLAN.md`
- **CODE_OVERVIEW.md** → `docs/reference/CODE_OVERVIEW.md`
- **NARRATIVE_dmaplane_foundation.md** → `docs/reference/NARRATIVE_dmaplane_foundation.md` (provided separately — the Medium blog describing project positioning and use cases)

```
dmaplane-master/
├── CLAUDE.md                       # Claude Code entry point (PROVIDED — copy as-is)
├── README.md                       # Project overview (write this — see below)
├── LICENSE                         # GPL-2.0 (standard text)
├── Makefile                        # Top-level: delegates to driver/Makefile
│
├── driver/                         # Kernel module source
│   ├── README.md                   # Driver architecture doc (Phase 1 content now)
│   ├── Makefile                    # Kbuild Makefile
│   ├── dmaplane.h                  # Shared header: ioctls, structs, constants
│   └── main.c                      # Character device, ioctl dispatch, module init/exit
│
├── include/                        # Userspace-visible headers
│   └── dmaplane_uapi.h            # Ioctl numbers, shared structs (kernel ↔ user)
│
├── tests/                          # One test per phase
│   ├── README.md                   # Test inventory
│   ├── Makefile                    # Builds all userspace test programs
│   └── test_phase1_driver.c        # Phase 1 stress test
│
├── examples/                       # Placeholder for later phases
│   ├── README.md
│   ├── misc/README.md
│   ├── streamer/README.md
│   ├── netshare/README.md
│   └── inference/README.md
│
├── docs/                           # Blog posts (one per phase, written as output)
│   ├── README.md                   # Index of blog posts (planned and completed)
│   └── reference/                  # Input documents for Claude Code
│       ├── MASTER_PLAN.md          # 9-phase engineering plan (PROVIDED — copy as-is)
│       ├── CODE_OVERVIEW.md        # Codebase analysis, updated each phase (PROVIDED)
│       └── NARRATIVE_dmaplane_foundation.md  # Medium blog: positioning & use cases (PROVIDED)
│
└── scripts/                        # Build and setup automation
    └── README.md                   # Placeholder
```

**docs/reference/ convention**: These files are *input* to Claude Code — they describe what to build and why. Blog posts in `docs/` are *output* — they document what was built and what was learned. The reference directory is not for end users; it's for the developer and for Claude Code context.

### 2. Phase 1 implementation

The character device driver with submission/completion rings and per-channel worker threads.

#### driver/dmaplane.h

The internal kernel header. Define:

- Module name constant: `"dmaplane"`
- Max channels: 4
- Ring size: 1024 entries (power of 2)
- Ring entry struct: at minimum a `u64 payload` and `u32 flags`
- Ring buffer struct: array of entries, `head` and `tail` indices, a spinlock. Head and tail should be on separate cache lines (`____cacheline_aligned_in_smp`) to avoid false sharing between producer and consumer.
- Channel struct: submission ring, completion ring, a `struct task_struct *worker` for the kthread, a `wait_queue_head_t` for the worker to sleep on, channel ID, an `atomic_t` for tracking in-flight submissions, and a `bool` for shutdown signaling.
- Device context struct: array of channels, `struct cdev`, `struct class *`, `dev_t`, `struct device *`, a `struct mutex` for device-level operations.
- File context struct: pointer to device context, pointer to the assigned channel (one channel per open fd).
- Ioctl command numbers using `_IOWR`/`_IOW`/`_IOR` macros:
  - `IOCTL_CREATE_CHANNEL` — allocate a channel and assign it to this fd
  - `IOCTL_SUBMIT` — push a work item onto the submission ring
  - `IOCTL_COMPLETE` — pop a completion from the completion ring
  - `IOCTL_GET_STATS` — return channel statistics (submissions, completions, ring occupancy)
- Stats struct: total submissions, total completions, ring high watermark, dropped count.

#### include/dmaplane_uapi.h

The userspace-visible subset of the above. Ioctl numbers, parameter structs, and any constants that userspace needs. This file must be includable from both kernel (`__KERNEL__`) and userspace.

#### driver/main.c

The module implementation. This is the bulk of Phase 1.

**Module init/exit:**
- `alloc_chrdev_region` for dynamic major number
- `class_create` + `device_create` for `/dev/dmaplane` udev node
- `cdev_init` + `cdev_add`
- Clean teardown in reverse order on exit and on init failure

**File operations:**
- `open`: allocate a `file_context`, store in `filp->private_data`. Do NOT create a channel yet — that happens via ioctl.
- `release`: if a channel was assigned, signal its worker to shut down, `kthread_stop` the worker, free the channel, free the file context. Must handle the case where the process exits without explicitly releasing.
- `unlocked_ioctl`: dispatch to handlers based on command number.

**IOCTL_CREATE_CHANNEL handler:**
- Find a free channel slot (protected by device mutex).
- Initialize submission and completion rings (zero head/tail, init spinlock).
- Create a kthread (`kthread_create`) for this channel. Name it `"dmaplane/%d"` where `%d` is the channel ID.
- Wake up the kthread.
- Store the channel pointer in the file context.
- Return channel ID to userspace.

**IOCTL_SUBMIT handler:**
- Copy the submission entry from userspace (`copy_from_user`).
- Acquire the submission ring spinlock.
- Check if the ring is full (head has caught up to tail). If full, return `-ENOSPC`.
- Write the entry at `ring[head % RING_SIZE]`.
- Advance head (use `smp_store_release` for the head update so the worker sees it).
- Release spinlock.
- Wake up the channel's worker thread (`wake_up_interruptible`).
- Increment stats.

**IOCTL_COMPLETE handler:**
- Acquire the completion ring spinlock.
- Check if the ring is empty. If empty, return `-EAGAIN` (non-blocking) or optionally sleep (but start with non-blocking).
- Read the entry at `ring[tail % RING_SIZE]`.
- Advance tail.
- Release spinlock.
- Copy result to userspace (`copy_to_user`).

**IOCTL_GET_STATS handler:**
- Copy the channel's stats struct to userspace.

**Worker thread function:**
- Loop until `kthread_should_stop()`.
- Wait on the channel's wait queue for: submission ring not empty OR kthread_should_stop.
- When woken: drain submissions from the submission ring.
- For each submission: "process" it (in Phase 1, this just means copying the payload to a completion entry, possibly with a transformation like incrementing the payload by 1 to prove the worker touched it).
- Push each result onto the completion ring (acquire completion spinlock, check full, write, advance head, release).
- Use `cond_resched()` periodically to avoid hogging the CPU.

**Concurrency discipline:**
- Submission ring: spinlock protects head (userspace writer) and is also used by worker (reader via tail). The worker reads tail, userspace writes head.
- Completion ring: spinlock protects head (worker writer) and tail (userspace reader).
- Device-level channel allocation: mutex (sleeping context OK for ioctl).
- Worker shutdown: set a flag, wake the worker, then `kthread_stop` (which waits for the thread to exit).
- Every lock must have a clear comment explaining what it protects and why it exists.

#### driver/Makefile

Standard Kbuild makefile:

```makefile
obj-m := dmaplane.o
dmaplane-objs := main.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
```

Note: `dmaplane-objs` will grow in future phases (e.g., `dmabuf_rdma.o`, `numa_topology.o`, etc.). Start with just `main.o`.

#### Top-level Makefile

```makefile
.PHONY: all clean driver tests

all: driver tests

driver:
	$(MAKE) -C driver

tests:
	$(MAKE) -C tests

clean:
	$(MAKE) -C driver clean
	$(MAKE) -C tests clean
```

#### tests/test_phase1_driver.c

A userspace C program that exercises the driver thoroughly.

**Test 1: Basic open/close**
- Open `/dev/dmaplane`, close it. No crash.

**Test 2: Channel creation**
- Open fd, ioctl `CREATE_CHANNEL`, verify returns channel ID 0.
- Open second fd, create channel, verify returns channel ID 1.

**Test 3: Submit and complete**
- Create channel.
- Submit an entry with a known payload (e.g., `0xDEADBEEF`).
- Small sleep or poll loop to let the worker process it.
- Complete: read the completion, verify the payload was transformed (e.g., `0xDEADBEEF + 1` if the worker increments).

**Test 4: Ring full behavior**
- Submit `RING_SIZE` entries rapidly without completing any.
- The `RING_SIZE`-th submit should return `-ENOSPC` (or `RING_SIZE - 1`-th, depending on full detection).
- Complete all, verify all payloads correct.

**Test 5: Stress test**
- Single channel: submit and complete 1M entries in a tight loop (submit batch, complete batch, repeat).
- Verify total completions == total submissions.
- Report throughput (submissions/sec).

**Test 6: Multi-channel stress**
- Open 4 fds, create 4 channels.
- Fork or use pthreads: each thread submits 250K entries to its channel.
- Join all threads.
- Complete all entries on all channels.
- Verify total == 1M, no cross-channel contamination (each channel's completions match its submissions).

**Test 7: Cleanup on close**
- Create channel, submit entries, close the fd without completing.
- Verify: no kernel warnings in `dmesg`, no leaked memory, module can be unloaded cleanly.

The test should print PASS/FAIL for each test and a summary. Use `ioctl()` directly — no library dependencies beyond libc.

#### tests/Makefile

```makefile
CC := gcc
CFLAGS := -Wall -Wextra -O2 -I../include

TESTS := test_phase1_driver

all: $(TESTS)

test_phase1_driver: test_phase1_driver.c
	$(CC) $(CFLAGS) -o $@ $< -lpthread

clean:
	rm -f $(TESTS)
```

### 3. README.md (top-level)

Write a project README that includes:
- Project name and one-paragraph description (educational kernel module for learning AI infrastructure data path)
- Phase index (list all 9 phases with one-line descriptions, mark Phase 1 as current, rest as planned)
- Build instructions: `make`, `sudo insmod driver/dmaplane.ko`, `sudo ./tests/test_phase1_driver`, `sudo rmmod dmaplane`
- Directory structure overview
- Pointer to `docs/reference/MASTER_PLAN.md` for the full engineering plan
- Pointer to `docs/reference/NARRATIVE_dmaplane_foundation.md` for project positioning and use cases
- License: GPL-2.0

### 4. driver/README.md

Write a driver README that covers:
- Architecture: character device, ioctl-driven, per-channel rings + workers
- Ioctl reference table (command, direction, struct, description)
- Concurrency model: which locks protect what
- How to load/unload the module

## Constraints

- Target kernel: 6.5.x (Ubuntu stock). Use APIs available in this version.
- No external dependencies. No libibverbs, no CUDA, no rdma-core. Just kernel headers.
- All code must compile with `-Wall -Werror` (kernel side) and `-Wall -Wextra` (userspace).
- Use `pr_info`, `pr_warn`, `pr_err` for kernel logging with the `"dmaplane: "` prefix (use `pr_fmt`).
- Include GPL-2.0 MODULE_LICENSE.
- Comment every lock explaining what it protects.
- The ring buffer full/empty detection must be correct. A common pattern: the ring is full when `(head - tail) == RING_SIZE` and empty when `head == tail`, using unsigned arithmetic. Both head and tail only increment (never wrap — the modulo is applied only when indexing into the array).

## What NOT to Build

- No DMA allocation (Phase 2)
- No NUMA awareness (Phase 3)
- No RDMA (Phase 4)
- No dma-buf (Phase 5)
- No GPU support (Phase 8)
- No network examples (Phase 4+)

The worker thread's "processing" is trivial (copy payload, increment by 1). The infrastructure — rings, locking, kthreads, ioctl dispatch, lifecycle management — is the point.

## Verification

After implementation, verify:

```bash
# Build
cd dmaplane-master && make

# Load
sudo insmod driver/dmaplane.ko
ls -la /dev/dmaplane          # should exist
dmesg | tail -5                # should show init message

# Test
sudo ./tests/test_phase1_driver
# All tests should print PASS

# Unload
sudo rmmod dmaplane
dmesg | tail -5                # should show exit message, no warnings

# Reload and re-test (verify clean lifecycle)
sudo insmod driver/dmaplane.ko
sudo ./tests/test_phase1_driver
sudo rmmod dmaplane
```

Check `dmesg` for any lockdep warnings, WARN_ON, or leaked memory messages after rmmod.

## Post-Implementation Tasks

### Update CODE_OVERVIEW.md

After the code compiles and tests pass, review `docs/reference/CODE_OVERVIEW.md`. It was written as a spec before implementation. Update it to reflect what was *actually* built — correct any struct names, field names, ioctl numbers, or design details that changed during implementation. The CODE_OVERVIEW must accurately describe the code as it exists, not as it was planned.

### Write the blog post

Write `docs/blog_01_driver_foundations.md`. This is the first in a 9-part series.

**Structure and tone**: Technical blog post written in first person. Not a tutorial — it's a narrative of building the driver and what was learned. Each section should have a concrete code example or measurement. The reader should come away understanding *why* the code is shaped the way it is, not just what it does.

**Content to cover:**
- Why a character device and ioctl (not sysfs, not netlink, not read/write)
- The ring buffer design: monotonic indices, unsigned arithmetic, why full/empty detection is subtler than it looks
- Locking discipline: what each lock protects, the decision between spinlock and mutex, what lockdep catches
- Worker threads: kthread lifecycle, wait queues, shutdown protocol, what happens if the process exits without cleaning up
- What surprised you: at least one concrete thing that was harder or more subtle than expected
- Connection to AI infrastructure: why this scaffolding matters for the DMA/RDMA/GPU phases ahead

**Length**: 1500–2500 words. Code snippets should be real (from the implementation), not pseudocode.

### Update CLAUDE.md

If any conventions changed during implementation (ioctl numbering, struct naming, etc.), update `CLAUDE.md` to reflect the actual state.
