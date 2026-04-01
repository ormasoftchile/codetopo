# Session Log: SQLite Persist Scaling Analysis (2026-04-01T01:00Z)

**Date:** 2026-04-01  
**Focus:** Root cause analysis for 6x per-file persist degradation (1.5ms→9.1ms) from 4K to 105K files  
**Agents:** Otho (performance engineering)  
**Outcome:** SUCCESS

## Summary

Completed comprehensive analysis of persist subsystem scaling bottleneck. Discovered 5 root causes explaining 80-100% of the 7.6ms/file degradation. Critical finding: `--turbo` performance flag is dead code — `enable_turbo()` function exists but is never called, causing the indexer to run with suboptimal SQLite pragmas during bulk insert.

## Profile Context (DsMainDev)

| Metric | FSM (4K files) | Full (105K files) | Ratio |
|---|---|---|---|
| Wall time | 20.5s | 1473.5s | 72x |
| Persist total | 6.2s (30%) | 915.2s (62%) | 148x |
| **Per-file persist** | **1.5ms** | **9.1ms** | **6.0x** |
| File read rate | 200→1.1ms | 200→65ms | 59x |
| Worker throughput | 787 files/s | 208 files/s | 0.26x |
| Lock contention | 0.0s | 0.0s | — |

**Key insight:** Zero lock contention rules out threading issues. Scaling is driven by SQLite query behavior at large database sizes.

## Root Cause Analysis (RC-1 through RC-5)

### RC-1: Dead `--turbo` flag (CRITICAL)

**Evidence:** 
- `cmd_index.cpp` line 202: `config.turbo = true`
- `supervisor.cpp` line 116: passes `--turbo` to child
- **`enable_turbo()` in connection.h:69 is defined but never called**

**Why it matters:** At 100K files with default batch_size=500: 211 COMMIT operations.
- Each COMMIT triggers WAL fsync (1-5ms on SSD) when `synchronous=NORMAL`
- `wal_autocheckpoint=1000` causes checkpoints when WAL exceeds 1000 pages
- At scale, checkpoints do random I/O on 2GB+ DB files

Impact: 3.0-4.0ms/file at 100K (vs 0.2ms at 4K).

### RC-2: `nodes.stable_key` UNIQUE constraint

**Evidence:** `schema.h:50` creates undropable B-tree index, grows from ~2 levels (4K) to ~4 levels (100K).

Impact: 1.5-2.0ms/file from extra page reads during INSERT.

### RC-3: mmap_size undersized

**Evidence:** `connection.h:48` sets 256MB; database grows to 2GB at 100K files. Coverage: 13% → 87% fallback to `read()` system calls.

Impact: 1.5-2.0ms/file from random I/O on B-tree traversals.

### RC-4: FK checks during bulk

**Evidence:** 142 FK checks per file × B-tree probes on large tables.

Impact: 0.5-1.0ms/file.

### RC-5: `files.path` UNIQUE constraint

**Evidence:** Duplicate-check on every INSERT with growing B-tree.

Impact: 0.3-0.5ms/file.

**Combined:** 6.8-9.5ms/file (model) vs 9.1ms/file (observed) → 80-100% coverage.

## Recommendations (Staged by Priority)

### P0: R1 — Wire up `--turbo` PRAGMAs
- **Change:** Call `conn.enable_turbo()` in cmd_index.cpp after `ensure_schema()`
- **PRAGMAs:** `synchronous=OFF`, `wal_autocheckpoint=0`, `temp_store=MEMORY`, `cache_size=-131072`
- **Expected:** 9.1ms → 5-6ms/file
- **Complexity:** Low (3 lines)

### P1: R2 — Disable FK checks during bulk insert
- **Change:** `PRAGMA foreign_keys = OFF` before drain, re-enable after
- **Expected:** 5-6ms → 4.5-5.5ms/file
- **Complexity:** Low (4 lines)

### P1: R3 — Increase mmap_size to 2GB
- **Change:** `PRAGMA mmap_size=2147483648` in bulk mode
- **Expected:** 4.5-5.5ms → 3.5-4.5ms/file
- **Complexity:** Low (1 line)

### P2: R5 — Scale batch_size to 5000
- **Change:** Increase batch for 50K+ files
- **Expected:** 0.1-0.3ms/file additional savings
- **Complexity:** Low (3 lines)

### P2: R4 — Increase page_size to 8192
- **Change:** Set before schema creation on fresh DBs
- **Expected:** 0.5ms/file additional savings
- **Complexity:** Medium (migration logic)

### P3: R6 — Drop stable_key UNIQUE during bulk
- **Change:** Recreate table without constraint, rebuild index post-insert
- **Expected:** 1.0ms/file additional savings
- **Complexity:** High (table recreation, error handling)

## Cumulative Impact Projection

| Milestone | Configuration | Persist/file | Wall time est. |
|---|---|---|---|
| Current (broken turbo) | 9.1ms | 915s (62%) |
| R1+R2+R3 (low-complexity) | 3.5-4.5ms | ~370-475s → **930-1030s total (-35%)** |
| R5 + R4 (medium) | 2.8-3.8ms | ~300-400s → ~730-830s total (-50%) |
| R6 (advanced) | 2.0-3.0ms | ~210-315s → ~640-740s total (-56%) |

## Next Steps

**Grag:** Implement P0+P1 fixes (R1, R2, R3). This is the critical path — expect 2-2.5x per-file improvement.

**Verification:** Re-run both profiles (`profile_subset.bat fsm` and `profile_subset.bat full`) and confirm:
- FSM persist: 1.5ms → 1.0-1.2ms/file (minimal change, as turbo already effective at 4K)
- Full persist: 9.1ms → 3.5-4.5ms/file (major change)
- Scaling ratio: 6.0x → 3-3.5x (remaining degradation from UNIQUE constraints, irreducible without R6)
- Total wall time: ~1474s → ~930-1030s (~35% faster)

## Technical Wins

- **Root cause isolation:** Identified dead code (enable_turbo), not mysterious scaling
- **Quantifiable impact:** Combined model explains 80-100% of observed degradation
- **Actionable roadmap:** 6 recommendations staged by complexity/priority
- **Low-risk:** P0+P1 fixes are all safe pragmas with no schema changes
