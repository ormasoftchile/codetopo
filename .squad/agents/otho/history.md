# Otho — History

## Project Seed

- **Project:** codetopo — MCP server in C++ for indexing symbols/relations across large codebases
- **Stack:** C++20, CMake, vcpkg, SQLite, tree-sitter, MCP protocol
- **User:** Cristiano
- **Key challenge:** Arena allocator needs tuning; large-file arena (1024MB) separate from standard arena (128MB). Turbo mode (synchronous=OFF, batch=1000) exists but indexer crashes before completing.

## Learnings

### 2026-03-30: resolve_references Step 6 — SQL join elimination
- **Root cause:** Step 6 used a 3-way SQL join (`refs × files × nodes`) to create edges. In turbo/bulk mode, all indexes are dropped for fast insert. Even after `rebuild_indexes()` (which runs before `resolve_references()`), the join on 2.87M refs × 4.5M nodes is inherently expensive.
- **Fix:** Collect edge tuples in-memory during Step 5's resolution loop (file_node_id, resolved_id, kind), then batch-insert directly. Zero SQL joins.
- **Key insight:** The in-memory maps (`fileid_to_path`, `file_node_map`) already have all data needed for edge construction. The SQL join was redundantly re-deriving what we already computed.
- **Files:** `src/index/persister.h` — `resolve_references()` method, Steps 5-6
- **Call order in cmd_index.cpp:** `rebuild_indexes()` (line 715) → `resolve_references()` (line 724). Indexes DO exist when resolver runs, but the join is still the bottleneck.
- **Pattern:** When you have in-memory hash maps from earlier steps, never go back to SQL for derived lookups. Keep data in-memory across steps.

### 2026-03-30: resolve_references 761s bottleneck — deferred index rebuild + FK disable
- **Symptom:** 162K-file run: indexing 489s, rebuild indexes 117s, cross-file resolution **761s**, FTS 50s.
  - 3700 edges/s throughput = terrible for batch SQLite operations.
- **Root causes found (ordered by impact):**
  1. **Index maintenance during writes (~450s):** `rebuild_indexes()` ran BEFORE `resolve_references()`, rebuilding ALL 10 secondary indexes including `idx_edges_src`, `idx_edges_dst`, `idx_refs_resolved`. Then resolve_references did 2.87M UPDATEs to refs (maintaining `idx_refs_resolved`) and 2.87M INSERTs to edges (maintaining both edge indexes). Random B-tree insertions into 3 secondary indexes dominated the runtime.
  2. **FK checks (~50-80s):** `PRAGMA foreign_keys=ON` forced 3 FK lookups per resolved ref (1 for UPDATE refs, 2 for INSERT edges) = 8.6M B-tree probes against the 4.5M-row nodes table.
  3. **Duplicate edges on re-run (correctness bug):** No UNIQUE constraint on edges(src_id, dst_id, kind) and no DELETE before re-insert. Every run accumulated duplicate edges. The "Resolved 0 refs, created 2.87M edges" on second run was the old Step 6 SQL join re-creating edges unconditionally.
  4. **Redundant Step 4 table scan (~5-10s):** Separate SQL query for class/struct symbols already loaded in Step 1.
  5. **Small batch commits (~5-10s):** 10K batch = 287 commit boundaries for 2.87M ops.
- **Fixes applied:**
  1. Split `rebuild_indexes` into read-path (before resolver) and write-path (after resolver). Only `idx_refs_resolved`, `idx_edges_src`, `idx_edges_dst` are deferred.
  2. `PRAGMA foreign_keys=OFF` around `resolve_references()`, re-enabled after.
  3. `DELETE FROM edges WHERE kind IN ('calls','includes','inherits') AND evidence='name-match'` before re-insert.
  4. Merged Step 1 & Step 4 into single scan (added `kind` column to Step 1 query).
  5. Batch commit size increased from 10K → 100K.
