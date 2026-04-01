# Grag — History

## Project Seed

- **Project:** codetopo — MCP server in C++ for indexing symbols/relations across large codebases
- **Stack:** C++20, CMake, vcpkg, SQLite, tree-sitter, MCP protocol
- **User:** Cristiano
- **Key challenge:** Indexer uses detached threads + watchdog for crash recovery, but this introduced STATUS_STACK_BUFFER_OVERRUN crashes. Prior approach (fixed thread pool) was stable but lacked timeout/recovery.

## Core Context (Prior Sessions)

**2026-03-30 — Infrastructure & Performance Fixes:**
- Supervisor exit code classification: 1-2 = non-crash (parse errors, lock failure), negative or >127 = crash (OS exception/signal)
- Init command exit code 1 tolerance: downstream consumers must distinguish "completed with warnings" (code 1) from "hard failure" (codes 2+)
- P0 regression fixes from 700→300 files/s: thread stack 64→8MB (1.15GB→144MB physical), window size +2→4x multiplier
- Deep regression fixes: arena functions re-inlined, extractor DFS stack → thread_local static, deadline check 0xFFFF→0xFFF (4K nodes)
- Slot system reverted to simple futures model (21% code reduction), lightweight watchdog added (~30 lines)
- Parse timeout wiring fix: config.parse_timeout_s was never passed to Parser/Extractor, causing hangs on pathological files
- Extractor deadline check 0xFFFF→0xFFF: 65K→4K node check frequency prevents 10-15s overruns on large-AST files
- SQLite prepared statement caching + SQLITE_STATIC: 6 statements cached, transient→static bindings, 47% throughput gain (278→410 files/s)

**2026-03-31 — Timeout Infrastructure & Profiling:**
- DEC-028 extraction timeout (R1-R4): extraction_timeout_s field, --extract-timeout CLI flag, remove redundant parse timeout, log slow files (>2s)
- Profiling infrastructure: --profile flag + --max-files N truncation for fast iteration, profile_subset.bat harness

## Learnings

### DEC-026 R3+R4 persist pipeline optimization (2026-03-31)
- Implemented batch drain (R3): Main thread dequeues ALL available results from completion queue under single lock instead of one-at-a-time. Reduces lock/condition_variable overhead.
- Implemented cold index flag (R4): `Persister::enable_cold_index_if_empty()` queries files table; when empty, sets flag to skip DELETE step in `persist_file()`. Eliminates no-op deletes with B-tree lookup + FK cascade overhead.
- Benchmark results (fsm, 4145 files): wall 36.2s→20.5s (-43%), persist 13.7s→6.2s (-55%, 3.3→1.5ms/file), throughput 114→200 files/s (+76%).
- Key insight: Contention now healthier at 6.4s; persist no longer dominant bottleneck. Parse+extract workers now constrain pipeline.
- Files modified: `src/index/persister.h` (cold_index_ field, enable_cold_index_if_empty(), conditional DELETE), `src/cli/cmd_index.cpp` (batch drain loop, cold index detection).
- All tests pass: 202/203 (1 pre-existing watchdog flake), Joan's 18 new persist tests validate both optimizations.
- Build: Release clean, 0 errors, 0 warnings.


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

### Build verification after extensive refactoring (2026-03-30)
- Full Release build after team-wide changes across ~20+ files (arena, thread pool, config, connection, schema, extractor, parser, persister, scanner, supervisor, main, process, cmd_index, cmd_init, CMakeLists, CMakePresets, .gitignore).
- CMake configure: clean, no warnings (1.6s).
- MSBuild Release: 0 errors, 0 warnings. All 11 grammar libs + codetopo.exe + codetopo_tests.exe built successfully.
- Test binary sanity: 173 test cases registered, binary runs correctly.
- cmake not on PATH in this environment; found at VS2022 Enterprise bundled path. Not a project issue.

### P0 performance regression fixes (2026-03-30)
- Applied two fixes from Otho's regression analysis (700→300 files/s regression).

