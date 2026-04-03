# Grag — History

## Project Seed

- **Project:** codetopo — MCP server in C++ for indexing symbols/relations across large codebases
- **Stack:** C++20, CMake, vcpkg, SQLite, tree-sitter, MCP protocol
- **User:** Cristiano
- **Key challenge:** Indexer uses detached threads + watchdog for crash recovery, but this introduced STATUS_STACK_BUFFER_OVERRUN crashes. Prior approach (fixed thread pool) was stable but lacked timeout/recovery.

## Learnings

### Supervisor exit code classification (2026-03-30)
- `spawn_and_wait` returns: on Windows, `GetExitCodeProcess` cast to int (NTSTATUS exceptions become large negative); on POSIX, `WEXITSTATUS` (0-255) or `128+signal` for signal death.
- Exit codes 1-2 from `cmd_index.cpp` are deliberate returns (parse errors, lock failure) — NOT crashes. The child cleans up worklist/progress before returning.
- Only negative codes (Windows NTSTATUS) and codes >127 (POSIX 128+signal) indicate actual process crashes.
- The `is_crash_exit_code()` helper in `supervisor.cpp` encodes this classification.
- Key file: `src/index/supervisor.cpp` — exit code triage at line ~148.
- Key file: `src/cli/cmd_index.cpp` — line 753: `return errors > 0 ? 1 : 0;` is the source of exit code 1 after successful completion with parse errors.

### Init command must tolerate exit code 1 (2026-03-30)
- `cmd_init.h::run_init()` called `run_index_supervisor()` and treated ANY non-zero return as fatal (`if (index_rc != 0)`), aborting without writing MCP editor configs.
- This meant: after indexing 162K files with 64 parse errors (index fully built, refs resolved, FTS5 rebuilt), the init command aborted — no MCP configs written.
- Fix: `cmd_init.h` now treats exit code 1 as a warning (non-fatal). Only exit codes > 1 abort.
- Root cause chain: child returns `errors > 0 ? 1 : 0` → supervisor correctly returns 1 (non-crash) → init incorrectly aborts.
- Lesson: downstream consumers of exit codes must distinguish "completed with warnings" (code 1) from "hard failure" (codes 2+).

### DEC-034 context: Persist still bottleneck after DEC-032 (2026-04-01)
- Otho profiled full pipeline (4 subsets: 500–4145 files). After DEC-032 improvements (persist 3.3→1.6ms/file), persist **still caps throughput at 617 files/s** (workers can deliver 788).
- Contention is 35% of wall from **bursty arrival**: largest-first sort → workers finish in waves → burst of results → persist burst → idle.
- ThreadPool still uses 64MB stacks (DEC-021 fix never applied) = 1.15GB committed memory.
- Parse dominates worker time (46-70%) at irreducible tree-sitter cost.
- **R1 (HIGH):** Fix ThreadPool stack 64→8MB (1-line, zero risk, 1GB+ RAM savings).
- **R2 (HIGH):** Pipelined persist thread (eliminate 35% contention, architectural refactor ~200 LOC).
- **R3 (MEDIUM):** Parallel WAL persist (DEC-026 R7, higher complexity/ceiling).
- Key for next phase: R1 is blocking the others. Should implement first.

### DEC-034 R1+R2: ThreadPool stack fix + pipelined persist thread (2026-04-01)
- **R1**: Changed `ThreadPool worker_pool(window_size, 64 * 1024 * 1024)` → `8 * 1024 * 1024` at cmd_index.cpp:584. Saves 1GB+ committed physical RAM (18×64MB→18×8MB).
- **R2**: Implemented pipelined persist thread architecture. Decouples SQLite writes from worker result collection:
  - Added `PersistQueue` (mutex+condvar queue with batch drain) and `PersistItem` structs local to `run_index()`.
  - Dedicated `persist_thread` owns the Persister, calls `begin_batch()`/`commit_batch()`, and runs a drain loop consuming all available items per wake.
  - Main thread now only: collects results from workers, revives slots, refills worker pool, pushes to persist queue, displays progress.
  - `std::atomic<int> persisted_count` tracks actual SQLite commits for progress display.
  - Progress file writes (`config.supervised`) happen on the persist thread after each batch commit.
  - Main thread joins persist thread after signaling done, then joins watchdog.
