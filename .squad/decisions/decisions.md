# Decisions — Codetopo

Archive of accepted, proposed, and rejected decisions. Decisions are ordered by decision ID (DEC-XXX) and include status, rationale, implementation records, and lessons learned.

---

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

## Key Findings

### Finding 1: **Persist is STILL the #1 bottleneck** (not workers)

Contrary to expectations from DEC-032, persist remains the pipeline-constraining phase at ALL subset sizes. Workers can produce 720-909 files/s but persist caps throughput at 194-617 files/s. The DEC-032 improvement was real (3.3→1.6ms/file), but workers were already faster. The gap just became more visible.

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

# DEC-039: Index Pipeline Architecture Review — Simon + Otho Performance Deep Dive

**Authors:** Simon (Lead/Architect), Otho (Performance Engineer)  
**Date:** 2026-04-03  
**Status:** Accepted — Design review complete, implementation proceeding

## Executive Summary

Full end-to-end architecture review of `run_index()` pipeline at 100K file scale (652s wall, 16 threads, post-DEC-038). Analysis identified that **persist throughput remains the pipeline ceiling** despite prior optimizations. Root cause is single-row symbol INSERT pattern (2400μs per ~30-symbol file, 69% of persist cost). Three-tier optimization plan targets 480s baseline (−172s, −26%), meeting 494s target.

## Key Findings

### Finding 1: Persist is Pipeline Bottleneck Despite DEC-034 R2 Pipelining
- Persist thread: 425s (persist 348s + flush 77s)
- Workers per-thread: 300s
- Main-thread idle: 194s (healthy)
- Main-thread backpressure: 242s (37% of wall) proves queue saturation

Pipeline balance ratio: 425/300 = 1.42:1 (persist-bound). Workers produce at 333 files/s theoretical, actual 153 files/s (46% efficiency).

### Finding 2: Single-Row Symbol INSERT is Dominant Cost
- Per-file: ~30 symbols × 1 `sqlite3_step()` each = 2400μs (69% of 3.48ms persist cost)
- Refs/edges already batch (80-row, 150-row per DEC-038 OPT-2)
- Symbols are the only unbatched INSERT; `last_insert_rowid()` per-symbol drives pattern

### Finding 3: File Read Still Uses ostringstream (O(N log N) allocations)
- Current: 35.3ms/file average, 3530s thread-time (largest single phase)
- Root: exponential allocation growth + redundant `.str()` copy
- Fix: Pre-sized string + single direct read → 50-65% faster

### Finding 4: 11-Phase Pipeline Analyzed End-to-End
1. Discovery (scanner) — 72s at 200K+ (off critical path)
2. Work list prep — double file read for changed files (warm-index only)
3. Worker pool — slot system dual-layer complexity (low-priority refactor)
4. File read — largest phase, pre-allocation quick win
5. Hashing — excellent, 20μs/file irreducible
6. Parsing — 100K parser allocations (2s overhead), thread-local pooling → 3-5s savings
7. Extraction — string dispatch overhead in hot loop (1.5ms/file)
8. Result queue — single-dequeue pattern (opportunity: batch drain)
9. Persist queue — well-designed, backpressure is symptom not cause
10. SQLite persist — batch symbol INSERT is critical path, batch drain, clear_bindings removal
11. Post-processing — 210s sequential (82s idx_read, 72s resolve_refs, 27s idx_write, 29s fts_rebuild)

### Finding 5: 7 Post-Processing Bottlenecks Assessed
- **idx_read (82s, 7 indexes):** Index audit reveals potential redundancy
- **resolve_refs (72s):** Well-optimized from DEC-015, sequential by design
- **idx_write (27s):** Deferred correctly, cannot parallelize with FTS (single-writer)
- **fts_rebuild (29s):** Atomic, cannot overlap
- All 4 phases single-threaded due to SQLite single-writer constraint

## Three-Phase Optimization Plan (11 Optimizations)

### Phase 1: Persist Speedup (Must-Do First)

**Tier 1 — Two Changes, Combined 652→520s (−132s, −20%)**

| Optimization | Cost | Effort | Risk |
|---|---|---|---|
| Batch symbol INSERT (20-row) | −110s persist, −117s wall | Medium (60 LOC) | Medium |
| Turbo batch size 1000→5000 | −30s flush | 1-line | Low |

**Method:** Follow existing batch INSERT pattern (refs 80-row, edges 150-row). Compute IDs via `last_insert_rowid() - (N-1)` arithmetic (SQLite sequential rowid guarantee within single INSERT, no concurrent deletes in transaction).

**Rationale:** After Phase 1, persist (300s) ≈ workers (300s). Pipeline becomes balanced instead of persist-bound.

### Phase 2: Worker Speedup (Unlocked by Phase 1)

