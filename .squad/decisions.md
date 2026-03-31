# Squad Decisions

## Active Decisions

### DEC-001: Architecture review findings (Dijkstra, 2026-03-08)
**Status:** For team awareness  
Priority recommendations: (1) split mega-headers into .h/.cpp, (2) extract CMake grammar management, (3) replace GLOB_RECURSE with explicit source lists, (4) separate process spawning from indexing in supervisor.h. Module boundaries, deps, test structure, arena allocator, PID lock, and path validation are fine as-is.

### DEC-002: Header-only split for mcp/tools and cli/cmd_index (Booch, 2026-03-08)
**Status:** Implemented  
Split `src/mcp/tools.h` (~1360 lines) and `src/cli/cmd_index.h` (~290 lines) into .h + .cpp pairs. `ParsedFile` and Windows SEH types now internal to `cmd_index.cpp`. CMakeLists.txt updated to include new .cpp files.

### DEC-003: Extract process spawning into src/util/process.h (Anders, 2026-03-08)
**Status:** Implemented  
Extracted `get_self_executable_path()` and `spawn_and_wait()` from supervisor.h into `src/util/process.h` + `src/util/process.cpp`. Removes `<windows.h>` leak. Process utilities now reusable.

### DEC-004: Explicit source file lists + grammar macro module (Dijkstra, 2026-03-08)
**Status:** Implemented  
Replaced GLOB_RECURSE with explicit `set()` lists. Extracted grammar boilerplate to `cmake/TreeSitterGrammars.cmake` with `ts_grammars_init()` + `add_ts_grammar()` macros. CMakeLists.txt reduced from ~477 to ~175 lines.

### DEC-005: checkthis.md Query API — enhance, don't duplicate (Dijkstra, 2026-03-08)
**Status:** Approved, implementation in progress  
9 of 10 proposed query APIs already exist. Add 1 new tool (`find_implementations`) and enhance 5 existing tools (`context_for`, `subgraph`, `shortest_path`, `entrypoints`, `callers/callees group_by`). Do NOT create duplicate tools with different names.

### DEC-006: Index freshness engine-side implementation (Anders, 2026-03-09)
**Status:** Implemented, build verified
Implemented R1 (git.h + persister metadata), R6 (QueryCache::clear), R7 (rehab_quarantine), R9-config (FreshnessPolicy enum + debounce_ms). All header-only. schema.h → index/scanner.h dependency accepted per proposal spec for ScannedFile type in rehab_quarantine.

### DEC-007: Protocol-side index freshness — StalenessState + _meta injection (Booch, 2026-03-09)
**Status:** Implemented, build verified
StalenessState is mutex-free (single-threaded stdio loop). Stale notifications injected as `_meta` in tool responses (MCP-spec-compatible). ReindexState uses std::atomic + detached thread with running/queued deduplication. --freshness/--debounce CLI flags plumbed through. debounce_ms accepted but unused until P2 watch mode.

### DEC-008: Freshness test conventions (Lambert, 2026-03-09)
**Status:** Established
Tests use inner-scoped Connection objects + cleanup() helper to avoid WAL file locking on Windows. All DB tests using temp directories must follow this pattern.

### DEC-009: BranchSwitch watcher design — filesystem-only detection (Anders, 2026-03-09)
**Status:** Implemented, build verified
`FileEvent::BranchSwitch` added to watcher.h. Detection is purely filesystem-based (path pattern matching on `.git/HEAD` and `.git/refs/`). Watcher does NOT import `git.h` — semantic interpretation belongs to the callback consumer. Quarantine rehab wired into cmd_index.cpp: after change detection and before pruning, compares stored `git_head` with current HEAD; on branch switch calls `rehab_quarantine()` and refreshes the in-memory quarantine set.

### DEC-010: --watch MCP integration — atomic bool cross-thread bridge (Booch, 2026-03-09)
**Status:** Implemented, build verified
`--watch` CLI flag on the `mcp` subcommand embeds the filesystem watcher into the MCP server. Watcher triggers `ReindexState` child processes; on completion sets `std::atomic<bool>` that the main stdio loop checks before each tool dispatch to clear QueryCache. No mutexes — cross-thread communication entirely via atomics. Watcher lifecycle tied to `server.run()`.