- **Expected improvement:** 761s → ~100-150s (5-7x faster). Main savings from deferred index rebuild.
- **Files changed:** `src/cli/cmd_index.cpp` (index rebuild split), `src/index/persister.h` (all other changes)
- **Key principle:** For batch write phases, only maintain indexes needed for reads. Build write-path indexes once after all writes complete — sequential scan + sort is 10-50x faster than per-row B-tree maintenance.

### 2026-04-01: Post-DEC-032 profiling — persist still bottleneck, not workers
- **Methodology:** Added `--profile` flag with full `ScopedPhase` instrumentation (12 main-thread + 5 worker-thread phases). Profiled at 4 data points: 500/2000/4145 files at 512KB, and 4145 at 1024KB.
- **Key finding: persist is STILL the #1 bottleneck** — contrary to DEC-032's conclusion that workers now constrain the pipeline. Workers produce 720-909 files/s but persist caps at 194-617 files/s depending on batch amortization.
- **Contention (35% wall at fsm):** Bursty arrival from largest-first sort. All workers hit big files simultaneously, finish in waves, persist bursts then waits. Main thread: 42% persisting, 49% waiting, 9% overhead.
- **file_read explodes at 1024KB:** 5ms→47ms (9.4x). Large files + largest-first sort + 16 threads = disk I/O contention. The 512KB default (DEC-029) is critical for performance.
- **Parse dominates worker time at 512KB (46-70%, 9-16ms/file):** Irreducible tree-sitter cost. Not a practical optimization target.
- **Extract well-controlled (3-6ms, 20-28%):** DEC-029 extraction timeout working correctly.
- **ThreadPool STILL at 64MB stacks (cmd_index.cpp:582):** DEC-021 fix never applied. 18×64MB=1.15GB committed.
- **Per-file persist cost scales with symbol count:** 500 largest → 5.2ms/file, all 4145 → 1.6ms/file.
- **Parallel efficiency 16-50%:** Single-threaded persist wastes 50-84% of available worker parallelism.
- **Artifacts added:** `--profile`, `--max-files`, `--extract-timeout` CLI flags; `Config::max_files`, `Config::extraction_timeout_s`, `Config::profile`; TreeGuard move-assignment; arena.cpp redefinition fix.
- **Files changed:** `src/core/config.h`, `src/core/arena.cpp`, `src/main.cpp`, `src/cli/cmd_index.cpp`, `src/index/parser.h`, `src/index/supervisor.cpp`

### 2026-04-01: DEC-034 R2 — Pipelined persist thread architecture (for profiling follow-up)
- **Architecture:** Grag implemented pipelined persist thread following DEC-034 R2 recommendations from Otho's profiling.
  - Main thread now: collects worker results, revives slots, refills pool, pushes to persist queue, displays progress only
  - Dedicated persist thread: owns Persister, calls `begin_batch()`/`commit_batch()`, drains queue in loop
  - `PersistQueue` (mutex+condvar) with batch drain: consumes ALL available items per wake (not one-at-a-time)
  - `std::atomic<int> persisted_count`: tracks actual SQLite commits for progress file
  - Result: Main thread free to loop, waiting for worker results (not blocked on SQLite writes)
- **Profiler interpretation change:** `contention` phase now measures pure main-thread idle time (waiting for workers), not persist-blocked time. High contention % is expected and healthy in pipelined model.
- **Benchmark (fsm, 4145 files):** 207 files/s, 20s wall. Persist: 15.5s (4ms/file on dedicated thread, not blocking). Contention: 19.4s (expected).
- **Also implemented R1:** ThreadPool stack 64→8MB (one-liner, cmd_index.cpp:584). Saves 1.15GB committed memory.
- **Key insight for next phase:** With persist decoupled, new bottleneck is likely worker throughput. Next profiling should focus on worker efficiency (file_read, parse, extract) to see if stmt caching (DEC-027 pattern) moves the needle further.

