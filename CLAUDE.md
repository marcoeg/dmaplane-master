# CLAUDE.md — dmaplane-master

## Project

dmaplane is a Linux kernel module for learning the host-side data path between AI frameworks and hardware. Character device at `/dev/dmaplane`, controlled via ioctl. Nine phases, built cumulatively.

## Current Phase

Phase 1 — Driver Foundations & Concurrency. See `docs/reference/MASTER_PLAN.md` Phase 1 for full spec and acceptance criteria.

## Key References

Read these before implementing anything:

- **`docs/reference/MASTER_PLAN.md`** — The 9-phase engineering plan. Contains phase specs, file maps, acceptance criteria, and skill KPIs. This is the authoritative source for what to build.
- **`docs/reference/CODE_OVERVIEW.md`** — Detailed analysis of every source file. Documents structs, ioctls, locking strategy, and design decisions. Updated each phase. Read this to understand the existing codebase.
- **`docs/reference/NARRATIVE_dmaplane_foundation.md`** — Technical essay describing the project's positioning, use cases, and relationship to production systems (Mooncake, TransferEngine, nvidia-peermem). Read this for context on *why* the code is shaped the way it is.

## Conventions

### Source files
- Every `.c` and `.h` file carries `SPDX-License-Identifier: GPL-2.0` and `Copyright (c) 2026 Graziano Labs Corp.`
- `MODULE_LICENSE("GPL v2")` in the kernel module

### Headers
- `driver/dmaplane.h` — kernel-internal header. Kernel-only types behind `#ifdef __KERNEL__`.
- `include/dmaplane_uapi.h` — userspace-visible API. Ioctl numbers, parameter structs, constants. Must be includable from both kernel and userspace. Keep in sync with `driver/dmaplane.h`.

### Logging
- Use `pr_fmt(fmt) KBUILD_MODNAME ": " fmt` at the top of every `.c` file for consistent `dmaplane:` prefix.
- Lifecycle messages (module load/unload, subsystem init/teardown): `pr_info`
- Per-operation messages (buffer create/destroy, submit/complete): `pr_debug`
- Warnings and errors: `pr_warn`, `pr_err`

### Locking
- Every lock must have a comment explaining what it protects and why it exists.
- Use `goto`-based cleanup in error paths with proper reverse-order resource release.

### Ioctl numbering
- Magic: `0xE4`
- Groups: buffer management `0x01`–`0x09`, RDMA `0x10`–`0x19`, MR `0x20`–`0x29`, benchmarks `0x30`–`0x39`, stats `0x40`–`0x49`, NUMA `0x50`–`0x59`, GPU `0x60`–`0x69`, peer QP `0x70`–`0x79`, WRITEIMM `0x80`–`0x89`
- Phase 1 uses `0x01`–`0x04` (create channel, submit, complete, get stats)

### Build
```bash
make                                    # Build driver + tests
sudo insmod driver/dmaplane.ko          # Load module
sudo ./tests/test_phase1_driver         # Run tests
sudo rmmod dmaplane                     # Unload module
dmesg | grep dmaplane                   # Check logs
```

### Testing discipline
- Every phase has a test in `tests/` that exercises the new functionality.
- Tests print PASS/FAIL per test case and a summary.
- After rmmod, check `dmesg` for lockdep warnings, WARN_ON, or leaked memory.

### Git discipline
- Tag each completed phase: `git tag phase-1-complete`
- Commit messages: `phase-N: <description>`
All development happens on main. Tag each completed phase as an immutable snapshot:
```bash
git tag -a phase-1-complete -m "Phase 1: Driver foundations & concurrency"
git push origin main --tags
```

## Directory Structure

```
dmaplane-master/
├── CLAUDE.md                  # This file
├── driver/                    # Kernel module (single dmaplane.ko)
├── include/                   # Userspace-visible headers
├── tests/                     # Per-phase tests
├── examples/                  # Progressive demos
│   ├── misc/                  # Standalone demos (Phase 2+)
│   ├── streamer/              # Weight-streaming TUI (Phase 6)
│   ├── netshare/              # Two-machine RDMA (Phase 4+)
│   └── inference/             # Disaggregated inference (Phase 9)
├── docs/                      # Blog posts (one per phase, output)
│   └── reference/             # Master plan, code overview, narrative (input)
└── scripts/                   # Build, setup, benchmark automation
```

## Phase Index

| Phase | Status | Topic |
|-------|--------|-------|
| 1 | **CURRENT** | Driver foundations & concurrency |
| 2 | Planned | DMA memory allocation |
| 3 | Planned | NUMA topology & placement |
| 4 | Planned | RDMA engine |
| 5 | Planned | dma-buf & zero-copy sharing |
| 6 | Planned | Backpressure & flow control |
| 7 | Planned | Instrumentation & latency measurement |
| 8 | Planned | GPU memory integration |
| 9 | Planned | Disaggregated inference demo |

Update the "Status" column and "Current Phase" section as phases are completed.
