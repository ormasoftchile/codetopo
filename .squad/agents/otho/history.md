# Otho — History

### 2026-03-31: Pipeline bottleneck identified — single-threaded persist is the wall

- **Benchmark:** xdb\manifest, 55,792 files, 16 threads, parse-timeout 5s, max-file-size 1024KB
- **Result:** 278 files/s (200s wall), 12 timeouts, contention 0.9s
- **Profile (52,986 files):** parse=254.1s (4796µs/f, 60%), extract=139.5s (2632µs/f, 33%), read=26.4s (498µs/f, 7%)
- **Worker capacity:** 420.4s total thread-time ÷ 16 = 26.3s wall → workers can do 2100+ files/s
- **Main thread persist:** 200s serial → 3.6ms/file → 278 files/s. **This is the bottleneck.**
- **Workers are 7.6× faster than persister can consume.** Result queue stays perpetually full.
- **Per-file persist cost:** 5 unprepared SQL statements per call (278K redundant sqlite3_prepare_v2), SQLITE_TRANSIENT string copies, DELETE-cascade even on cold index
- **Clean subfolder (829 files/s):** persist is ~1.2ms/file (small uniform files). Full xdb\manifest: ~3.6ms/file (heavy files with 100-500 symbols/refs)
- **Timeout impact is a red herring:** 12 × 5s = 60 thread-s = 1.9% of available worker time. Workers aren't the bottleneck.
- **Velocity pattern:** 700+ f/s through 50%, crashes to 91 f/s during 59-89% (complex file subtree), recovers to ~280
- **Recommendations (DEC-026):** Cache prepared statements + SQLITE_STATIC (quick wins), skip DELETE on cold index, pipelined/parallel persist (architectural fix for >500 files/s)
- **Key insight:** DEC-025 fixed worker-side tail latency. Pipeline throughput is now capped by serial SQLite persist. No worker-side optimization will help until persist is unblocked.

### 2026-03-31: Per-phase profiling — extract is the tail-latency killer

- **Benchmark target:** `C:\One\DsMainDev\Sql\xdb\manifest\svc\mgmt\fsm` — 4145 C# files, clean representative workload.
- **Clean run (max-file-size 256KB, no outliers):**
  - `Profile (4052 files): arena=0.0s (0us/f) read=2.5s (608us/f) hash=0.0s (7us/f) parse=26.9s (6640us/f) extract=7.4s (1827us/f)`
  - Wall-clock: 5s at **829 files/s** — near baseline! Contention: 0.0s.
  - Phase breakdown: parse=73%, extract=20%, read=7%. Arena/hash negligible.
  - Thread-time total: 36.8s / 16 threads = 2.3s ideal. Observed 5s = 46% parallel efficiency (DB commit overhead).
- **Uncapped run (max-file-size 1024KB, with outliers):**
  - `Profile (4131 files): arena=0.0s (0us/f) read=3.1s (760us/f) hash=0.0s (7us/f) parse=39.9s (9664us/f) extract=98.7s (23889us/f)`
  - Wall-clock: **91s at 45 files/s** — 18x slower! Contention: 83.7s across 6 stalls.
  - Extract blew up from 1.8ms/f → 23.9ms/f (13x). A few large files taking 10-15s each blocked workers.
  - 1 parse timeout hit (5s cap), but extract has NO timeout — unbounded tail latency.
- **Full 162K-file run (background, max-file-size 1024):**
  - At 71%, 115K files in 415s, trending 280-340 f/s. Periodic stalls from pathological files pulling average down from ~340 to ~213 over full run.
- **Root cause of 700→213 regression:**
  - NOT contention (0.0s on clean run, 1.6s on previous full run).
  - NOT arena or hash (negligible).
  - **Primary: Extraction has no timeout.** A few files per batch produce huge ASTs (10K+ nodes) causing 10-15s extraction times. These block worker threads and create pipeline stalls. With 16 threads, one 15s stall = 15s × 16 = 240 file-slots wasted.
  - **Secondary: Parse overhead from `set_timeout(30s)`.** Tree-sitter's internal QPC polling adds ~15µs/file overhead. With baseline 6.6ms/f for small files, this is ~0.2%. Negligible per-file but the timeout mechanism itself may interfere with tree-sitter's parser state machine optimization.
  - **Tertiary: Large file I/O (read=760us/f uncapped vs 608us/f capped).** Larger files simply take longer to read.
- **Key insight:** The regression is a **tail-latency problem**, not a throughput problem. Median file processes at ~830 f/s (near baseline). But ~0.1% of files (large/complex ASTs) take 100-1000x longer and have no timeout, creating cascading pipeline stalls across all threads.