- **Profiler note**: `contention` phase now measures pure main-thread idle time (waiting for worker results), not persist-blocked time. High contention % is expected and healthy — it means the main thread is free.
- **Benchmark (fsm, 4145 files)**: 207 files/s, 20s wall (comparable to baseline of 200 files/s). Persist runs at 4ms/file on dedicated thread. Pipeline decoupling confirmed working.
- Key files: `src/cli/cmd_index.cpp` lines 644-798 (persist pipeline), line 584 (stack fix).

### 2026-04-03: Arena Allocator Thread-Safety Audit — Heap Corruption Root Cause

**Audit scope:** Thread-safety analysis of Arena allocator and pool, tracing root cause of heap corruption crashes at high throughput.

**ROOT CAUSE IDENTIFIED:** Parser reuse (DEC-037a, labeled OPT-5) creates dangling pointers. Thread-local parser caches buffers from arena N; those buffers become inaccessible after arena N is reset. When parser is reused for next file with arena M, tree-sitter calls `realloc()` on old pointers → `arena_M.contains(ptr_from_arena_N)` returns FALSE → code blindly dereferences header at `ptr_from_arena_N - HEADER_SIZE` → **arena N has been reset or is in use by another thread** → heap corruption.

**Five thread-safety issues identified:**
1. **Parser Reuse Across Arena Boundaries (CRITICAL — ROOT CAUSE)** — 100% of crashes. Parser is thread-local and reused per language across files, but each file uses a different arena. Tree-sitter's parser maintains internal buffers allocated via arena allocator. When arena is reset, parser's cached buffers are dangling pointers. Next parse with new arena triggers realloc on old pointers.
2. **Overflow Pointer Tracking Without Free (CRITICAL)** — Realloc'd overflow pointers accumulate orphans in `overflow_ptrs_` vector. When original pointer is freed, new pointer takes its place in vector. Both are freed on reset, but orphaned pointer may still be referenced elsewhere.
3. **Non-Atomic overflow_ptrs_ Modifications (CRITICAL)** — Vector reallocation during malloc overflow is not exception-safe. At high throughput with frequent overflow, vector grows frequently (4→8→16→32), increasing chance of corruption if exception fires during reallocation.
4. **Integer Overflow in Alignment Calculation (LOW)** — If `current + alignment - 1` overflows `size_t`, alignment produces incorrect result. Requires arena near SIZE_MAX (theoretical on 64-bit, unlikely in practice).
5. **Redundant contains() Check in arena_realloc (CODE QUALITY)** — Both if/else branches do identical operations. Dead code, suggests author intended different behavior but never implemented.

**Severity assessment:** Issue #1 (parser reuse) causes **100% of observed heap corruption crashes** at high throughput. Confirmed by: (1) temporal match with warm cache (parser reuse visible in all files), (2) throughput correlation (3.2ms cycle time at 10K f/s vs 1024ms at 500 f/s), (3) crash window analysis (reuse interval < reset duration at high throughput).

**Proposed fix (RECOMMENDED):** Option A — Revert DEC-037a parser reuse. Create new `Parser` per file instead of reusing thread-local map. Risk: ZERO (pure revert). Perf impact: -15 files/s (negligible vs heap corruption). Effort: 5 minutes. Verdict: **IMPLEMENT IMMEDIATELY**.

**Alternative fixes:** Option B (parser reset when switching arenas) requires tree-sitter API not available. Option C (parser-per-arena in ArenaPool) is architectural, 3-4hr effort, future optimization if perf needs warrant.

**Files analyzed:** `src/cli/cmd_index.cpp` (471-472 parser reuse, 432 arena switch), `src/core/arena.h` (119 overflow_ptrs, 47 push_back, 111-112 free_overflow_ptrs, 158-167 redundant check, 41 alignment overflow).

**Test plan:** After revert, run full benchmark with warm cache (162K files) — expect ZERO crashes across multiple runs.

**Deliverables:** `grag-arena-thread-safety.md` (478 lines, 5 issues with detailed analysis, triggering sequences, and fixes).

---

