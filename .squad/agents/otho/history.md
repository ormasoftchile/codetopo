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