### 2026-04-01: Partial profiling infrastructure — --max-files flag + batch script

- **Problem:** Full 104K-file repo runs take 5-10+ minutes, too slow for rapid iteration on optimizations. 
- **Solution:** Added `--max-files N` CLI flag (Config, Scanner, main.cpp, supervisor.cpp, cmd_init.h). Truncates after scan to N files. Combined with `--root` pointed at subdirectories to avoid slow git ls-files on the full 162K-file monorepo.
- **Key insight:** `--max-files` alone isn't enough for fast profiling on git repos — `git ls-files` still scans the entire repo (72s for DsMainDev). Using `--root` on a subdirectory (no `.git`) forces manual dir walk, which is much faster for targeted subsets.
- **Profiling script:** `profile_subset.bat` with presets: tiny (500f, ~10s), small (2000f, ~15s), fsm (4145f, ~39s), medium (10Kf, ~60s), large (50Kf, ~5m), full.
- **DsMainDev/Sql repo structure:** 104K source files total. Breakdown: xdb/manifest/svc/mgmt/WebApiHosting (25.7K), mgmt/workflows (15.7K), mgmt/fsm (4.1K baseline). The fsm subdirectory is the established baseline (all C#, uniform size, well-characterized).
- **Benchmark results (fsm, 4145 files, 39s wall):**
  - Worker phases: file_read=82%, parse=13%, extract=5%. Read dominance suggests network/storage bottleneck.
  - Main thread: contention=32%, persist=26%, scan=11%. Contention is workers waiting for persist to consume.
  - Parallel efficiency: 64% — best we've seen. Workers produce at ~164 f/s sustained.
- **Benchmark results (tiny, 500 files from fsm, 10.4s wall):**
  - Persist=46%, scan=44%. At small scale, scan overhead and persist startup dominate.
  - Workers can produce 741 f/s but persist caps at 104 f/s.
- **Files changed:** `src/core/config.h` (max_files field), `src/index/scanner.h` (truncation), `src/main.cpp` (CLI options), `src/index/supervisor.cpp` (arg passthrough), `src/cli/cmd_init.h` (signature), `profile_subset.bat` (new).

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

### 2026-03-30: Performance audit — implementation quality verification
- **DEC-014 (Deferred index rebuild):** Verified correctly implemented. `cmd_index.cpp` lines 714-731 rebuild only read-path indexes before `resolve_references()`. Lines 743-749 rebuild `idx_refs_resolved`, `idx_edges_src`, `idx_edges_dst` AFTER resolve completes. FK checks properly disabled/re-enabled around the resolver. Clean separation.
- **DEC-015 (SQL join elimination):** Verified correctly implemented. `persister.h` collects `EdgeTuple` structs in a `std::vector` during Step 5's resolution loop, then batch-inserts in Step 6. No SQL joins remain in the resolve path. The `edge_tuples.reserve(1000000)` is a reasonable initial allocation.
- **Benchmark (codetopo self-index):** 51 files indexed in 1s, 140 cross-ref edges resolved in <1s. Too small to stress-test, but confirms zero regression.
- **Remaining bottleneck #1 — `INSERT OR IGNORE` without UNIQUE constraint:** `persister.h:502` uses `INSERT OR IGNORE INTO edges(...)` but the `edges` table (schema.h:79-87) has NO UNIQUE constraint on `(src_id, dst_id, kind)`. `OR IGNORE` only suppresses constraint violations — without a unique constraint, it's functionally identical to plain `INSERT`. The preceding `DELETE FROM edges WHERE kind IN (...) AND evidence = 'name-match'` prevents duplicates on re-runs, so this is a **misleading code pattern, not a data bug** — but it would silently accumulate duplicates if the DELETE were ever removed or the filter changed. Recommend either adding `UNIQUE(src_id, dst_id, kind)` to the schema or changing to plain `INSERT`.
- **Remaining bottleneck #2 — CASCADE DELETE cost at scale:** `persist_file()` does `DELETE FROM files WHERE path = ?` which cascades to nodes → edges, refs. On a file with 500 symbols and 2000 refs, this triggers ~2500 cascade deletes across 3 tables per file. With indexes dropped in bulk mode, each cascade is a full table scan. For 162K files on re-index, this is ~400M cascade operations without index support. Mitigation: the DELETE is only hit for changed/re-indexed files (not new ones), and bulk mode only triggers on >1000 files.
- **Remaining bottleneck #3 — `symbol_map.reserve(2500000)` is hardcoded:** For repos smaller than 2.5M symbols, this wastes memory. For repos larger, it causes rehashing. Consider sizing based on a COUNT query or the number of files × average symbols/file.
- **Remaining bottleneck #4 — String copies in resolution loop:** Each ref's `kind` and `name` are copied to `std::string` from SQLite text. These are short-lived and only used for hash lookups. Using `std::string_view` where the SQLite row data is valid would eliminate ~5.7M string allocations (2.87M refs × 2 strings).
- **Code quality observations:**
  - Thread pool, arena pool, and watchdog implementations are solid — no race conditions detected.
  - Arena routing logic correctly uses `file_size * 30 > arena_capacity` threshold instead of old fixed-KB threshold. Good.
  - SEH translator properly calls `_resetstkoflw()` before throwing — prevents cascading `__fastfail` crashes.
  - Batch commit boundaries (100K) are well-chosen for SQLite's journal performance characteristics.

### 2026-03-30: Performance regression 700→300 files/s — root cause analysis
- **Context:** Uncommitted changes to arena.cpp, thread_pool.h, cmd_index.cpp introduced 2.3x throughput regression.
- **RC-1 (HIGH): 64MB committed stacks.** `_beginthreadex` in `thread_pool.h:33` called with flags=0, making 64MB the COMMIT size (not reserve). 18 threads × 64MB = 1.15GB committed for stacks vs 16MB before. Fix: reduce to 8MB or use `STACK_SIZE_PARAM_IS_A_RESERVATION`.
- **RC-2 (HIGH): Largest-first sort + small window.** `cmd_index.cpp:228` sorts work_list by size_bytes descending. All 18 initial slots process multi-MB files simultaneously. Old code had no sort. Fix: remove sort or partial_sort top N only.
- **RC-3 (MEDIUM): Window_size = thread_count + 2 too small.** Old code pre-queued all 162K futures (workers never idle). New code limits in-flight to 18 tasks; main thread serializes result processing. During batch commits (~100ms), workers stall. Fix: increase to thread_count * 4.
- **RC-4 (MEDIUM-LOW): System malloc fallback leaks.** `ts_arena_free` is no-op even for system malloc fallback allocations in new arena.cpp. Memory leaks accumulate over 162K files.
- **RC-5 (LOW): Non-inlined ts_arena_realloc.** Moved from inline header to .cpp. Millions of realloc calls add ~3-5s function call overhead.
- **Key insight:** No single change explains 2x. It's the combination: memory pressure from committed stacks + pipeline starvation from small window + poor scheduling from biggest-first sort. Each shaves 15-35% throughput; multiplicatively they compound to ~2.3x.
  - Progress file correctly written only on batch commit boundaries, not per-file.
  4. Merged Step 1 & Step 4 into single scan (added `kind` column to Step 1 query).
  5. Batch commit size increased from 10K → 100K.
- **Expected improvement:** 761s → ~100-150s (5-7x faster). Main savings from deferred index rebuild.
- **Files changed:** `src/cli/cmd_index.cpp` (index rebuild split), `src/index/persister.h` (all other changes)
- **Key principle:** For batch write phases, only maintain indexes needed for reads. Build write-path indexes once after all writes complete — sequential scan + sort is 10-50x faster than per-row B-tree maintenance.

### 2026-03-30: Deep regression -- 700 to 286 files/s true root causes
- **Context:** Previous analysis (RC-1 stack 64 to 8MB, RC-3 window thread_count*4) fixed real issues but missed the dominant per-file hot-path costs. Throughput dropped further to 286 files/s.
- **RC-A (HIGH): Arena allocator de-inlined.** `ts_arena_realloc` moved from inline `arena_realloc()` in `arena.h` to non-inline static in `arena.cpp`. Tree-sitter calls realloc ~5000x/file x 162K files = ~810M calls. Function call overhead + extra 512MB branch + lost compiler optimizations = 30-80s total. Fix: move hot-path allocator functions back to inline or use `__forceinline`.
- **RC-B (HIGH): Iterative DFS heap allocation per file.** `extractor.cpp:visit_node` converted from recursive to iterative with `std::vector<Frame>` -- allocates heap memory (162K allocations) + `std::chrono::steady_clock::now()` every 4096 nodes (1.6M system calls). Fix: make stack `thread_local static` + check deadline every 65536 nodes.
- **RC-C (MEDIUM): System malloc fallback leaks.** New `ts_arena_malloc` falls back to `std::malloc` but `ts_arena_free` is a no-op. Leaked blocks cause memory pressure and heap fragmentation across 162K files.
- **Key lesson:** When a function is called hundreds of millions of times, nanosecond changes compound to minutes. Previous analysis focused on system-level resources (stack, concurrency) but missed per-invocation hot-path costs. Always profile the innermost loop first.

### 2026-03-30: Thread contention deep-dive — the real 700→315 regression cause
- **Context:** RC-A/RC-B fixes (inline arena, thread_local extractor stack) confirmed applied but throughput stuck at 315 files/s. Cristiano requested exclusive focus on threading/synchronization.
- **Confirmed:** Arena inlining (arena.h:170), thread_local stack (extractor.cpp:210), 65536-node deadline check (extractor.cpp:221) all in place. Prior hot-path fixes are correct.
- **ROOT CAUSE (CRITICAL): ThreadPool sized to `window_size` (72) instead of `thread_count` (18).** `cmd_index.cpp:564`: `ThreadPool worker_pool(window_size, 8MB)`. The `window_size = thread_count * 4` was meant as an in-flight task budget, not the thread count. Creates 72 OS threads on 18 cores.
  - 36+4 = 40 arenas → 40 threads active, 32 permanently blocked on arena condvar
  - 40 active threads on 18 cores = 2.2x oversubscription → context switching + cache thrashing
  - 72 × 8MB committed stacks = 576MB (due to `_beginthreadex` flags=0)
  - Net effect: each core runs at ~45% efficiency due to cache pollution from thread switching
  - Math: 18 cores × 17.5 files/s/core ≈ 315 files/s — matches observed throughput exactly
- **SECONDARY: `_beginthreadex` flags=0 commits full stack.** `thread_pool.h:33` passes 0 for flags, making 8MB the commit (not reserve) size. Should use `STACK_SIZE_PARAM_IS_A_RESERVATION`.
- **NOT THE CAUSE:** Mutex scopes are all tight (microsecond queue ops). `notify_one` used correctly everywhere. `SQLITE_OPEN_NOMUTEX` avoids SQLite internal locking. Console output is ~100 writes total, not per-file. Main thread refills before persisting — no submission starvation.
- **Fix:** Change `ThreadPool worker_pool(window_size, ...)` to `ThreadPool worker_pool(thread_count, ...)`. Add `STACK_SIZE_PARAM_IS_A_RESERVATION` flag. Expected: 700+ files/s restored.
- **Key lesson:** When a variable controls two things (concurrency budget AND thread count), they must be split. Thread pool size should match CPU cores; task queue depth is a separate concern. DEC-017 filed.

### 2026-03-31: Surgical parse regression analysis — 213 f/s vs 700 baseline
- **Context:** Cristiano requested line-by-line analysis of 6 modified hot-path files. Threading ruled out (1.6s contention). Pure per-file cost: 3.3ms/file delta.
- **Files analyzed:** arena.h (77 lines), arena.cpp (-20 lines), arena_pool.h (+13 lines), extractor.cpp (135 lines), extractor.h (+11 lines), parser.h (+13 lines).
- **Findings by file:**
  1. **arena.h:** Fast-path +1 branch/alloc (~20µs/file). System malloc fallback added but never executes (128MB arena unchanged). `ts_arena_free` leaks fallback allocs (correctness bug, not perf). `inline` keyword useless — called through function pointers.
  2. **extractor.cpp (LARGEST CONTRIBUTOR):** Recursive→iterative DFS. `thread_local static vector<Frame>` adds TLS overhead (~5-10ns/node). 48-byte Frame × tree depth. `process_node` lambda may not be inlined by MSVC (40-line body with 10-way if/else). Estimated ~300-800µs/file.
  3. **extractor.cpp parent_qualname:** `const std::string& parent_qualname = root_qualname` is NOT a bug — old recursive code also passed same value unchanged to all descendants (always "").
  4. **parser.h:** `set_timeout(30s)` activates tree-sitter's internal clock polling (QPC every 100 ops, ~15µs/file). NEVER existed in old code.
  5. **arena_pool.h:** `try_lease()` not on hot path. Zero impact.
  6. **extractor.h:** 24 bytes added to Extractor struct, one `steady_clock::now()` per file. Negligible.
- **GAP:** 6 files explain ~0.3-0.8ms of 3.3ms delta (10-25%). Remaining 2.5ms is OUTSIDE these files — likely in cmd_index.cpp config changes, batch dynamics, or memory pressure effects.
- **Key lesson:** Static analysis of isolated files has diminishing returns when the regression spans cross-cutting concerns (config propagation, memory allocation patterns, batching strategy). Always verify with instrumented profiling once static analysis accounts for <50% of the gap.
- **Key lesson 2:** `inline` on functions whose address is taken for function pointers provides zero optimization benefit — the compiler must emit an out-of-line copy anyway. The larger function body just increases I-cache pressure at the call site.

### 2026-07-04: Profiling harness — `--profile` flag with comprehensive phase breakdown
- **What:** Created `src/util/profiler.h` with `Profiler`, `PhaseTimer`, and `ScopedPhase` RAII types. Added `--profile` CLI flag to both `index` and `init` subcommands. Instrumented all 12 main-thread phases and 5 worker-thread phases in `cmd_index.cpp`.
- **Phases instrumented (main thread):** scan, change_detect, prune, persist, flush, contention, index_rebuild_read, resolve_refs, index_rebuild_write, fts_rebuild, wal_checkpoint, metadata.
- **Phases instrumented (worker threads):** arena_lease, file_read, hash, parse, extract.
- **Report format:** Two sections (main-thread wall-time, worker thread-time) + automatic bottleneck analysis (top 3 phases) + persist-vs-worker comparison. Always prints the existing one-line `Profile (N files): ...` summary. Only prints the full table when `--profile` is passed.
- **Key design decisions:**
  - `PhaseTimer` uses `std::atomic<int64_t>` with relaxed ordering — zero contention overhead.
  - `ScopedPhase` RAII pattern means no risk of missing stop calls.
  - Profiler always collects data (atomics are cheap) — only the report is gated by `--profile`.
  - Added `persist` timing to the always-on one-line summary since it was the known bottleneck but was never measured.
  - `--profile` flag plumbed through supervisor→child process so it works in supervised mode too.
- **Files changed:** `src/util/profiler.h` (new), `src/core/config.h`, `src/main.cpp`, `src/cli/cmd_init.h`, `src/cli/cmd_index.cpp`, `src/index/supervisor.cpp`.
- **Build:** Release + tests all pass (179 tests, 939 assertions).
- **Smoke test (self-index, 8 files):** Report correctly showed metadata (26%), scan (19%), persist (15%) as top phases. Worker breakdown: parse 57%, read 22%, extract 21%. Parallel efficiency: 1% (expected — too few files for 16 threads).

### 2026-07-04: SQLite persist 6x scaling degradation — root cause analysis (4K→100K files)
- **Profile comparison:** FSM 4,145 files → 1.5ms/file persist. Full 105,694 files → 9.1ms/file persist. 6x degradation, 62% of wall time.
- **CRITICAL finding: `--turbo` flag is dead code.** `config.turbo` is set and plumbed through supervisor, but `conn.enable_turbo()` is **never called** in cmd_index.cpp. All profiled runs used synchronous=NORMAL, default autocheckpoint, and 64MB cache. This is the single biggest contributor (~2.8-3.8ms/file).
- **RC-2: `nodes.stable_key` UNIQUE constraint maintained during bulk insert.** `drop_bulk_indexes()` only drops CREATE INDEX indexes, not column-level UNIQUE constraints. At 3.4M nodes, this is a 4-level B-tree with ~80-byte text keys. ~1-2ms/file at scale.
- **RC-3: mmap_size=256MB undersized for 2GB database.** At 100K files, DB grows to ~2GB. Only 13% is mmap'd; rest uses read() syscalls for B-tree traversals. ~1-2ms/file at scale.
- **RC-4: FK checks ON during bulk persist.** ~142 FK B-tree probes per file, all against tables exceeding mmap. FK only disabled inside resolve_references(), not during main drain loop. ~0.5-1ms/file.
- **file_read 1.1ms→65ms:** Filesystem cache eviction. 200MB fits in Windows cache; 5.3GB does not. Not fixable in code.
- **Recommendations:** R1 wire turbo PRAGMAs (P0, low), R2 FK off during bulk (P1, low), R3 mmap 2GB (P1, low), R4 page_size 8192 (P2, medium), R5 batch_size 5000 (P2, low), R6 drop stable_key UNIQUE during bulk (P3, high).
- **Expected combined R1+R2+R3:** 9.1ms→3.5-4.5ms/file (~2.5x improvement). Total wall time ~1474s→~930-1030s.
- **Key principle:** Always verify that CLI flags actually reach the codepath they claim to configure. "Dead flag" bugs are invisible to users and profiling — the system appears to work, just slower than expected.
- **Files analyzed:** `src/cli/cmd_index.cpp`, `src/index/persister.h`, `src/db/connection.h`, `src/db/schema.h`, `src/core/config.h`, `src/main.cpp`, `profile_subset.bat`.