### DEC-011: Coordinator must never implement — delegate all work (Cristiano, 2026-03-30)
**Status:** Standing directive  
The coordinator must NEVER write code, edit files, or do implementation work directly. ALL work — code, fixes, analysis, debugging, testing — must be routed to the appropriate team member (Simon, Grag, Otho, Joan). No exceptions. User directive after coordinator violated this rule by implementing arena routing, thread pool, and crash fixes directly instead of spawning agents.

### DEC-012: Init command tolerates exit code 1 — parse errors non-fatal (Grag, 2026-03-30)
**Status:** Implemented  
`cmd_init.h::run_init()` now treats exit code 1 (parse errors) as a warning, not a fatal abort. Only exit codes > 1 (e.g. schema mismatch=3) abort. This allows `codetopo init` on large repos with unavoidable parse errors to still write MCP editor configs. Root cause chain: `cmd_index.cpp` returns 1 on parse errors → supervisor correctly classifies as non-crash → `cmd_init.h` previously treated any non-zero as fatal.

### DEC-013: Supervisor exit code classification (Grag, 2026-03-30)
**Status:** Implemented, build verified, all tests pass  
Exit codes classified into categories: success (0), schema mismatch (3, no retry), normal error (1-2, log warning, propagate, NO crash recovery), actual crash (negative or >127, full crash recovery). Added `is_crash_exit_code()` helper returning `true` for `code < 0 || code > 127`. Prevents supervisor from doing a full cold restart (re-scan 162K files, re-resolve 2.87M edges) when the child simply had parse errors.

### DEC-014: Deferred index rebuild for resolve_references (Otho, 2026-03-30)
**Status:** Proposed (implemented, pending team review)  
Split index rebuild into read-path (before resolver) and write-path (after resolver) phases. Disable FK checks during resolve. Delete stale cross-ref edges before re-insert. Merge Step 1+4 into single table scan. Increase batch commit 10K→100K. Expected 761s→~100-150s (5-7x). Trade-off: write-path indexes temporarily absent during resolve; acceptable given existing crash recovery.

### DEC-015: Eliminate SQL join in resolve_references Step 6 (Otho, 2026-03-30)
**Status:** Implemented, build + tests verified  
Replaced 3-way SQL join (`refs × files × nodes`) in Step 6 with in-memory edge collection during Step 5's resolution loop. Edge tuples collected in a vector and batch-inserted via prepared statement. Eliminates the dominant cost center (~80% of total time). 761s→single-digit seconds for edge creation. All 173 tests pass (914 assertions).

### DEC-016: Root cause analysis — 0xC0000409 crash on large codebases (Simon, 2026-03-30)
**Status:** Diagnosis complete, fixes pending  
Ranked root causes for STATUS_STACK_BUFFER_OVERRUN during 162K-file indexing: (1) HIGH 70% — Arena realloc with corrupt header → memcpy overrun in `arena.h`; fix: validate header, add magic number corruption detection. (2) MEDIUM 20% — SEH translator throwing C++ exception on stack overflow in `cmd_index.cpp`; fix: quick-exit instead of throw for EXCEPTION_STACK_OVERFLOW. (3) LOW 8% — Tree-sitter grammar external scanner buffer overflow; fix: audit scanners, update grammar deps. (4) LOW 2% — Supervisor quarantine window misalignment (amplifier, not root cause).

### DEC-017: Slot system vs. simple futures — revert to futures + lightweight watchdog (Simon, 2026-03-30)
**Status:** Decision made, implemented by Grag (DEC-024)  
Discarded the SlotState/windowed submission system (690 lines) in favor of the committed simple futures model (339 lines) with a lightweight watchdog (~30 lines). The crash that motivated the slot design was root-caused in DEC-016 to arena header corruption and SEH mishandling — not thread management. Watchdog uses per-thread `atomic<int64_t>` timestamps + `cancel_flag` vectors; no slots, generations, or result queues. Keeps `_resetstkoflw()`, `parse_timeout_s`, quarantine rehab, and dual arena pool routing.

### DEC-018: No interleaved file scheduling — largest-first sort must stay (Cristiano, 2026-03-30)
**Status:** Standing directive  
Interleaved file scheduling (mixing large and small files) was already tried and caused major slowness. The largest-first sort must stay. Do not propose interleaved scheduling as a fix.

