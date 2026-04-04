# Session: Partial Profiling Infrastructure

**Date:** 2026-03-31

## What Happened

1. User requested fast-iteration profiling infrastructure for DsMainDev repo (104K files)
2. Otho added `--max-files N` CLI flag to index and init commands (plumbed through Config, Scanner, main.cpp, cmd_init.h, supervisor.cpp)
3. Created `profile_subset.bat` with 6 presets (tiny/small/fsm/medium/large/full) for rapid profiling on subsets
4. Key insight: `--root` to subdirectory (no `.git`) is essential for large git repos; `git ls-files` still scans entire repo (72s overhead)
5. Profiled FSM baseline (4145 files, 39.4s):
   - file_read dominates at 82% of worker time
   - persist still limits main thread at 26% wall time
   - parallel efficiency: 64%
6. Profiled tiny preset (500 files, 10.4s):
   - persist: 46%, scan: 44%
   - workers capable of 741 f/s but persist caps at ~104 f/s
7. Build passes (0 errors, 0 warnings), all 179 tests pass (939 assertions)

## Key Findings

- **Storage bottleneck:** File read at 82% of worker time on DsMainDev suggests network/storage latency (shared network paths common in enterprise)
- **Persist remains wall:** Main thread persist is still the throughput bottleneck; persist tail-latency hasn't regressed
- **Fast iteration enabled:** tiny and small presets now allow sub-20s benchmarking for rapid hypothesis testing
- **Monorepo pattern:** Large git repos need `--root` on subdirectory, not just `--max-files`, because git ls-files scans the entire worktree regardless of file limit
- **Parallel efficiency:** 64% on baseline (4145 files) is best we've achieved; 46% on tiny suggests scan/startup overhead dominates at small scale
