# Session: Init Command Profiling Harness

**Date:** 2026-03-31

## What Happened

1. User requested profiling infrastructure to measure init command performance
2. Otho implemented `--profile` CLI flag for both `index` and `init` subcommands
3. Designed 17-phase profiling report covering main-thread and worker-thread execution
4. Rationale: existing one-line profile output only tracked 5 worker phases; dominant bottleneck (persist) was unmeasured
5. Implemented `src/util/profiler.h` with `Profiler`, `PhaseTimer`, `ScopedPhase` RAII types
6. Wrapped all phases in cmd_index.cpp with `ScopedPhase` timers
7. Added persist timing to the always-on summary line
8. Build passed (0 errors, 0 warnings), all 179 tests pass (939 assertions)

## Key Findings

- **Persist dominates:** ~49% of wall time on production targets
- **Parallel efficiency poor:** Only ~7% on small repos (82 files), per-thread arena overhead significant
- **Profile data available:** Team can now see exact time spent per phase, guiding next optimization priorities
