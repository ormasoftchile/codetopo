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

### DEC-034 R2 Follow-up: File node leak on re-persist (Joan's finding)
- **Issue found by Joan during test coverage:** `persist_file()` deletes files with cascade to symbol nodes, but file_nodes (file_id=NULL) survive because NULL doesn't participate in FK cascade.
- **Impact:** Single-file warm re-persist: harmless (ID collision benign, existing tests pass). Batch re-persist: wrong file_node_id can cause edges to reference incorrect src nodes.
- **Root cause:** `INSERT INTO nodes(...stable_key...)` hits UNIQUE constraint silently (unchecked `sqlite3_step` return). `sqlite3_last_insert_rowid()` then returns file record ID, colliding with node IDs from different table.
- **Recommendation:** Add explicit `DELETE FROM nodes WHERE node_type='file' AND name = ?` before file_node INSERT, OR use `INSERT OR REPLACE` for file_node. This also fixes orphaned file_node leak over time.
- **Priority:** Low (current usage unaffected). Consider for DEC-027 stmt caching phase or separate bug fix.

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