### DEC-035: MCP Tool Gap Fixes (2026-04-01)
- **Fix 1 — file_summary node_id chaining (src/mcp/tools.cpp):** Added `id` column to SQL query, included `node_id` in JSON output. Enables LLMs to chain file_summary results directly into symbol_get, context_for, callers_approx without extra roundtrip.
- **Fix 2 — C# extractor edge types (src/index/extractor.cpp):** Added using_directive→include, base_list→inherit, object_creation_expression→call refs. Follows C++/TypeScript precedents; enables file_deps, find_implementations, callers_approx for C#.
- **Fix 3 — source_at tool (src/cli/cmd_mcp.h):** New MCP tool T092 reads 1-based line ranges (max 500), path-validated. Fills gap between "read whole file" and "requires node_id".
- **Test coverage:** Joan wrote 13 tests (72 assertions, all pass immediately). Build clean, 231 tests pass, no regressions.
- **Commit:** 8bd7a92 "Fix MCP tool gaps: file_summary node_id, C# extractor edges, source_at tool"
- **Key insight:** Cold-path MCP changes don't impact indexer performance. New tool surface (source_at) is stateless/read-only with path validation — safe.

### DEC-034: SQLite Turbo PRAGMAs + FK Disable + 2GB mmap (2026-04-01)
- **R1 — Wire turbo PRAGMAs (cmd_index.cpp, connection.h):** Added `conn.enable_turbo()` call after schema ensure with `synchronous=OFF, wal_autocheckpoint=0, temp_store=MEMORY, cache_size=128MB`. `--turbo` flag was plumbed but never invoked.
- **R2 — Disable FK during bulk persist (cmd_index.cpp):** Bracketed drain loop with `PRAGMA foreign_keys=OFF/ON`. FK checks on 3.4M nodes table were expensive at scale; disabled during cold-index bulk load where IDs are generated by SQLite (violations impossible).
- **R3 — Increase mmap_size to 2GB (connection.h):** Changed unconditional `PRAGMA mmap_size=268435456` → `2147483648`. 100K-file DB is ~2GB; 256MB mmap covers only 13%, forcing read() fallback. mmap backed by virtual address space (not physical RAM).
- **Benchmark (105K files, cold index):** Persist degraded 9.1ms/file → fixed to 1.7ms (-81%, 5.3x faster). Total wall 1474s → 511s (-65%). Persist % of wall reduced 62% → 34%.
- **Files modified:** `src/cli/cmd_index.cpp` (turbo call, FK bracket, mmap_size set), `src/db/connection.h` (enable_turbo definition, mmap_size PRAGMA).
- **Build:** Release clean, 218 tests pass (1126 assertions), no regressions.
- **Key insight:** Dead flag (enable_turbo never called) was root cause. R1+R2+R3 together compound — WAL fsync elimination + FK disable + mmap expansion give 5.3x speedup at scale.
- **Fix 1 — Stack size:** `cmd_index.cpp:561` — `ThreadPool` stack changed from 64MB to 8MB. 64MB committed stacks burned 1.15GB physical RAM across 18 threads (flags=0 means commit, not reserve). 8MB matches Linux default; total stack commit drops to ~144MB.
- **Fix 2 — Window size:** `cmd_index.cpp:323` — `window_size` changed from `thread_count + 2` to `thread_count * 4`. The +2 window was too small; workers starved during main thread's ~100ms batch commits. 4x multiplier provides enough buffer to absorb commit pauses.
- Did NOT change largest-first sort order (Cristiano confirmed interleaved scheduling was worse).
- Did NOT change batch sizes.
- Release build: 0 errors, 0 warnings. All 173 tests pass (914 assertions).