### DEC-019: Performance regression analysis — 5 root causes ranked (Otho, 2026-03-30)
**Status:** Analysis complete, fixes implemented (DEC-021, DEC-022)  
Regression from 700→300 files/s. Ranked causes: RC-1 (HIGH) 64MB committed thread stacks — 1.15GB for stacks alone; RC-2 (HIGH) largest-first sort + small window causes initial stall; RC-3 (MEDIUM) windowed submission starves workers during batch commits; RC-4 (MEDIUM-LOW) system malloc fallback memory leak in arena; RC-5 (LOW) non-inlined ts_arena_realloc. Combined fix expected to recover to 530-710 files/s.

### DEC-020: Deep regression analysis — arena de-inline + extractor DFS are dominant costs (Otho, 2026-03-30)
**Status:** Analysis complete, fixes implemented (DEC-022)  
Previous analysis targeted wrong bottlenecks (stack/window). Actual 2x regression from two per-file hot path changes: RC-A (40-60%) arena allocator de-inlined — hundreds of millions of calls with function-call overhead; RC-B (30-50%) iterative DFS creates std::vector per file + chrono syscalls every 4096 nodes. Combined fix expected 286→450-600 files/s.

### DEC-021: P0 fixes — thread stack 64→8MB, window thread_count+2 → thread_count*4 (Grag, 2026-03-30)
**Status:** Implemented, build + tests verified  
Stack: `_beginthreadex` with `flags=0` commits full size as physical RAM. 18×64MB=1.15GB → 18×8MB=144MB. Window: old +2 meant workers starved during batch SQLite commits; 4x multiplier provides buffer. Largest-first sort unchanged per DEC-018.

### DEC-022: Deep fixes — arena re-inline + extractor DFS stack reuse (Grag, 2026-03-30)
**Status:** Implemented, build + tests verified  
Re-inlined `ts_arena_malloc/calloc/realloc/free` from arena.cpp back to arena.h. Extractor DFS stack changed to `thread_local static` (zero allocation after first file). Deadline check mask increased from 0xFFF to 0xFFFF (16x fewer steady_clock::now() syscalls). Expected +140-280 files/s.

### DEC-023: Parse timeout wired up — fixes indexer hang on pathological files (Grag, 2026-03-31)
**Status:** Implemented, build verified, all tests pass  
Parser timeout and extractor timeout infrastructure existed but were never called in cmd_index.cpp worker lambda. Without timeout, tree-sitter on pathological files blocks forever; with futures[i].get() sequential consumption, this causes head-of-line blocking — apparent hang. Two-line fix: call `parser.set_timeout()` and pass `config.parse_timeout_s` to Extractor constructor.

### DEC-024: Slot system reverted to simple futures + lightweight watchdog (Grag, 2026-03-30)
**Status:** Implemented, build verified, all tests pass  
Reverted cmd_index.cpp from SlotState/windowed submission (690 lines) to simple futures (545 lines, 21% reduction). Removed: SlotState struct, IndexedResult queue, try_submit_one(), window budgeting, complex watchdog. Restored: ThreadPool pool(thread_count) + futures.reserve(total) + sequential futures[i].get(). Added: WatchdogEntry struct (~30 lines) with thread_local slot, atomic timestamps, cancel flags. All prior arena/sort/batch/deferred-index work preserved.

### DEC-025: Extractor deadline check frequency 0xFFFF→0xFFF (Grag, 2026-03-31)
**Status:** Implemented, build + tests + benchmark verified  
Extractor's deadline check was firing every 65536 nodes (0xFFFF mask), meaning large-AST files with 500K+ nodes only checked ~8 times — the deadline could be exceeded by 10-15 seconds before detection, tanking throughput to 213 files/s. Changed to every 4096 nodes (0xFFF mask). `steady_clock::now()` costs ~15ns so overhead is negligible (~60ns per 4096-node batch). `cancel_flag_` confirmed not wired up from cmd_index.cpp — deadline is sole enforcement and sufficient. DFS truncation correctly halts the walk via `result_->truncated` check. Benchmark: 55792 files at 395 files/s (up from 213), 12 timeouts caught, extract time 2609us/f. All 179 tests pass (939 assertions).