### DEC-034 R2 Follow-up: File node leak on re-persist (Joan's finding)
- **Issue found by Joan during test coverage:** `persist_file()` deletes files with cascade to symbol nodes, but file_nodes (file_id=NULL) survive because NULL doesn't participate in FK cascade.
- **Impact:** Single-file warm re-persist: harmless (ID collision benign, existing tests pass). Batch re-persist: wrong file_node_id can cause edges to reference incorrect src nodes.
- **Root cause:** `INSERT INTO nodes(...stable_key...)` hits UNIQUE constraint silently (unchecked `sqlite3_step` return). `sqlite3_last_insert_rowid()` then returns file record ID, colliding with node IDs from different table.
- **Recommendation:** Add explicit `DELETE FROM nodes WHERE node_type='file' AND name = ?` before file_node INSERT, OR use `INSERT OR REPLACE` for file_node. This also fixes orphaned file_node leak over time.
- **Priority:** Low (current usage unaffected). Consider for DEC-027 stmt caching phase or separate bug fix.

### DEC-040 Arena allocator heap corruption audit (Grag) (2026-04-02)
- **Context:** Warm cache (10,000+ f/s) triggers 7-10 crashes per 100K files with exit codes `0xC0000374` (heap corruption) and `0xC0000791` (stack buffer overrun). Cold cache (500 f/s) has ZERO crashes.
- **ROOT CAUSE IDENTIFIED:** Parser reuse optimization (DEC-039 OPT-5, thread_local parser map) is incompatible with arena switching. Tree-sitter parsers cache internal buffers allocated from arena N, but when thread parses next file with arena M, parser tries to `realloc()` old buffer → reads from arena N (which has been reset or re-leased) → heap corruption.
- **Trigger sequence:** Thread A parses file1 with arena1 → parser allocates internal buffer from arena1 → file1 completes, arena1 reset → thread A parses file2 with arena2 → parser reuses cached state → calls `realloc(ptr_from_arena1, new_size)` with `t_current_arena = arena2` → `arena_realloc` reads header at `ptr_from_arena1 - HEADER_SIZE` → **arena1 memory is invalid** (reset or in use by another thread) → crash.
- **Why only at high throughput:** At 10,000+ f/s, arena lease/return/reset cycle is <1ms. Arena1 is reset and re-leased to thread B before thread A finishes reading from old pointer → concurrent access to same memory → data race.
- **Five issues found:** (1) Parser reuse across arenas (CRITICAL, root cause), (2) Orphaned overflow pointers in realloc (HIGH, memory leak + use-after-free risk), (3) Non-atomic overflow_ptrs_ vector modifications (MEDIUM, exception safety), (4) Integer overflow in alignment calc (LOW, theoretical), (5) Redundant contains() check in arena_realloc (code quality).
- **IMMEDIATE FIX REQUIRED:** Revert DEC-039 OPT-5 parser reuse — change `thread_local std::unordered_map<std::string, Parser> t_parsers;` back to `Parser parser;` (per-file creation) in cmd_index.cpp line 471-472. Risk: ZERO (pure revert). Perf impact: -15 f/s (tolerable vs heap corruption).
- **High-priority fixes:** (1) Add `Arena::remove_overflow_ptr()` and free orphaned pointers in `arena_realloc`, (2) Pre-reserve overflow_ptrs_ vector capacity (128 elements) in Arena constructor.
- **Test plan:** Revert OPT-5, run full benchmark with warm cache, expect ZERO crashes across 162K files. Then apply all fixes, run 10× full index (1.62M files) stress test.
- **Long-term:** If parser reuse is needed, implement "parser-per-arena" architecture where parsers are owned by ArenaPool and reset alongside arenas.
- **Report delivered:** `.squad/decisions/inbox/grag-arena-thread-safety.md` — full audit with exact locations, trigger sequences, and code fixes.

