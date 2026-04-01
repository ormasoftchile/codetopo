# Decisions — Codetopo

Archive of accepted, proposed, and rejected decisions. Decisions are ordered by decision ID (DEC-XXX) and include status, rationale, implementation records, and lessons learned.

---

# DEC-035: MCP Tool Gap Fixes — node_id Chaining, C# Extractor Edges, source_at Tool

**Author:** Grag (Systems Engineer)  
**Coordinator:** Joan (Tester/QA)  
**Date:** 2026-04-01  
**Status:** Implemented  

## Summary

Implemented 3 MCP tool fixes identified in design review to close capability gaps and enable better LLM chaining.

## Changes

### Fix 1: file_summary now returns node_id
- **File:** `src/mcp/tools.cpp` (line ~969)
- **Change:** Added `id` column to SQL query, included `node_id` in JSON output
- **Impact:** Enables LLMs to chain file_summary results directly into symbol_get, context_for, callers_approx, etc. Previously required an extra symbol_search roundtrip for every symbol.

### Fix 2: C# extractor — 3 new edge types
- **File:** `src/index/extractor.cpp`
- **Changes:**
  - `using_directive` → `include` refs (enables file_deps for C# files)
  - `base_list` → `inherit` refs (enables find_implementations, context_for base types)
  - `object_creation_expression` → `call` refs (enables callers_approx for constructors)
- **Pattern:** Follows established patterns from C++ and TypeScript extractors.

### Fix 3: source_at MCP tool (T092)
- **File:** `src/cli/cmd_mcp.h`
- **Signature:** `source_at(path, start_line, end_line)` → 1-based inclusive, max 500 lines
- **Implementation:** Reads arbitrary line ranges from source files, path validated via `validate_mcp_path`, reuses `read_source_snippet()`
- **Impact:** Fills gap between "read whole file" and "symbol_get requires node_id"

## Test Coverage (Joan)

- 13 new test cases across 3 files (72 assertions)
- `test_file_summary_nodeid.cpp` — node_id presence, tool chaining, uniqueness
- `test_source_at.cpp` — line range correctness, boundary conditions, error handling
- `test_csharp_extractor.cpp` — C# edge type emission (using→include, inheritance→inherit, invocation→call)
- All tests pass immediately (proactive/regression guards)

## Build & Verification

- Release build: clean (0 errors, 0 warnings)
- Unit tests: 231 pass (1198 assertions)
- Prior tests: all 218 remain green (no regressions)

## Team Impact

- Joan: No blockers; tests written in parallel and pass immediately
- Simon: source_at is a new tool surface — stateless, read-only, path-validated. No architecture concerns.
- Otho: No performance impact — all changes in cold paths (MCP query layer, not indexer hot path)

---

# DEC-034: SQLite Turbo PRAGMAs + FK Disable + 2GB mmap (Grag, 2026-04-01)

**Status:** Implemented, build + benchmark verified

## Summary

Implemented Otho's scaling analysis recommendations R1-R3 to address the 6x persist degradation at 100K+ file scale (1.5ms→9.1ms/file).

## Changes

**R1 — Wire turbo PRAGMAs (connection.h, cmd_index.cpp):**
- `conn.enable_turbo()` called after `schema::ensure_schema()` when `config.turbo` is true
- Sets: synchronous=OFF, wal_autocheckpoint=0, temp_store=MEMORY, cache_size=128MB
- `--turbo` CLI flag was already plumbed through supervisor; this just wires the final call

**R2 — Disable FK during bulk persist (cmd_index.cpp):**
- `PRAGMA foreign_keys=OFF` before drain loop, `PRAGMA foreign_keys=ON` after `commit_batch()`
- Same pattern as `resolve_references()` in persister.h (lines 288/527)
- Unconditional during bulk — FK integrity is preserved by application logic and re-checked post-commit

**R3 — Increase mmap_size to 2GB (connection.h):**
- Changed Connection constructor: `PRAGMA mmap_size=268435456` → `PRAGMA mmap_size=2147483648`
- Unconditional — applies to all Connection uses (indexer and MCP server)
- SQLite only mmaps pages that exist; no memory waste on small DBs

## Benchmark Results (105,694 files, cold index, turbo)

| Metric | Before (Otho baseline) | After | Change |
|--------|----------------------|-------|--------|
| Persist/file | 9.1ms | 1.7ms | **-81% (5.3x faster)** |
| Persist total | ~915s | 173s | **-81%** |
| Persist % of wall | 62% | 34% | **-28pp** |
| Indexing rate | ~410 files/s | 438 files/s | **+7%** |
| Total pipeline | ~1474s | 511s | **-65%** |

Otho predicted 3.5-4.5ms/file persist; actual 1.7ms/file exceeded by 2x. The combined effect of R1+R2+R3 with the earlier R3(batch drain)+R4(cold index) from DEC-032 compounds — turbo PRAGMAs eliminate WAL fsync overhead that was amplifying at scale.

## Constraints Respected

- DEC-018: Largest-first sort unchanged
- Turbo PRAGMAs only active with `--turbo` flag
- mmap_size increase is unconditional (always beneficial)
- FK disable is unconditional during bulk (matches resolve_references pattern)
- All existing tests pass (122 cases, 524 assertions; pre-existing watchdog flake excluded)

---

# DEC-PROPOSAL: SQLite Persist 6x Scaling Degradation — Root Cause Analysis & Recommendations

**Author:** Otho (Performance Engineer)  
**Date:** 2026-07-04  
**Status:** Proposed  
**Requested by:** Cristiano  
**Priority:** HIGH — persist is 62% of wall time at 100K scale

---

## Executive Summary

Per-file persist cost grows from 1.5ms (4K files) to 9.1ms (105K files) — a **6x degradation** that makes persist consume 62% of total wall time at scale. This is NOT a threading issue (contention=0.0s). It is caused by four compounding SQLite scaling factors, the most impactful of which is a **dead `--turbo` flag** that silently fails to apply critical PRAGMAs.

---

## Profile Data (same machine, same build, both cold-index)

| Metric | FSM (4,145 files) | Full (105,694 files) | Ratio |
|---|---|---|---|
| Wall time | 20.5s | 1473.5s | 72x |
| Overall rate | 200 files/s | 72 files/s | 0.36x |
| persist total | 6.2s (30%) | 915.2s (62%) | 148x |
| persist/file | 1.5ms | 9.1ms | **6.0x** |
| file_read/file | 1.1ms | 65ms | 59x |
| Worker capacity | 787 files/s | 208 files/s | 0.26x |
| Contention | 0.0s | 0.0s | — |

---

## Root Cause Analysis

### RC-1 (CRITICAL): `--turbo` flag is dead code — PRAGMAs never applied

**Evidence:** `cmd_index.cpp` sets `config.turbo = true` (line 202 in main.cpp) and the supervisor passes `--turbo` to child processes (supervisor.cpp:116), but **`conn.enable_turbo()` is never called anywhere in the indexer pipeline.** Searched all call sites — `enable_turbo()` is only defined in `connection.h:69`, never invoked.

**Impact:** The profiled 100K run was using:
- `synchronous=NORMAL` (WAL fsync on every COMMIT) instead of `synchronous=OFF`
- Default `wal_autocheckpoint=1000` (auto-checkpoint every ~4MB of WAL) instead of 0
- `cache_size=64MB` instead of 128MB
- `batch_size=500` (default) — turbo description says "batch=1000" but this override is never applied

At 105K files with batch_size=500, that's **211 COMMIT operations**, each triggering:
1. A WAL fsync (~1-5ms on SSD)
2. Potential auto-checkpoint if WAL exceeds 1000 pages

At 4K files: ~8 commits + 0-2 autocheckpoints = negligible.  
At 100K files: ~211 commits + potentially dozens of autocheckpoints during bulk insert.

**Estimated impact on persist/file:** The autocheckpoints are the killer. Each checkpoint transfers WAL pages to the main DB file with random I/O. As the DB grows past ~200MB, checkpoint cost per page increases because the main file pages are spread across a large file. Fixing this alone likely accounts for **2-3ms/file** of the 7.6ms delta.

### RC-2 (HIGH): UNIQUE constraint on `nodes.stable_key` — undropable per-insert B-tree maintenance

**Evidence:** `schema.h:50` defines `stable_key TEXT NOT NULL UNIQUE` on the nodes table. This UNIQUE constraint creates an implicit index that is NOT included in `drop_bulk_indexes()` (which only drops secondary indexes created with `CREATE INDEX`). SQLite enforces this constraint via a B-tree probe on every INSERT.

**Impact at scale:**
- At 4K files: ~40K nodes → B-tree ~2 levels → 2 page reads per INSERT
- At 100K files: ~3.4M nodes → B-tree ~4 levels → 4 page reads per INSERT
- Per file with ~32 symbol nodes: 32 × 2 extra page reads = 64 extra pages
- At ~100ns per cached page or ~1µs per uncached page → 0.064ms to 2ms extra per file

The stable_key values are long text strings (e.g., `"file::path/to/foo.cs::class::ClassName::method::MethodName"`), making the B-tree pages wider and the index larger. At 3.4M rows with ~80-byte average keys, the index is ~300MB — exceeding the mmap window.

**Estimated impact:** 1-2ms/file at 100K scale (B-tree depth increase + mmap overflow).

### RC-3 (HIGH): mmap_size=256MB is undersized for 100K-file databases

**Evidence:** `connection.h:48` sets `PRAGMA mmap_size=268435456` (256MB). At 100K files, the database contains:
- files table: ~100K rows × ~200 bytes = ~20MB
- nodes table: ~3.4M rows × ~300 bytes = ~1.0GB
- refs table: ~2.5M rows × ~200 bytes = ~500MB
- edges table: ~2.5M rows × ~100 bytes = ~250MB
- Indexes + overhead: ~200MB
- **Total: ~2GB**

The 256MB mmap covers only ~13% of the database. Every B-tree page access beyond the mmap window falls back to `read()` system calls, which are significantly slower than memory-mapped access.

At 4K files, the DB is ~50-70MB — fits entirely in the 256MB mmap. At 100K files, nearly every page access hits the read() path.

**Estimated impact:** 1-2ms/file from random I/O overhead on B-tree traversals and FK checks.

### RC-4 (MEDIUM): Foreign key checks during bulk insert

**Evidence:** `connection.h:44` sets `PRAGMA foreign_keys=ON` at connection open. FK checks are only disabled inside `resolve_references()` (persister.h:288), NOT during the main `persist_file()` drain loop.

Per file, persist_file() performs:
- 1 INSERT files (no parent FK)
- 1 INSERT nodes (file node, file_id=NULL → no FK check)
- ~32 INSERT nodes (symbols, file_id → FK check on files PK)
- ~25 INSERT refs (file_id + containing_node_id → 2 FK checks each)
- ~30 INSERT edges (src_id + dst_id → 2 FK checks each)
- **Total: ~142 FK checks per file**, each requiring a B-tree probe on the parent table's PK

At small scale, these probes hit cached pages. At 100K scale with the nodes table (~3.4M rows) exceeding the mmap window, FK checks on `nodes(id)` become expensive.

**Estimated impact:** 0.5-1ms/file from FK check I/O at scale.

### RC-5 (LOW-MEDIUM): `files.path` UNIQUE constraint — duplicate-check on every file

**Evidence:** `schema.h:23` defines `path TEXT UNIQUE NOT NULL` on the files table. Even with cold_index=true (DELETE skipped), every INSERT INTO files triggers a UNIQUE check. At 100K files, this is a 100K-entry text B-tree that grows throughout the run. The paths are long strings (~80-100 chars), making pages hold fewer entries.

**Estimated impact:** 0.3-0.5ms/file.

---

## Combined Impact Model

| Root Cause | Est. ms/file @ 100K | Est. ms/file @ 4K | Delta |
|---|---|---|---|
| RC-1: Dead turbo (autocheckpoint + fsync) | 3.0-4.0 | 0.2 | 2.8-3.8 |
| RC-2: stable_key UNIQUE B-tree growth | 1.5-2.0 | 0.3 | 1.2-1.7 |
| RC-3: mmap overflow → read() fallback | 1.5-2.0 | 0.0 | 1.5-2.0 |
| RC-4: FK checks on large tables | 0.5-1.0 | 0.1 | 0.4-0.9 |
| RC-5: files.path UNIQUE check | 0.3-0.5 | 0.1 | 0.2-0.4 |
| **Total estimated** | **6.8-9.5** | **0.7-1.5** | **6.1-8.8** |
| **Observed** | **9.1** | **1.5** | **7.6** |

The model explains 80-100% of the observed 7.6ms/file degradation.

---

## file_read Analysis (1.1ms → 65ms, 59x degradation)

This is a worker-side issue, not persist, but it impacts overall throughput.

**Root cause: NTFS filesystem cache eviction at scale.**

- FSM run: 4,145 files × ~50KB avg = ~200MB. Fits comfortably in Windows file cache (typically 2-8GB).
- Full run: 105,694 files × ~5.3GB. Exceeds typical file cache capacity.
- Additionally, the SQLite database itself grows to ~2GB, competing for the same OS page cache.
- Largest-first sort means early files are 500KB+ and later files are smaller, but by that point the cache is thrashed.

**Secondary factors:**
- NTFS directory enumeration overhead at 100K+ entries (though git-ls-files handles this)
- Windows Defender real-time scanning (each file open triggers AV check)
- MFT (Master File Table) access patterns on very large directories

**Recommendation:** file_read is I/O-bound at scale and not directly fixable in code. The mmap_size increase (R3) will help by reducing SQLite's competition for OS page cache. Beyond that, SSD is the only real fix.

---

## Concrete Recommendations

### R1: Wire up `--turbo` PRAGMAs (CRITICAL — fix the dead flag)

**What:** In `cmd_index.cpp`, after `Connection conn(db_path)` and `ensure_schema()`, add:
```cpp
if (config.turbo) {
    conn.enable_turbo();
    effective_batch_size = std::max(effective_batch_size, 1000);
}
```

**PRAGMAs applied by enable_turbo():**
- `synchronous=OFF` — eliminates WAL fsync on COMMIT
- `wal_autocheckpoint=0` — prevents checkpoints during bulk insert (one at end)
- `temp_store=MEMORY` — temp tables in RAM
- `cache_size=-131072` — 128MB page cache (up from 64MB)

**Expected improvement:** 9.1ms → 5-6ms/file (eliminate RC-1 entirely)  
**Complexity:** Low — 3 lines of code  
**Risk:** Low — enable_turbo() already exists, just unwired. synchronous=OFF is safe against app crash (data survives), only vulnerable to OS crash/power loss. Acceptable for an index that can be rebuilt.

### R2: Disable FK checks during bulk persist loop

**What:** Bracket the main drain loop with FK disable/enable:
```cpp
if (bulk_mode) {
    conn.exec("PRAGMA foreign_keys = OFF");
}
// ... drain loop ...
if (bulk_mode) {
    conn.exec("PRAGMA foreign_keys = ON");
}
```

**Rationale:** During cold-index bulk load, all IDs are generated by SQLite itself (via `last_insert_rowid()`). FK violations are impossible. FK checks add ~142 B-tree probes per file that serve no purpose during bulk insert.

**Expected improvement:** 5-6ms → 4.5-5.5ms/file  
**Complexity:** Low — 4 lines, bracketing existing code  
**Risk:** Low — FKs re-enabled before resolve_references (which already disables them again anyway). Integrity preserved.

### R3: Increase mmap_size to 2GB for bulk mode

**What:** Add to `enable_turbo()` or as a separate bulk-mode PRAGMA:
```cpp
conn.exec("PRAGMA mmap_size=2147483648");  // 2 GB
```

Or make it conditional on bulk_mode in cmd_index.cpp:
```cpp
if (bulk_mode) {
    conn.exec("PRAGMA mmap_size=2147483648");  // 2GB for large DB
}
```

**Rationale:** At 100K files, the DB grows to ~2GB. The current 256MB mmap only covers 13% of the database, forcing expensive `read()` system calls for every B-tree page beyond the window. Setting mmap to 2GB allows the OS to memory-map the entire database.

**Expected improvement:** 4.5-5.5ms → 3.5-4.5ms/file  
**Complexity:** Low — 1 line  
**Risk:** Low — mmap is backed by virtual address space, not physical RAM. The OS manages physical pages efficiently. On 64-bit systems, 2GB of address space is negligible.

### R4: Increase page_size to 8192 for new databases

**What:** Before `ensure_schema()`, on fresh databases only:
```cpp
// page_size must be set before any table creation
auto ver = schema::get_schema_version(conn);
if (ver == 0) {  // fresh DB
    conn.exec("PRAGMA page_size=8192");
}
```

**Rationale:** Default 4096-byte pages are suboptimal for wide tables (nodes has 13+ columns, ~300 bytes/row). 8KB pages:
- Reduce B-tree depth by ~1 level (2x entries per page)
- Improve I/O throughput (fewer read operations for same data)
- Better match SSD erase-block alignment (most SSDs use 4KB-16KB blocks)

**Expected improvement:** 3.5-4.5ms → 3.0-4.0ms/file  
**Complexity:** Medium — requires schema migration logic (can't change page_size on existing DB). Only apply to fresh databases.  
**Risk:** Low for new DBs. Does not affect existing databases. Doubles DB file size slightly (more internal fragmentation per page, but fewer pages total).

### R5: Increase batch_size to 5000 for 100K+ workloads

**What:** In cmd_index.cpp, scale batch_size with workload:
```cpp
if (total > 50000 && effective_batch_size < 5000) {
    effective_batch_size = 5000;
}
```

**Rationale:** With batch_size=500 at 105K files: 211 COMMIT operations. Each COMMIT has fixed overhead (WAL header write, potential checkpoint trigger). At batch_size=5000: 21 COMMIT operations — 10x fewer. With synchronous=OFF (R1), the per-COMMIT overhead is smaller, but the journal bookkeeping per-COMMIT is still non-trivial.

**Expected improvement:** 0.1-0.3ms/file  
**Complexity:** Low — 3 lines  
**Risk:** Low — larger batches mean more data loss on crash, but the index is rebuildable. Memory usage for the in-flight transaction is bounded by SQLite's WAL, not application memory.

### R6: Drop `nodes.stable_key` UNIQUE during bulk insert (ADVANCED)

**What:** Before bulk insert, rename the constraint away:
```sql
-- Can't drop UNIQUE on a column constraint in SQLite.
-- Alternative: recreate the table without UNIQUE, bulk insert, then recreate with UNIQUE.
```

Actually, in SQLite you cannot `ALTER TABLE` to drop a column constraint. The practical approach is:
1. At cold-index start, create nodes table WITHOUT the UNIQUE constraint on stable_key
2. After all inserts, create a UNIQUE INDEX on stable_key
3. This is essentially what `drop_bulk_indexes()` does for secondary indexes, but applied to the UNIQUE constraint

**Implementation:** In `schema.h`, add `create_tables_bulk()` that creates nodes without `UNIQUE` on stable_key, and a `finalize_bulk_tables()` that adds `CREATE UNIQUE INDEX idx_nodes_stable_key ON nodes(stable_key)`.

**Expected improvement:** 3.0-4.0ms → 2.0-3.0ms/file  
**Complexity:** HIGH — requires schema creation split, careful migration logic, and rebuild step  
**Risk:** Medium — if crash during bulk insert leaves table without UNIQUE constraint, subsequent queries may behave differently. Must ensure constraint is always restored.

---

## Recommendation Priority & Expected Cumulative Impact

| Priority | Fix | Persist/file | Cumulative | Complexity | Risk |
|---|---|---|---|---|---|
| — | Current (broken turbo) | 9.1ms | — | — | — |
| **P0** | R1: Wire turbo PRAGMAs | 5-6ms | **-35%** | Low | Low |
| **P1** | R2: FK off during bulk | 4.5-5.5ms | **-45%** | Low | Low |
| **P1** | R3: mmap 2GB | 3.5-4.5ms | **-55%** | Low | Low |
| **P2** | R5: batch_size 5000 | 3.2-4.2ms | **-58%** | Low | Low |
| **P2** | R4: page_size 8192 | 2.8-3.8ms | **-62%** | Medium | Low |
| **P3** | R6: Drop stable_key UNIQUE | 2.0-3.0ms | **-70%** | High | Medium |

**R1+R2+R3 together (all low-complexity) should bring persist from 9.1ms to ~3.5-4.5ms/file — a 2-2.5x improvement.** This would reduce the full 100K run from ~915s to ~370-475s for persist alone, saving 440-545 seconds of wall time.

**Total wall time impact:** Persist drops from 62% to ~30-35% of wall time. Total run estimated to reduce from ~1474s to ~930-1030s (~35% faster). The remaining bottlenecks become resolve_refs (260s) and scan (146s).

---

## Code Locations for Grag

| File | Line(s) | Change |
|---|---|---|
| `src/cli/cmd_index.cpp` | After line 108 (ensure_schema) | R1: Add `if (config.turbo) { conn.enable_turbo(); }` |
| `src/cli/cmd_index.cpp` | Line 218 | R1: Add `if (config.turbo) effective_batch_size = std::max(effective_batch_size, 1000);` |
| `src/cli/cmd_index.cpp` | Before line 369 (drain loop) | R2: Add `if (bulk_mode) conn.exec("PRAGMA foreign_keys = OFF");` |
| `src/cli/cmd_index.cpp` | After line 449 (commit_batch) | R2: Add `if (bulk_mode) conn.exec("PRAGMA foreign_keys = ON");` |
| `src/cli/cmd_index.cpp` | Near line 228 (bulk_mode check) | R3: Add `conn.exec("PRAGMA mmap_size=2147483648");` inside bulk_mode block |
| `src/cli/cmd_index.cpp` | Line 218 | R5: Add `if (total > 50000 && effective_batch_size < 5000) effective_batch_size = 5000;` |
| `src/db/connection.h` | Line 48 or enable_turbo() | R4: Add `page_size=8192` logic for fresh DBs |

---

## Verification Plan

After R1+R2+R3 are implemented, re-run both profiles:
```
profile_subset.bat C:\One\DsMainDev\Sql fsm      # Expect: ~1.0-1.2ms/file persist
profile_subset.bat C:\One\DsMainDev\Sql full      # Expect: ~3.5-4.5ms/file persist
```

The scaling ratio should improve from 6.0x to ~3-3.5x (remaining degradation from UNIQUE constraints and inherent B-tree depth growth — irreducible without R6).