### DEC-026: Remaining throughput gap analysis — 278 vs 829 files/s (Otho, 2026-03-31)
**Status:** Analysis complete, recommendations pending implementation  
Workers (16 threads) produce at 2,100+ files/s but single-threaded SQLite persist caps pipeline at 278 files/s. Per-file persist costs 3.6ms (5 re-prepared SQL statements, SQLITE_TRANSIENT string copies, DELETE-cascade on cold index). Recommendations ranked: R1 cache prepared statements + R2 SQLITE_STATIC (quick wins, +25-40%), R3 pipelined drain (moderate, +10-20%), R4 skip DELETE on cold index, R7 parallel WAL persist (nuclear option for >500 files/s). Worker optimization is exhausted — next gains must come from persist side.

### DEC-027: SQLite prepared statement caching + SQLITE_STATIC (Grag, 2026-03-31)
**Status:** Implemented and verified  
Cached 6 prepared statements in Persister (lazily prepared, reused via `sqlite3_reset()` + `sqlite3_clear_bindings()`, finalized in destructor). Switched all `persist_file()` string binds from SQLITE_TRANSIENT to SQLITE_STATIC (source data lives through `sqlite3_step()`). Class now non-copyable/non-movable for RAII. Benchmark: 200s→136s wall time (-32%), 278→410 files/s (+47%). Contention spike from 0.9s→87.7s is expected — workers now outpace persist more, proving persist was the bottleneck. All 179 tests pass (939 assertions).

### DEC-025: Thread pool sizing error — 72 threads created instead of 18 (Otho, 2026-03-30)
**Status:** Analysis complete, actionable  
ThreadPool constructed with `window_size` (72) instead of `thread_count` (18). Creates 4x oversubscription: 72 OS threads on 18 cores, 576MB committed stacks, 32 threads permanently blocked on arena pool. Context switching destroys cache locality for tree-sitter's memory-intensive parsing. One-line fix: `ThreadPool worker_pool(thread_count, 8*1024*1024)`. Also recommends `STACK_SIZE_PARAM_IS_A_RESERVATION` flag in `_beginthreadex` to reserve rather than commit stack memory.

### DEC-026: Edges table — add UNIQUE constraint, defer with write-path indexes (Otho, 2026-03-30)
**Status:** Proposed  
`INSERT OR IGNORE INTO edges` has no UNIQUE constraint to ignore — it's a no-op. Recommended: add `UNIQUE INDEX idx_edges_unique ON edges(src_id, dst_id, kind)` but include it in `drop_bulk_indexes()`/deferred write-path rebuild per DEC-014 pattern. Best correctness with minimal bulk-insert cost.

### DEC-027: Parse regression root cause — surgical per-file analysis (Otho, 2026-03-31)
**Status:** Analysis complete  
Examined 6 changed files line-by-line. Arena fast-path adds ~20µs/file (extra branch). Extractor iterative DFS adds ~300-800µs/file (TLS vector overhead + potential lambda non-inlining). Tree-sitter timeout polling adds ~15µs/file. Total from 6 files: ~0.3-0.8ms/file — explains only ~25% of the 3.3ms/file regression. Remaining 75% suspected in cmd_index.cpp changes (batch sizing, config plumbing) or I-cache pollution. Key finding: `inline` keyword provides ZERO benefit for arena functions called through function pointers (`ts_set_allocator`).

### DEC-028: Profile results — extract phase is the regression killer (Otho, 2026-03-31)
**Status:** Actionable findings  
Per-phase profiling on 4145 C# files. With max-file-size 256KB: 829 files/s, zero contention — engine is fine for normal files. With max-file-size 1024KB: 45 files/s — extract blew up 13x (7.4s→98.7s thread-time) from unbounded extraction on large-AST files. Regression is tail-latency from ~0.1% pathological files. R1: add extraction timeout (highest priority). R2: lower default max-file-size to 512KB. R3: remove redundant tree-sitter set_timeout(30s). R4: log slow files.

## Governance

- All meaningful changes require team consensus
- Document architectural decisions here
- Keep history focused on work, decisions focused on direction
