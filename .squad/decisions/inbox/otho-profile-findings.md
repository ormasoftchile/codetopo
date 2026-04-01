# DEC-034: Post-DEC-032 Profile — Parse+Extract Phase Analysis

**Author:** Otho (Performance Engineer)  
**Date:** 2026-04-01  
**Status:** Actionable findings with ranked recommendations

## Context

After DEC-032 (batch drain + cold index skip, +76% throughput), workers (parse+extract) were expected to be the new bottleneck. This profiling session instruments the full pipeline to determine where time is actually going.

## Methodology

Added `--profile` flag and per-phase `ScopedPhase` instrumentation to `cmd_index.cpp`. Profiler (already in `profiler.h`) measures 12 main-thread phases and 5 worker-thread phases. Also added `--max-files` and `--extract-timeout` CLI flags, and fixed two build issues (arena.cpp redefinition, missing Config fields).

## Results — 4 data points at 512KB max-file-size, 16 threads

| Subset | Files | Wall | Rate | Persist | Contention | Worker Ideal |
|--------|-------|------|------|---------|------------|--------------|
| tiny   | 500   | 3.5s | 141/s | 2.6s (73%) 5.2ms/f | 0.1s (3%) | 0.5s |
| small  | 2000  | 9.7s | 206/s | 4.9s (50%) 2.4ms/f | 0.3s (3%) | 2.8s |
| fsm    | 4145  | 22.5s| 185/s | 6.7s (30%) 1.6ms/f | 7.9s (35%) | 5.3s |
| fsm@1MB| 4145  | 34.0s| 122/s | 10.6s (31%) 3ms/f | 7.4s (22%) | 16.8s |

### Worker Thread Phase Breakdown (fsm, 512KB)

| Phase | Thread Time | % Thread | Avg/file |
|-------|-------------|----------|----------|
| arena_lease | 0.0s | 0.0% | 1μs |
| file_read | 21.9s | 26.0% | 5ms |
| hash | 0.0s | 0.0% | 8μs |
| **parse** | **38.5s** | **45.7%** | **9ms** |
| **extract** | **23.8s** | **28.2%** | **6ms** |

### Worker Thread Phase Breakdown (fsm, 1024KB)

| Phase | Thread Time | % Thread | Avg/file |
|-------|-------------|----------|----------|
| **file_read** | **192.7s** | **71.5%** | **47ms** |
| parse | 52.4s | 19.5% | 13ms |
| extract | 24.3s | 9.0% | 6ms |

## Key Findings

### Finding 1: **Persist is STILL the #1 bottleneck** (not workers)

Contrary to expectations from DEC-032, persist remains the pipeline-constraining phase at ALL subset sizes. Workers can produce 720-909 files/s but persist caps throughput at 194-617 files/s. The profiler explicitly identifies this: "Workers can produce 788 files/s but persist caps at 617 files/s."

The DEC-032 improvement was real (3.3→1.6ms/file), but workers were *already* faster. The gap just became more visible.

### Finding 2: **Contention (7.9s, 35% wall) is bursty arrival**

The largest-first sort causes all 16 workers to process similarly-sized files simultaneously. They finish in waves → burst of results → persist burst → wait for next wave. Main thread alternates between idle (contention) and busy (persist). At fsm: 6.7s persist + 7.9s contention = 14.6s out of 16s indexing (91%).

### Finding 3: **file_read explodes at 1024KB max-file-size**

- 512KB: 5ms/file (26% of worker time)
- 1024KB: 47ms/file (71.5% of worker time) — **9.4x increase**

Large files read slowly AND sort to the front → all 16 workers hit disk simultaneously → I/O contention. The max-file-size default of 512KB (DEC-029) is critical for performance.

### Finding 4: **Parse dominates worker time at 512KB (46-70%)**

At 512KB, parse is 9-16ms/file. This is tree-sitter's irreducible parsing cost and has limited optimization potential. Not a practical optimization target.

### Finding 5: **Extract is well-controlled at 3-6ms/file**

DEC-029's extraction timeout is working correctly. Extract is 20-28% of worker time — healthy balance.

### Finding 6: **ThreadPool still creates 64MB stacks**

`cmd_index.cpp:582`: `ThreadPool worker_pool(window_size, 64 * 1024 * 1024)` — 18 threads × 64MB = 1.15GB committed stack memory. DEC-021 documented the fix (→ 8MB) but it was never applied.

### Finding 7: **WAL checkpoint is 18% of wall at 1024KB**

6.1s for checkpoint at 1024KB vs 1.3s at 512KB. Proportional to data volume.

### Finding 8: **Per-file persist cost scales with symbol count**

500 largest files: 5.2ms/file persist. All 4145 files: 1.6ms/file persist. The largest files have more symbols → more INSERT operations per persist_file call.

## Ranked Recommendations

### R1 (HIGH, one-line fix): Fix ThreadPool stack size 64→8MB
DEC-021 documented this but it wasn't applied. Saves 1GB+ of committed physical RAM. Zero risk.
```cpp
ThreadPool worker_pool(window_size, 8 * 1024 * 1024);
```

### R2 (HIGH, architectural): Pipelined persist thread
Move persist to a dedicated writer thread. Main thread dequeues results and passes to writer via lock-free queue. Decouples result consumption from SQLite writes. Expected: eliminates the 35% contention, approaches worker-limited throughput (788 files/s theoretical, ~400-500 realistic with SQLite overhead).

### R3 (MEDIUM, already available): Parallel WAL persist
DEC-026 R7. Use WAL mode's concurrent-read + separate-writer capability. More complex than R2 but higher ceiling. Only needed if R2 still leaves persist as bottleneck.

### R4 (LOW, diminishing returns): Memory-mapped file reading
At 512KB, file_read is 5ms/file (26% of worker time) but only 1.4s of wall time. Wall impact is modest. Would matter more at 1024KB (12s of wall).

### R5 (LOW): Defer WAL checkpoint in turbo mode
At 1024KB, WAL checkpoint takes 6.1s (18% wall). Could defer to background or skip entirely in turbo mode. Small gain at 512KB (1.3s).

## Artifacts

- `--profile`, `--max-files`, `--extract-timeout` CLI flags added and wired through supervisor
- `Profiler` instrumentation wired into `cmd_index.cpp` (12 main-thread + 5 worker-thread phases)
- `TreeGuard` move assignment operator added (needed for instrumentation scoping)
- arena.cpp duplicate function definitions fixed (conflict with DEC-022 inline arena functions)
- `Config::max_files`, `Config::extraction_timeout_s`, `Config::profile` fields added
- All 173 tests pass (914 assertions)
