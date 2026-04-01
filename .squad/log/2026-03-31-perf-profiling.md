# Session Log: Performance Profiling & Regression Investigation

**Date:** 2026-03-31  
**Focus:** Parse throughput regression (700→213 files/s) root cause and mitigation

## Timeline

### Parse Timeout Wired Up (DEC-023)
Grag discovered that `Parser::set_timeout()` and `Extractor(timeout_s)` existed but were never called in the cmd_index.cpp worker lambda. Without timeout, pathological files block forever; combined with sequential `futures[i].get()`, this causes head-of-line blocking and apparent hangs. Two-line fix applied.

### Deferred Index Rebuild (DEC-014, DEC-015)
Resolve phase optimized: read-path indexes built before resolver, write-path indexes deferred until after. FK checks disabled during resolve. SQL join in Step 6 replaced with in-memory edge collection. Result: **776s resolve time, down from 1474s** (~47% reduction). Edge creation reduced from dominant cost center to single-digit seconds.

### Result Queue Replacing futures[i].get()
Slot system reverted to simple futures + lightweight watchdog (DEC-017, DEC-024). Eliminates head-of-line blocking where a slow file at position `i` blocks consumption of all completed files at positions `i+1, i+2, ...`. Workers never starve — entire worklist queued upfront. 21% code reduction (690→545 lines).

### Contention Measurement (DEC-025)
Atomic contention counters added. Total contention across full run: **1.6s** — confirms thread synchronization is NOT the bottleneck. Arena pool, thread pool, and result queue mutexes all have microsecond-scope locks. The regression is per-file processing cost, not coordination overhead.

### Per-Phase Profiling Added (DEC-028)
Instrumented cmd_index.cpp with per-phase atomic timers: arena lease, file read, content hash, tree-sitter parse, symbol extraction.

**Results on 4145 C# files:**

| Scenario | Files/s | Extract time | Parse time | Contention |
|----------|---------|-------------|------------|------------|
| max-file-size 256KB | **829** | 7.4s (1.8ms/f) | 26.9s (6.6ms/f) | 0.0s |
| max-file-size 1024KB | **45** | 98.7s (23.9ms/f) | 39.9s (9.7ms/f) | 83.7s |

### Key Finding
Normal files process at **829 files/s** — above the 700 baseline. The regression is entirely **tail-latency from the extractor on large-AST files**. Files >256KB cause a 13x extraction blowup. The extractor has no timeout (parse has `--parse-timeout` but extraction runs unbounded). ~0.1% pathological files with 10K+ AST nodes take 10-15s each, blocking worker threads and stalling the pipeline.

## Decisions Merged
DEC-017 through DEC-028 merged from inbox (12 decisions covering slot revert, performance analysis, fixes, contention measurement, profiling results, and edges constraint proposal).

## Next Steps
- R1: Add extraction timeout (highest impact — expected 2-3x throughput gain)
- R2: Consider lowering default max-file-size to 512KB
- R4: Log slow files for diagnostics
- DEC-026: Add UNIQUE constraint to edges table (deferred with write-path indexes)