### Deep regression fixes — arena re-inline + extractor DFS optimization (2026-03-30)
- Applied two fixes from Otho's deep regression analysis (`otho-deep-regression.md`).
- **Fix 1 — Arena hot path re-inlined:** Moved `ts_arena_malloc`, `ts_arena_calloc`, `ts_arena_realloc`, `ts_arena_free` from `static` functions in `arena.cpp` back to `inline` functions in `arena.h`. Added `extern thread_local Arena* t_current_arena` declaration in header. Arena.cpp now only defines the thread_local, accessors, and `register_arena_allocator`. The 512MB header sanity cap remains intact. System malloc fallback remains intact.
- **Fix 2 — Extractor DFS stack reuse:** Changed `std::vector<Frame> stack` in `extractor.cpp:visit_node` from per-call heap allocation to `thread_local static` with `clear()` at start — memory is allocated once per thread and reused across all 162K files. Also reduced deadline check frequency from every 4096 nodes (`0xFFF` mask) to every 65536 nodes (`0xFFFF` mask), cutting chrono syscalls by 16x.
- Constraints respected: largest-first sort unchanged, 512MB realloc cap kept, iterative DFS kept, system malloc fallback kept.
- Release build: 0 errors, 0 warnings. All 173 tests pass (914 assertions).

### Slot system reverted to simple futures + lightweight watchdog (2026-03-30)
- Reverted `cmd_index.cpp` from SlotState/windowed submission (690 lines) back to simple futures model (545 lines, 21% reduction). Per Simon's architecture decision: the slot system was solving a problem the arena fixes already solved.
- Removed: `SlotState` (8 fields, 6 atomics), `IndexedResult` queue, `try_submit_one()` slot-finding loop, `window_size`/`in_flight` budgeting, complex watchdog with dead-slot revival and generation counters, `submit_detached()` pattern.
- Restored: `pool.submit()` returning `std::future<ParsedFile>`, `futures.reserve(total)`, all tasks pre-queued, sequential `futures[i].get()` consumption. Workers never starve — entire worklist queued up front.
- Added lightweight watchdog: `WatchdogEntry` struct (3 fields), `thread_local int my_slot` via atomic counter for pool-thread-to-entry mapping, scans every 2s, sets `cancel_flag` on stuck parses. ~30 lines vs ~200 lines of slot machinery.
- Preserved all non-slot improvements: arena pool routing, largest-first sort, batch commit + progress files, deferred index rebuild, EdgeTuple in-memory collection, `_resetstkoflw()`, worklist cache, turbo mode.
- Release build: 0 errors, 0 warnings. All 173 tests pass (914 assertions).

### Parse timeout not wired up — indexer hangs on pathological files (2026-03-31)
- Root cause of "indexer gets stuck" bug: `config.parse_timeout_s = 30` existed in config.h but was NEVER passed to `Parser::set_timeout()` or `Extractor` constructor in cmd_index.cpp's worker lambda.
- Without timeout: tree-sitter's `parse()` can spin indefinitely on pathological files. Combined with submit-all-up-front + sequential `futures[i].get()`, one stuck parse hangs the entire indexer (head-of-line blocking).
- ArenaPool is NOT the cause: with N threads and 2N arenas, at most N arenas are ever leased. All 8 return/catch paths properly release arenas via RAII. ArenaLease destructor runs before the future is fulfilled.
- Fix: two lines in cmd_index.cpp lambda — (1) `parser.set_timeout(config.parse_timeout_s * 1000000ULL)` at line ~283, (2) pass `config.parse_timeout_s` to `Extractor` constructor at line ~298.
- Also noted: the "lightweight watchdog" mentioned in the slot-revert history entry is absent from current cmd_index.cpp. Timeout fix resolves the primary hang; watchdog would be defense-in-depth.
- Release build: 0 errors, 0 warnings. All 173 tests pass (914 assertions).