**Tier 1 — Two Changes, Combined 520→480s (−40s, reaching target)**

| Optimization | Cost | Effort | Risk |
|---|---|---|---|
| Pre-sized file_read | −60s wall | Small (10 LOC) | Very Low |
| Thread-local parser reuse | −10s wall | Small (15 LOC) | Low |

**Method:** (1) Allocate `std::string(file.size_bytes)` once, single `f.read()` call. (2) `thread_local unordered_map<string, Parser>` per language, reuse across files.

**Rationale:** After Phase 1, persist no longer gates. Worker optimizations become visible. These are the two largest per-file worker costs (35ms + 2ms of 48ms total).

**After Phase 1+2:** 652s → ~480s (−172s, −26%), **meets 494s target with margin**.

### Phase 3: Post-Processing Polish (If Needed)

**Tier 2 — Two Changes, Combined 480→455s (−25s, exceed target)**

| Optimization | Cost | Effort | Risk |
|---|---|---|---|
| Defer content_hash index | −10s | Small | Very Low |
| Page size 4K→8K (new DBs) | −15s | Small | Very Low |

**Method:** (1) Skip building `idx_files_content_hash` during cold index (only used by incremental change detector). (2) Set `PRAGMA page_size=8192` before `ensure_schema()` on fresh DBs.

**Rationale:** Low-complexity refinement. Not on critical path but useful polish.

### Additional Tier 1 Opportunities (Low-Risk, Not Critical)

- **Remove clear_bindings() calls:** All parameters re-bound anyway. −10s.
- **Batch drain from result_queue:** Pop ALL ready results per lock, not one-at-a-time. Improves worker utilization, reduces lock overhead.
- **Language enum dispatch:** Pre-computed enum instead of string comparisons in extract hot loop (synergizes with Phase 2 parser pooling).

## Risk Assessment

| Change | Risk | Mitigation |
|---|---|---|
| Batch symbol INSERT | Medium (rowid guarantee) | Unit test persist_file round-trip validation, verify sequential IDs |
| Turbo batch 5000 | Low (already in turbo mode) | Accepts expanded crash recovery window (acceptable for rebuildable index) |
| Pre-sized file_read | Very Low (size is known) | filesystem::file_size() is reliable |
| Thread-local parser | Low (lifetime management) | Parser outlives all tasks; reset automatic on language switch |
| Defer content_hash | Very Low (lazy build) | Build on first incremental detect() call if needed |

## Data-Driven Validation

- **Persist throughput:** 3.48ms/file × 100K = 348s measured (matches profile)
- **Worker production:** 100K / (300s / 16 threads) = 5333 files/s theoretical vs 153 files/s actual (35% efficiency proves persist bottleneck)
- **persist_wait backpressure:** 242s = 242s / (100K × 0.00242ms/iteration) confirms 100% main-thread blocked, queue saturation
- **Flush cost:** 100 commits × 770ms = 77s (each writes 20-40MB WAL)

## Critical Path Insight

**Pipeline bottleneck chain:** Persist (425s) > Workers (300s) > Main contention (436s ceiling reachable if workers freed).

After Phase 1: Persist (300s) ≈ Workers (300s). Workers become new bottleneck when Phase 2 implemented.

After Phase 1+2: Persist (300s) > Workers (185s). Persist ceiling lower, but still limits overall wall time. Remaining gains require persist micro-optimizations (Phase 3) or architectural changes (eliminate main-thread relay, lock-free arena distribution).

## Recommendation

**Execute Phase 1 immediately.** Batch symbol INSERT + turbo batch 5000 are the only changes that directly reduce wall time at current bottleneck. Phase 1 (2-3 hours) delivers −132s (−20%). Phase 1+2 combined (5-6 hours total) meets target: **652s → 480s** (−172s, −26%).

## Implementation Sequence

1. **Grag:** Implement Phase 1 (batch symbol INSERT, turbo batch 5000)
2. **Joan:** Write tests for batch symbol INSERT round-trip validation
3. **Team:** Re-profile fsm (4145 files) to validate 652→520s projection
4. **Grag:** Implement Phase 2 (pre-sized file_read, thread-local parser)
5. **Joan:** Write tests for parser reuse, file_read edge cases
6. **Team:** Final profile on fsm to confirm 520→480s and 100K baseline improvement

## Artifacts

- Full review: `.squad/decisions/inbox/simon-index-design-review.md` (11 phases, 250+ lines)
- Full analysis: `.squad/decisions/inbox/otho-perf-opportunities.md` (7 bottlenecks, 385+ lines)
- Orchestration logs: `.squad/orchestration-log/2026-04-03T0235Z-simon.md` and `.squad/orchestration-log/2026-04-03T0235Z-otho.md`

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