### 2026-04-01: DEC-034 R2 Validation — Pipeline now nearly balanced
- **Methodology:** Clean profiling runs on tiny (500), small (2000), fsm (4145) against DsMainDev/Sql/xdb/manifest/svc/mgmt/fsm with `--profile --turbo`.
- **Key finding: Pipeline is NEARLY BALANCED at scale.** Persist/worker ratio: tiny 1.71:1 (still persist-bound), small 1.07:1, fsm 1.07:1. Pre-R2 was ~2:1 at fsm. Grag's pipelined persist thread eliminated the persist-as-blocking-bottleneck pattern.
- **Indexing-phase rates confirmed:** tiny 100 files/s, small 181 files/s, fsm 218 files/s. Grag's claimed 207 files/s validated (within run-to-run variance). Total rates lower (87/124/137) due to post-indexing phases.
- **Contention semantics validated:** 50.5% contention at fsm is HEALTHY — means main thread waiting for workers, not blocked on persist. Pre-R2's 35% contention was persist-blocked time (unhealthy). Apples-to-oranges comparison.
- **New bottleneck: WAL checkpoint emerged as #3 cost center.** 4.8s (16% wall) at fsm. Fully synchronous, on main thread after indexing. Could be overlapped with resolve_refs phase.
- **Worker time breakdown (fsm):** file_read 49.6% (29ms), parse 34.5% (20ms), extract 15.9% (9ms). file_read dominates because largest-first sort hits 16-thread disk I/O contention on large files first.
- **Parallel efficiency:** 41% (tiny), 57% (small), 49% (fsm). Low at tiny because 500 largest files = high per-file cost, bursty waves. Efficiency improves at small where smaller files amortize better, but drops at fsm due to the long tail of initial large-file processing.
- **Scaling analysis:** Per-file persist cost drops from 8ms (tiny) → 5ms (small) → 4ms (fsm) as batch amortization improves. file_read avg drops 48ms → 42ms → 29ms as file size mix normalizes.
- **Next optimization targets (priority order):**
  1. WAL checkpoint overlap — run during resolve_refs to hide 4.8s
  2. Stmt caching on persist thread — could reduce 4ms/file persist cost
  3. file_read prefetch or memory-mapped I/O — attack the 29ms/file worker bottleneck
  4. Parse is irreducible (tree-sitter) — not a practical target

### Simon's Analysis & Five Optimization Vectors (2026-04-01)
- **Architecture assessment:** Indexer is well-designed. R1+R2 (Grag) correctly fixed structural issues. Remaining bottlenecks are micro-optimizations in parser allocation, string comparison, I/O buffering.
- **Phase 1 (Quick Wins, 3.5 hours):** Pre-alloc file read (30 min, +40 f/s), language dispatch (1 hr, +18 f/s), parser pooling (2 hr, +15 f/s). Expected cumulative 260–280 files/s.
- **Phase 2 (Refinements, 5.5 hours):** String cache (1.5 hr, +8 f/s), parallel resolve (4 hr, offline optimization). Expected 285–300 files/s.
- **Combined potential:** 280–320 files/s (+35–55%). All ranked by ROI and effort.
- **Deprioritized:** Lazy resolve (architectural shift), parallel WAL persist (R2 solved it, no measured lock contention), vectored I/O (marginal gains for complexity).
- **Key decision points locked in:** Parser reuse pattern (thread-local, no locks), language dispatch (enum-based switch, no strings in hot loop), file read buffering (pre-alloc exact size).

### Cross-Agent Summary & Next Steps (2026-04-01)
- **Grag's leak fix validated:** 207/208 tests, build clean. DEC-035 locked.
- **Otho's validation confirmed:** R2 pipeline nearly balanced at 1.07:1 ratio. Workers now ceiling. R3a (WAL checkpoint) is quick win. DEC-036 locked.
- **Simon's roadmap ready:** Phase 1 code review → implementation → re-profile. DEC-037 locked.
- **Build status:** MSVC + CMake clean. 208 tests (1112 assertions), 207 pass (1 pre-existing flake).
- **Team actions:** Code review Phase 1, implement in parallel if possible, profile before/after on fsm to validate gains.