### Extractor deadline enforcement — check frequency 0xFFFF→0xFFF (2026-03-31)
- Root cause of 213 files/s throughput on mixed workloads: parser timeout (set_timeout) works, but extractor's deadline check fires only every 65536 nodes. A file with 500K+ AST nodes gets ~8 checks — each check is after processing huge batches, so the deadline can be 10-15 seconds past by the time it fires.
- Fix: changed deadline check mask from `0xFFFF` (65536 nodes) to `0xFFF` (4096 nodes) in `extractor.cpp` `process_node` lambda. `steady_clock::now()` costs ~15ns on Windows, so at 4096 nodes the overhead is negligible (~60ns per check).
- Also confirmed: `cancel_flag_` is NOT wired up in cmd_index.cpp — Extractor constructor called with only 3 args, 4th (cancel_flag) defaults to nullptr. Deadline is the sole enforcement mechanism and that's sufficient.
- Also confirmed: DFS loop correctly checks `result_->truncated` at the top of the while loop, so truncation does halt the walk.
- Benchmark on Sql\xdb\manifest (55792 files): 395 files/s in 141s, 12 timeouts caught. Extract time 2609us/f. Previously this workload hit 213 files/s because large-AST files were taking 10-15s unchecked.
- Release build: 0 errors, 0 warnings. All 179 tests pass (939 assertions).

### SQLite prepared statement caching + SQLITE_STATIC (2026-03-31)
- `persist_file()` was preparing+finalizing 6 SQL statements per file — 335K redundant round-trips to SQLite's query planner on the 55K-file benchmark. Also used `SQLITE_TRANSIENT` for all string binds, forcing SQLite to malloc+memcpy every string despite the source data outliving each `sqlite3_step()`.
- Fix 1: Added 6 `sqlite3_stmt*` members to `Persister`, lazily prepared on first call, reused via `sqlite3_reset()` + `sqlite3_clear_bindings()`. Destructor finalizes. Class is non-copyable.
- Fix 2: Changed all `SQLITE_TRANSIENT` → `SQLITE_STATIC` in `persist_file()` binds. Safe because all source data (ScannedFile&, ExtractionResult&, content_hash, parse_status, parse_error) lives in scope through `sqlite3_step()`.
- Scope: `persist_file()` only — `resolve_references()` and `prune_deleted()` untouched.
- Benchmark on Sql\xdb\manifest (55792 files): **410 files/s in 136s** (was 278 files/s in 200s). 47% throughput gain, 32% wall time reduction.
- Contention rose from 0.9s/122 stalls to 87.7s/20K stalls — expected: workers now outpace persist more often, proving the queue is still the chokepoint.
- Release build: 0 errors, 0 warnings. All 179 tests pass (939 assertions).

### Profiling harness reveals persist as pipeline bottleneck (2026-03-31)
- Otho implemented `--profile` flag for init and index commands with 17-phase comprehensive profiling report
- Key finding on production init (~162K files): persist dominates at ~49% of total wall time
- Parallel efficiency only ~7% on small repos (82 files) due to per-thread arena overhead
- Per-file persist costs 3.6ms after caching optimizations; single-threaded SQLite persistence is now the dominant chokepoint
- Profile data available to guide next optimization cycle; workers produce at 2,100+ files/s but persist caps pipeline at ~410 files/s (DEC-027)
- Build verified: 0 errors, 0 warnings. All 179 tests pass (939 assertions).

### DEC-028 extraction timeout implementation (2026-03-31)
- Implemented all four DEC-028 recommendations for extraction timeout:
  - **R1 — Extraction timeout:** Added `extraction_timeout_s = 10` to Config, separate from `parse_timeout_s`. New `--extract-timeout` CLI flag on both `index` and `init` subcommands. Supervisor passes it to child process. Extractor constructor now receives the extraction-specific timeout instead of reusing parse_timeout_s.
  - **R2 — Lower default max-file-size:** Changed `max_file_size_kb` default from 10240 (10MB) to 512 (512KB). Updated config.h, main.cpp (both index and init subcommand defaults).
  - **R3 — Remove redundant tree-sitter set_timeout:** Removed `parser.set_timeout()` call from cmd_index.cpp worker lambda. Parse phase is fast (1.8ms/file avg per DEC-028 profiling); extraction timeout (R1) is the real defense against tail-latency. Comment explains rationale.
  - **R4 — Log slow files:** Added `extract_us` field to ParsedFile struct. Worker lambda captures per-file extraction time. Drain loop logs files exceeding 2-second threshold with `[SLOW]` tag, showing extract time, symbol count, and truncation status. Summary line includes slow file count.