### File node leak fix in persist_file (2026-04-01)
- **Fix:** Added explicit `DELETE FROM nodes WHERE node_type='file' AND name = ?` before the file_node INSERT in `persist_file()`. This cleans up the orphaned file node that survives CASCADE (because `file_id=NULL` doesn't match any FK value).
- **Error checking:** Added `sqlite3_step` return value check on the file_node INSERT. If it returns anything other than `SQLITE_DONE`, throws `std::runtime_error` with the file path and SQLite error message. This catches UNIQUE constraint violations that were previously silently ignored.
- **No stmt cache yet:** DEC-027 not implemented; all statements are inline `prepare/step/finalize`. The new DELETE follows the same pattern. When DEC-027 lands, this DELETE should be cached alongside the others.
- **Test result:** 207/208 passed. Single failure is pre-existing timing flake in `test_watchdog.cpp` (arena parallelism elapsed check).

### Otho's R2 Validation & Cross-Agent Findings (2026-04-01)
- **R2 validated at production scale:** fsm (4145 C# files) now at 218 files/s indexing rate. Persist/worker ratio 1.07:1 (nearly balanced). Workers are the true bottleneck, not persist.
- **Pipeline success:** Persist now 53% of wall (pipelined, off critical path) vs pre-R2 42% (blocking). Main thread high contention (19.4s waiting for workers) is expected and healthy.
- **New #3 bottleneck:** WAL checkpoint 4.8s (16% wall), fully synchronous on main thread post-index. R3a (overlap with resolve_refs) is the quick win.
- **Worker analysis:** file_read 50% (29ms/file, I/O contention with largest-first), parse 20% (20ms, irreducible tree-sitter), extract 9% (9ms, well-controlled). Extract timeout working as designed.
- **Scaling insights:** At 50K files, resolve phase becomes 100–200s offline; parallel resolve becomes high-ROI.

### Simon's Five Optimization Vectors (2026-04-01)
- **Architecture verdict:** Indexer well-designed. DEC-034 R1+R2 correctly addressed structural issues. Remaining bottlenecks are micro-optimizations, not architectural.
- **Phase 1 (Quick Wins):** Pre-alloc file read (+40 f/s), language dispatch (+18 f/s), parser pooling (+15 f/s). Combined 3.5 hours, low risk. Target 260–280 files/s.
- **Phase 2 (Refinements):** String cache (+8 f/s), parallel resolve (post-index optimization). Target 285–300 files/s.
- **Combined potential:** 280–320 files/s (+35–55% over 207 baseline). All five opportunities ranked by ROI and implementation effort.
- **Deprioritized:** Lazy resolve (architectural shift), parallel WAL persist (DEC-034 R2 solved it), vectored I/O (marginal gains).

### Decision Summary (2026-04-01)
- **DEC-035:** File node leak fix locked in. Prepared statement caching (DEC-027) will reuse pattern.
- **DEC-036:** R2 pipelined persist validated. R3a (WAL checkpoint) next quick win.
- **DEC-037:** Five optimization vectors ranked. Phase 1 ready for implementation.
- **Build status:** Clean (MSVC + CMake). 208 tests (1112 assertions), 207 pass (1 pre-existing flake).
- **Next phases:** Implement Phase 1 optimizations, re-profile for validation, Phase 2 candidate preparation.

### Surgical Revert of Phase 1 Harmful Optimizations (2026-04-01)
- **Context:** Commit e1a6762 ("Phase 1 optimizations") was a sprawling 19-file commit that mixed legitimate infrastructure with harmful "optimizations" causing CPU issues. Cristiano requested surgical revert of only the harmful parts.
- **Reverted (9 files):**
  - arena.cpp: Removed extern "C" overflow-handling implementations (contains()-based routing, stdlib fallbacks). Restored original simple static ts_arena_* wrappers inside namespace codetopo.
  - arena.h: Removed `contains()` method and `extern "C"` declarations. Kept forward declarations for set_thread_arena/get_thread_arena/register_arena_allocator (needed by cmd_parse_file.h, cmd_watch.h).
  - arena_pool.h: Removed `try_lease()` and pre-leased ArenaLease constructor.
  - thread_pool.h: Restored original inline worker loop; removed extracted `worker_loop()`, `submit_detached()`, and `stack_size` parameter.
  - cmd_index.cpp: Removed WAL checkpoint overlap thread, restored sequential WAL checkpoint. Simplified arena leasing from try_lease fallback chains to blocking `lease()`. Changed `submit_detached()` to `submit()`. Removed stack_size argument from ThreadPool constructor.
  - extractor.cpp: Reverted language dispatch from `switch(LanguageId)` to `if/else` string chain. Restored simple ostringstream `read_file_content()`.
  - extractor.h: Removed `lang_id_` field. Kept `node_count_`, `timeout_s_`, `cancel_flag_`, `deadline_` fields and extra constructors (required by extractor.cpp timeout logic from df1c1dd).
  - scanner.h: Removed `lang_id` field and `LanguageId` initialization from ScannedFile.
  - parser.h: Removed `set_language(LanguageId)` overload and `get_language_by_id()`.
- **Kept intact:** language_id.h, profiler.h, cmd_parse_file.h, process.h, schema.h, tools.h, CMakeLists.txt, test_resource_limits.cpp.
- **Key discovery:** e1a6762 also fixed 9 pre-existing header declaration mismatches (forward declarations missing for try_lease, submit_detached, node_count_, etc.). These were added by df1c1dd to .cpp files but not to headers. The revert adapted call sites instead of keeping the now-removed declarations.
- **Build:** Clean (MSVC Release). **Tests:** 139/140 (1 pre-existing temp file locking flake in test_mcp_enhancements.cpp).

### DEC-038 Persist Pipeline Overhaul — All 3 Optimizations Implemented (2026-04-01)
- **OPT-1: Cold index skip (1-line)** — Added `persister.enable_cold_index_if_empty()` before begin_batch in cmd_index.cpp:680. The method already existed (DEC-026 R4, lines 101-108 in persister.h). The cold_index_ flag is checked in persist_file() line 127 to skip DELETE on empty files table. Zero-risk wiring.
- **OPT-3: Dedicated persist thread (architectural change, ~120 LOC)** — Created bounded `PersistQueue` class (capacity=512) with mutex/condvar, `PersistItem` struct, and `PersistThreadState` for shared atomics. Persist thread launched after persister creation, owns all SQLite writes. Main thread now: dequeues worker results → pushes to persist_queue → refills workers → displays progress using `persist_state.persisted_count`. Added `profiler.persist_wait` phase to measure main thread backpressure when queue is full. Persist thread: loops on `pop()` → `persist_file()` → `flush_if_needed()` → increments atomic count → writes progress file on commit. Clean shutdown: `persist_queue.close()` → `persist_thread.join()` → main thread continues with WAL checkpoint. Persist thread NEVER touches parsing/tree-sitter/arena (only SQLite). Architecture: Workers → result_queue → Main → PersistQueue → Persist Thread.
- **OPT-2: Multi-row INSERT batching (refs & edges, ~140 LOC)** — Added `stmt_batch_insert_ref_` (80-row, 720 params) and `stmt_batch_insert_edge_` (150-row, 750 params) to Persister. Lazy-prepared on first use via `ensure_batch_ref_stmt()` and `ensure_batch_edge_stmt()`. Symbols remain single-row (need per-row rowid for symbol_ids vector). Refs: split into full 80-row chunks (use batch stmt) + remainder (single-row stmt). Edges: pre-filter valid edges (confidence ≥ 0.3, dst resolved), split into 150-row chunks + remainder. All bindings use `SQLITE_STATIC` (DEC-027). Finalized in destructor alongside other stmts.
- **Files modified:** `cmd_index.cpp` (PersistQueue, persist thread, main loop changes, OPT-1 wiring), `persister.h` (batch stmt fields, prepare helpers, batched INSERT logic in persist_file), `profiler.h` (added persist_wait phase).
- **Test result:** 100/100 tests pass (409 assertions). All TDD tests from Joan at `test_persist_optimizations.cpp` pass (OPT-1 cold index, OPT-2 batch correctness). OPT-3 placeholder tests marked `[!mayfail]` as expected (they test queue behavior, not end-to-end pipeline).
- **Key insight:** Persist thread is fully decoupled from worker threads. Connection created by main thread, passed to Persister, used exclusively by persist thread during drain, then reused by main thread post-join for resolve_references/WAL checkpoint. No concurrent Connection access (join() provides happens-before).
- **Performance implications:** OPT-1 eliminates DELETE overhead on cold index. OPT-2 reduces SQLite call overhead by 80× for refs, 150× for edges (chunked INSERTs). OPT-3 decouples SQLite writes from worker result collection, eliminating persist-induced contention (formerly 35% of wall time per DEC-034).

### WAL checkpoint before post-processing (2026-04-02)
- **Root cause:** With `wal_autocheckpoint=0` and 60+ WAL segments (~1GB), every SELECT in post-processing (idx_read, resolve_refs, fts_rebuild, idx_write) had to search through all WAL segments via hash-table lookups, causing 2.7x-6x regression.
- **Fix:** Added `conn.exec("PRAGMA wal_checkpoint(TRUNCATE)")` in cmd_index.cpp between persist_thread.join() completion and the idx_read phase (line ~828). This merges the WAL back into the main DB file and truncates it, so all subsequent SELECTs read from the clean B-tree.
- **Placement:** After the "Done" summary output, before the "Rebuild read-path indexes" block. The persist thread has already joined (happens-before), so `conn` is safe to use on the main thread.
- **Build:** Clean (MSVC Release). **Tests:** 100/100 pass (409 assertions).

### Otho's resolve_refs 6-fix optimization batch (2026-04-02)
- **R1:** Added `PRAGMA wal_checkpoint(TRUNCATE)` between idx_read and resolve_refs in cmd_index.cpp. Consolidates idx_read's WAL writes so resolve_refs starts clean.
- **R2:** Added `PRAGMA wal_autocheckpoint=1000` after the first TRUNCATE checkpoint (post-persist_thread.join). Re-enables periodic WAL consolidation during post-processing, preventing unbounded WAL growth during resolve_refs' 5M writes.
- **R3 (biggest win):** Replaced single-row INSERT loop in resolve_references() Step 6 with 150-row batch INSERT. Builds multi-row VALUES clauses with 3 bound params per row (src_id, dst_id, kind) and literal `0.7,'name-match'`. Keeps 100K-row COMMIT batching. Uses single-row fallback for remainder. No UNIQUE constraint on edges table, so switched from `INSERT OR IGNORE` to plain `INSERT` (OR IGNORE was a no-op).
- **R4:** Increased `PRAGMA mmap_size` from 2GB to 4GB in connection.h constructor, ensuring full DB file coverage after checkpoint growth.
- **R5:** Added `SELECT count(*) FROM refs` and `SELECT count(*) FROM nodes` cache warming scans after R1 checkpoint, before resolve_refs starts.
- **R6:** Simplified DELETE predicate in resolve_references() from `WHERE kind IN ('calls','includes','inherits') AND evidence = 'name-match'` to `WHERE evidence = 'name-match'`. All resolver edges use name-match evidence, so the kind IN clause was redundant overhead.
- **Files modified:** `src/db/connection.h` (R4), `src/cli/cmd_index.cpp` (R1, R2, R5), `src/index/persister.h` (R3, R6).
- **Build:** Clean (MSVC Release). **Tests:** 100/100 pass (409 assertions).

### DEC-039 Five Optimizations Implemented (2026-04-02)
- **OPT-1: Batch symbol INSERT (20-row)** — Added `stmt_batch_insert_symbol_` with lazy preparation via `ensure_batch_symbol_stmt()`. Symbol loop in `persist_file()` now batches 20 at a time using 13 params/row (260 total, under 999 limit). IDs computed arithmetically from `sqlite3_last_insert_rowid()`: first_id = last_id - (chunk_size - 1). Remainder handled by single-row stmt. Safe because nodes table uses `INTEGER PRIMARY KEY` (no AUTOINCREMENT), we're in a transaction, and no concurrent deletes.
- **OPT-2: Turbo batch 1000→5000 (1-line)** — Changed `(std::max)(effective_batch_size, 1000)` to `5000` in cmd_index.cpp:339. Reduces COMMIT frequency in turbo mode by 5×.
- **OPT-3: Pre-sized file read buffer** — Replaced `ostringstream << f.rdbuf()` in `read_file_content()` (extractor.cpp:787) with `filesystem::file_size()` + pre-allocated `std::string` + `f.read()`. Eliminates O(N log N) buffer doubling. Falls back to ostringstream for zero-size or special files.
- **OPT-4: Remove sqlite3_clear_bindings()** — Removed all 9 `sqlite3_clear_bindings()` calls in persister.h (8 in persist_file on cached stmts, 1 in resolve_references on local batch stmt). All are redundant because every parameter is re-bound (via bind_text/bind_int64/bind_null) before each step().
- **OPT-5: Thread-local parser reuse** — Replaced per-file `Parser parser;` in cmd_index.cpp:469 with `thread_local std::unordered_map<std::string, Parser> t_parsers`. Parser objects reuse `ts_parser_new()` allocations across files of the same language on the same thread. Safe because ThreadPool threads are persistent (joined in destructor), so thread_local lifetime covers the entire index operation.
- **Files modified:** `src/index/persister.h` (OPT-1 batch symbol INSERT + OPT-4 clear_bindings removal), `src/cli/cmd_index.cpp` (OPT-2 turbo batch + OPT-5 thread-local parser), `src/index/extractor.cpp` (OPT-3 file read buffer).
- **Build:** Clean (MSVC Release). **Tests:** 100/100 pass (409 assertions).

### Watchdog timeout redesign (2026-04-02)
- **Context:** Simon's design approved to tighten watchdog timeouts. Old defaults (30s parse, 10s extraction) were far too generous — most files parse in <100ms. Generous timeouts let stuck parsers block slots for 30+ seconds.
- **Change 1 (config.h):** `parse_timeout_s` 30→5, `extraction_timeout_s` 10→5. Both defaults now 5s.
- **Change 2 (cmd_index.cpp):** Watchdog formula: old was +1s per 10KB (coarse, no cap). New is +10ms per KB with 10s hard cap (`std::min`). A 500KB file now gets 5000+5000=10000ms (capped), not 5000+50000=55000ms.
- **Change 3 (cmd_index.cpp):** Kill threshold: 1.5× → 2×. Gives more time for cooperative cancel via `ts_parser_set_cancellation_flag` before hard kill.
- **Change 4 (cmd_index.cpp):** Updated comments near watchdog section: "default 30s" → "default 5s", "+1s per 10 KB" → "+10ms per KB, hard cap 10s", fallback from 30000→5000.
- **Build:** Clean (MSVC Release, exit code 0).

### DEC-040 Arena allocator heap corruption audit (Grag) (2026-04-02)
- **Context:** Warm cache (10,000+ f/s) triggers 7-10 crashes per 100K files with exit codes `0xC0000374` (heap corruption) and `0xC0000791` (stack buffer overrun). Cold cache (500 f/s) has ZERO crashes.
- **ROOT CAUSE IDENTIFIED:** Parser reuse optimization (DEC-039 OPT-5, thread_local parser map) is incompatible with arena switching. Tree-sitter parsers cache internal buffers allocated from arena N, but when thread parses next file with arena M, parser tries to `realloc()` old buffer → reads from arena N (which has been reset or re-leased) → heap corruption.
- **Trigger sequence:** Thread A parses file1 with arena1 → parser allocates internal buffer from arena1 → file1 completes, arena1 reset → thread A parses file2 with arena2 → parser reuses cached state → calls `realloc(ptr_from_arena1, new_size)` with `t_current_arena = arena2` → `arena_realloc` reads header at `ptr_from_arena1 - HEADER_SIZE` → **arena1 memory is invalid** (reset or in use by another thread) → crash.
- **Why only at high throughput:** At 10,000+ f/s, arena lease/return/reset cycle is <1ms. Arena1 is reset and re-leased to thread B before thread A finishes reading from old pointer → concurrent access to same memory → data race.
- **Five issues found:** (1) Parser reuse across arenas (CRITICAL, root cause), (2) Orphaned overflow pointers in realloc (HIGH, memory leak + use-after-free risk), (3) Non-atomic overflow_ptrs_ vector modifications (MEDIUM, exception safety), (4) Integer overflow in alignment calc (LOW, theoretical), (5) Redundant contains() check in arena_realloc (code quality).
- **IMMEDIATE FIX REQUIRED:** Revert DEC-039 OPT-5 parser reuse — change `thread_local std::unordered_map<std::string, Parser> t_parsers;` back to `Parser parser;` (per-file creation) in cmd_index.cpp line 471-472. Risk: ZERO (pure revert). Perf impact: -15 f/s (tolerable vs heap corruption).
- **High-priority fixes:** (1) Add `Arena::remove_overflow_ptr()` and free orphaned pointers in `arena_realloc`, (2) Pre-reserve overflow_ptrs_ vector capacity (128 elements) in Arena constructor.
- **Test plan:** Revert OPT-5, run full benchmark with warm cache, expect ZERO crashes across 162K files. Then apply all fixes, run 10× full index (1.62M files) stress test.
- **Long-term:** If parser reuse is needed, implement "parser-per-arena" architecture where parsers are owned by ArenaPool and reset alongside arenas.
- **Report delivered:** `.squad/decisions/inbox/grag-arena-thread-safety.md` — full audit with exact locations, trigger sequences, and code fixes.

