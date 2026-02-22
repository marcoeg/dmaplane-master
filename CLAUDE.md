# CLAUDE.md — dmaplane-master

## Project

dmaplane is a Linux kernel module for learning the host-side data path between AI frameworks and hardware. Character device at `/dev/dmaplane`, controlled via ioctl. Nine phases, built cumulatively.

## Current Phase

Phase 4 — RDMA Integration — **COMPLETE**. See `docs/reference/MASTER_PLAN.md` for the next phase spec.

## Key References

Read these before implementing anything:

- **`docs/reference/MASTER_PLAN.md`** — The 9-phase engineering plan. Contains phase specs, file maps, acceptance criteria, and skill KPIs. This is the authoritative source for what to build.
- **`docs/reference/CODE_OVERVIEW.md`** — Detailed analysis of every source file. Documents structs, ioctls, locking strategy, and design decisions. Updated each phase. Read this to understand the existing codebase.
- **`docs/reference/NARRATIVE_dmaplane_foundation.md`** — Technical essay describing the project's positioning, use cases, and relationship to production systems (Mooncake, TransferEngine, nvidia-peermem). Read this for context on *why* the code is shaped the way it is.

## Quality Gate — Reference Check

Before implementing, read these files from the original dmaplane project 
(available in docs/reference/) and verify that this prompt accounts for 
every pattern and design decision relevant to this phase:

1. docs/reference/CODE_OVERVIEW_original.md — the original project's full 
   code analysis. Search for keywords relevant to this phase.
2. The original project's source files for the equivalent functionality 
   (if available in docs/reference/original_source/).

If this prompt contradicts or omits something from the original that seems 
important, flag it before implementing.

## Conventions

### Source files
- Every `.c` and `.h` file carries `SPDX-License-Identifier: GPL-2.0` and `Copyright (c) 2026 Graziano Labs Corp.`
- `MODULE_LICENSE("GPL v2")` in the kernel module

### Headers
- `include/dmaplane_uapi.h` — userspace-visible API (primary definition point). Ioctl numbers, parameter structs, constants. Must be includable from both kernel and userspace.
- `driver/dmaplane.h` — kernel-internal header. Includes `dmaplane_uapi.h` and adds kernel-only types behind `#ifdef __KERNEL__`.

### Logging
- Use `pr_fmt(fmt) KBUILD_MODNAME ": " fmt` at the top of every `.c` file for consistent `dmaplane:` prefix.
- Lifecycle messages (module load/unload, subsystem init/teardown): `pr_info`
- Per-operation messages (buffer create/destroy, submit/complete): `pr_debug`
- Warnings and errors: `pr_warn`, `pr_err`

### Code comments
- Every `.c` and `.h` file gets a header block: what it does, where it fits, key design decisions
- Every struct field gets an inline comment
- Every lock declaration gets a block comment: what it protects, who acquires it, why this lock type, ordering constraints
- Every ioctl handler gets a block comment: what it does, concurrency model, error returns
- Non-obvious lines get inline "why" comments (memory barriers, skip-zero-on-wrap, DMA API subtleties)
- UAPI structs annotate fields `/* in */`, `/* out */`, or `/* in/out */`
- Standard: a kernel engineer should understand the code without reading any external documentation

### Locking
- Every lock must have a comment explaining what it protects and why it exists.
- Use `goto`-based cleanup in error paths with proper reverse-order resource release.

### Ioctl numbering
- Magic: `0xE4`
- Groups: buffer management `0x01`–`0x09`, RDMA `0x10`–`0x19`, MR `0x20`–`0x29`, benchmarks `0x30`–`0x39`, stats `0x40`–`0x49`, NUMA `0x50`–`0x59`, GPU `0x60`–`0x69`, peer QP `0x70`–`0x79`, WRITEIMM `0x80`–`0x89`
- Phase 1 uses `0x01`–`0x04` (create channel, submit, complete, get stats)

### Build system
- `driver/Makefile` uses `$(CURDIR)` (not `$(PWD)`) for the Kbuild `M=` path. `$(PWD)` inherits from the parent make invocation and resolves to the wrong directory when called via `make -C driver`.

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
| 1 | **COMPLETE** | Driver foundations & concurrency |
| 2 | **COMPLETE** | DMA memory allocation |
| 3 | **COMPLETE** | dma-buf export & zero-copy sharing |
| 4 | **COMPLETE** | RDMA engine |
| 5 | Planned | dma-buf & zero-copy sharing |
| 6 | Planned | Backpressure & flow control |
| 7 | Planned | Instrumentation & latency measurement |
| 8 | Planned | GPU memory integration |
| 9 | Planned | Disaggregated inference demo |

Update the "Status" column and "Current Phase" section as phases are completed.