- Enabled the blocked test case (Joan's `#if 0` in test_extraction_timeout.cpp → `#if 1`).
- Key files modified: config.h, main.cpp, cmd_index.cpp, cmd_init.h, supervisor.cpp, test_extraction_timeout.cpp.
- Release build: 0 errors, 0 warnings. All 185 tests pass (964 assertions).

### DEC-026 R3+R4: Batch drain + cold index skip DELETE (2026-03-31)
- **R3 — Batch drain:** Replaced single-result dequeue in cmd_index.cpp drain loop with batch drain. Main thread now dequeues ALL available results from the completion queue under a single lock, processes them outside the lock. Reduces lock/unlock cycles and condition variable wait/signal overhead. Pre-reserves drain vector at `min(total, thread_count * 4)` capacity with clear() between iterations.
- **R4 — Cold index skip DELETE:** Added `cold_index_` bool to Persister class with `enable_cold_index_if_empty()` method that queries `SELECT count(*) FROM files`. When empty (cold index), skips `DELETE FROM files WHERE path=?` in persist_file(). On cold index the DELETE is a guaranteed no-op but incurs B-tree lookup + foreign key cascade-check overhead per file.
- Cold index detection runs once before the drain loop starts, after prune_deleted() has run (ensures correct state).
- Windows `min` macro required `(std::min)` parenthesization pattern (MSVC C2589 error).
- Benchmark (fsm subset, 4145 C# files, 3 runs averaged, warm cache):
  - Before: 36.2s wall, 114 files/s, persist 13.7s (3.3ms/file), contention 0.1s
  - After:  20.5s wall, 200 files/s, persist 6.2s (1.5ms/file), contention 6.4s
  - Persist: -55% time (3.3→1.5ms/file), throughput 303→670 files/s (+121%)
  - Overall: -43% wall time, +76% files/s
- Contention increased from 0.1s to 6.4s — expected and healthy: faster persist means the main thread now outruns workers and idles more, proving persist is no longer the dominant bottleneck. Workers (parse+extract) are now the pipeline constraint.
- Key files modified: `src/index/persister.h` (cold_index_ field + methods, conditional DELETE), `src/cli/cmd_index.cpp` (batch drain loop, cold index detection).
- Release build: 0 errors, 0 warnings. 202/203 tests pass (1095/1096 assertions). 1 pre-existing failure in test_watchdog.cpp (process spawn error 87, unrelated).

### Cross-team coordination (2026-03-31)
- Joan's proactive DEC-028 extraction timeout test suite (test_extraction_timeout.cpp, 6 cases) was written before Grag's R1 implementation landed, with test 4 gated via `#if 0` guard pending Config field addition.
- Upon Grag's R1 commit, test 4 automatically unblocked and passed. Orchestration log and session log written.
- Otho's profiling infrastructure (--max-files flag + profile_subset.bat) provides fast iteration harness for future performance tuning.

### Otho SQLite persist scaling analysis ready for implementation (2026-04-01)
- **Otho completed comprehensive scaling analysis** on persist subsystem 6x degradation (1.5ms→9.1ms, 4K→105K files).
- **Critical finding:** `--turbo` performance flag is dead code. `Connection::enable_turbo()` exists in connection.h:69 but is never called anywhere. Indexer runs with suboptimal pragmas during bulk insert.
- **Root cause chain:** At 100K files, default `synchronous=NORMAL` + `wal_autocheckpoint=1000` causes 211 COMMIT operations, each triggering WAL fsync (1-5ms) and potential checkpoint. At 4K: ~8 commits negligible. At 100K: dominant bottleneck.
- **5 root causes identified** (RC-1 through RC-5) explaining 80-100% of observed 7.6ms/file degradation. Combined model validation: 6.8-9.5ms/file (model) vs 9.1ms/file (observed).
- **6 recommendations staged by complexity/priority:** P0-R1 (wire turbo PRAGMAs), P1-R2 (FK off during bulk), P1-R3 (mmap 2GB), P2-R5 (batch 5000), P2-R4 (page_size 8192), P3-R6 (drop stable_key UNIQUE).
- **Expected impact of P0+P1 fixes:** persist 9.1ms → 3.5-4.5ms/file (2-2.5x improvement), full 100K run ~915s → ~370-475s for persist alone, wall time ~1474s → ~930-1030s total (~35% faster), persist drops from 62% to ~30-35% of wall time.
- **Decision proposal:** .squad/decisions/decisions.md with full code locations for Grag, verification plan, and detailed implementation notes.
- **Next step for Grag:** Implement R1, R2, R3 (all low-complexity). Re-profile both fsm and full runs to validate scaling improvement from 6x to ~3-3.5x.

### Otho SQLite scaling R1-R3 implementation (2026-04-01)
- **R1 — Wire turbo PRAGMAs:** Added `conn.enable_turbo()` call in cmd_index.cpp after schema::ensure_schema, gated on `config.turbo`. Prints confirmation message. `enable_turbo()` sets synchronous=OFF, wal_autocheckpoint=0, temp_store=MEMORY, cache_size=128MB.
- **R2 — Disable FK during bulk persist:** Added `PRAGMA foreign_keys=OFF` before drain loop and `PRAGMA foreign_keys=ON` after `commit_batch()`. Same pattern as resolve_references() in persister.h (lines 288/527). Eliminates ~142 FK B-tree probes per file.
- **R3 — Increase mmap_size to 2GB:** Changed Connection constructor mmap_size from 268435456 (256MB) to 2147483648 (2GB). Unconditional — applies to all Connection uses. SQLite only mmaps what exists, so no memory waste on small DBs.
- **Benchmark (full, 105694 files, cold index, turbo):**
  - Wall: 241s indexing phase, 511s total pipeline (scan+index+resolve+rebuild+FTS+checkpoint)
  - Throughput: **438 files/s** indexing phase
  - Persist: **173.3s, 1727µs/file (1.7ms/file)** — down from 9.1ms/file baseline (**5.3x improvement**)
  - Persist share: **34% of wall** — down from 62% baseline
  - Contention: 30.6s / 6019 stalls
  - Otho predicted 3.5-4.5ms/file; actual 1.7ms/file exceeded prediction by 2x
- Key files modified: `src/db/connection.h` (mmap_size), `src/cli/cmd_index.cpp` (turbo call, FK off/on)
- Release build: 0 errors, 0 warnings. 122 test cases pass (524 assertions), excluding pre-existing watchdog/supervisor flakes.

### MCP tool gaps — 3 fixes for tool chaining and C# coverage (2026-04-01)
- **Fix 1 — file_summary node_id:** Added `id` column to file_symbols SQL query and `node_id` field to JSON output. Shifted all column indices by +1. Enables LLMs to chain file_summary → symbol_get/context_for without a separate symbol_search roundtrip.
- **Fix 2 — C# extractor edge types:** Added `using_directive` → include refs, `base_list` → inherit refs, `object_creation_expression` → call refs to `extract_csharp()`. Follows patterns from C++ (`base_class_clause`) and TypeScript (`import_statement`) extractors. `using_directive` uses field-name lookup with fallback to named-child iteration for grammar compatibility.
- **Fix 3 — source_at tool:** New MCP tool reads arbitrary line ranges from source files. Path-validated, capped at 500 lines. Declaration in `tools.h`, implementation in `tools.cpp`, registered in `cmd_mcp.h`. Reuses existing `read_source_snippet()` helper.
- Key files modified: `src/mcp/tools.h`, `src/mcp/tools.cpp`, `src/cli/cmd_mcp.h`, `src/index/extractor.cpp`
- Release build: 0 errors, 0 warnings. All 218 tests pass (1126 assertions).

