# Orchestration Log: Partial Profiling Infrastructure

**Date:** 2026-03-31 22:10  
**Trigger:** User requested fast-iteration profiling for DsMainDev (104K file repo)

## Agent Spawned

| Agent | Role | Assignment | Status |
|-------|------|------------|--------|
| Otho | Performance Engineer | Add --max-files flag, create profiling scripts, profile FSM subset | ✅ SUCCESS |

## Work Delivered

**Scope:** Enable rapid profiling iteration on large repos by adding CLI flags and batch profiling script.

**Implementation:**
1. **`--max-files N` flag** on `index` and `init` subcommands
   - Plumbed through Config → Scanner → main.cpp, cmd_init.h, supervisor.cpp
   - Truncates file list to N files after scanning (0 = unlimited)
   
2. **`profile_subset.bat`** with 6 presets for DsMainDev/Sql:
   - tiny: 500 files, ~10s
   - small: 2000 files, ~15s
   - fsm: 4145 files, ~39s (established baseline, C#, uniform)
   - medium: 10K files, ~60s
   - large: 50K files, ~5m
   - full: all 104K files
   
3. **Profiling methodology:**
   - Use `--root` to subdirectory (no `.git`) to avoid 72s git ls-files overhead
   - Delete DB before each run for clean cold-index benchmarks
   - Measure via existing `--profile` flag from prior session

**Files Modified:**
- `src/core/config.h` — added max_files field
- `src/index/scanner.h` — added truncation logic
- `src/main.cpp` — added --max-files CLI option
- `src/cli/cmd_init.h` — updated signature
- `src/index/supervisor.cpp` — arg passthrough
- `profile_subset.bat` — new profiling script

## Validation Results

**Build:** 0 errors, 0 warnings  
**Tests:** 179 tests pass (939 assertions)

**Benchmark: FSM Subset (4145 C# files, 39.4s total)**
- file_read: 82% of worker time
- parse: 13%
- extract: 5%
- Main thread persist: 26% of wall time
- Main thread contention: 32%
- Parallel efficiency: 64%
- Sustained throughput: ~164 files/s

**Benchmark: Tiny Subset (500 files, 10.4s total)**
- persist: 46% of wall time
- scan: 44%
- Workers capable of 741 f/s but persist caps at ~104 f/s

## Key Insights

- **Storage bottleneck:** 82% of worker time on file_read suggests network/storage constraint on DsMainDev (shared network paths)
- **Persist remains limiting:** Even with fast workers, main thread persist is the wall. At scale (162K files), estimated 5-10+ minutes per full index run
- **Fast iteration enabled:** tiny and small presets now allow sub-20s profiling cycles for rapid optimization testing
- **Monorepo pattern:** For large git repos, `--root` on subdirectories is faster than `--max-files` alone (git ls-files scans entire tree)

## Decisions Recorded

- DEC-030: `--max-files` flag and partial profiling infrastructure (accepted)

## Next Steps

- Use preset profiles to test optimization hypotheses rapidly
- File read dominance on DsMainDev warrants investigation (network caching, storage latency)
- Persist remains critical path for next optimization cycle
