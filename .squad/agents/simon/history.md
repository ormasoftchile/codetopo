# Simon — History

## Project Seed

- **Project:** codetopo — MCP server in C++ for indexing symbols/relations across large codebases
- **Stack:** C++20, CMake, vcpkg, SQLite, tree-sitter, MCP protocol
- **User:** Cristiano
- **Key challenge:** Must handle codebases with 100k+ files reliably; current indexer crashes with STATUS_STACK_BUFFER_OVERRUN (0xC0000409) on large repos

## Prior Decisions (inherited)

- DEC-001: Split mega-headers into .h/.cpp pairs
- DEC-002: Header split implemented for mcp/tools and cli/cmd_index
- DEC-003: Process spawning extracted to src/util/process.h
- DEC-004: Explicit CMake source lists + grammar macro module

## Learnings

### 2026-03-30: 0xC0000409 Crash Root Cause Analysis

**Architecture of the indexer pipeline:**
- `run_index()` in `src/cli/cmd_index.cpp` is the main entry point
- Uses detached `_beginthreadex` threads on Windows with 8MB stacks
- SlotState struct with generation counter, cancel_flag, dead flag for watchdog
- ArenaPool provides thread-local bump allocators for tree-sitter (128MB normal, 1024MB large)
- `make_parse_task` lambda: leases arena → sets thread_local → parse → extract → return result
- Supervisor in `src/index/supervisor.cpp` restarts crashed children, quarantines in-flight files

**Key findings:**
1. `arena_realloc` in `arena.h` trusts ArenaAllocHeader blindly — no magic number, no bounds check on old_size. If header is corrupt (arena reset + reuse after exhaustion), memcpy can overrun
2. SEH translator (`seh_translator` in cmd_index.cpp) throws C++ exception on ALL SEH codes including EXCEPTION_STACK_OVERFLOW — this is UB and corrupts the GS cookie
3. `visit_node` recursion capped at depth 500, ~200KB stack usage max — NOT the crash source
4. Thread-local arena model is sound — ArenaLease RAII scoping is correct
5. Supervisor quarantine window in single-thread mode is too wide (thread_count+2 instead of 1)

**File sizes that trigger crash:** Files sorted largest-first, crashes before file 2000. DsMainDev has massive auto-generated C# files that are the first to be processed.

**Critical code locations:**
- `src/core/arena.h` lines 104-118: arena_realloc with trusted header
- `src/cli/cmd_index.cpp` lines 43-47: seh_translator UB
- `src/cli/cmd_index.cpp` lines 49-53: vectored_handler no-op for stack overflow
- `src/index/extractor.cpp` line 210: visit_node depth guard (safe, max 500)
- `src/index/supervisor.cpp` lines 218-229: quarantine window (too wide in single-thread)
- `src/core/config.h` line 25: max_ast_depth = 500

### 2026-04-02: Forward-Looking Optimization Analysis

**Current Performance State:**
- Indexing speed: ~207 files/s on 4145 C# files (20s wall time)
- Worker time breakdown: Parse 46% (9ms/file), Extract 28% (6ms/file), File read 26% (5ms/file)
- Infrastructure: Thread pool sound (window-based, 18 concurrent slots), arena allocation efficient, persist pipelined correctly
- No structural bottlenecks; remaining gains in micro-optimization

**Hot Path Analysis:**

1. **Parser Configuration:**
   - Parser objects created per-file (4145 allocations) instead of reused
   - Language loading via FFI (tree_sitter_cpp(), tree_sitter_python(), etc.) called per-file
   - Opportunity: Parser pooling → 5–10% speedup

2. **File Read (5ms/512KB):**
   - Current: std::ostringstream << f.rdbuf() allocates 2–3 times + copies
   - Opportunity: Pre-allocate string to exact size → 40% faster (5ms → 3ms)

3. **Extractor (6ms/file):**
   - Language dispatch in hot loop uses string comparisons (~100K per large file)
   - Node type allocations repeated unnecessarily
   - Opportunity 1: Switch statement dispatch → 10–15% faster
   - Opportunity 2: String cache for node types → 3–5% faster

4. **Parse Phase (9ms/file):**
   - Tree-sitter library overhead ~6ms (grammar + incremental parsing)
   - Arena allocation during parse ~1.5ms
   - Language dispatch + callbacks ~1.5ms
   - Assessment: Near-irreducible; parser pooling may unlock 0.5–1ms

5. **Resolve Phase (10–20s post-index):**
   - Single-threaded name resolution of 500K refs
   - Not on critical path (runs after all files indexed)
   - Opportunity: Parallel resolve by file_id → 8–15s savings offline

**Five Concrete Opportunities (Ranked by ROI):**
1. Pre-allocated file read (30 min, 40% faster I/O) — QUICK WIN
2. Parser pooling (2 hr, 5–10% overall) — SOLID WIN
3. Language dispatch switch (1 hr, 10–15% faster extract) — SOLID WIN
4. String cache for node types (1.5 hr, 3–5% faster extract) — DIMINISHING RETURNS
5. Parallel resolve phase (4 hr, 10–20s offline savings) — ADVANCED

**Combined Potential:** 207 files/s → 280–320 files/s (+35–55%) with ~8.5 hours effort

**Key Decisions to Lock In:**
- DEC-035: Parser reuse pattern (thread-local per-language, no locks)
- DEC-036: Language dispatch via pre-computed enum + switch (no string comparisons in hot loop)
- DEC-037: File read buffering (exact-size pre-allocation, no exponential growth)

**Recommendations:**
- Phase 1 (next sprint): #1, #3, #2 → 260–280 files/s, ~3.5 hours
- Phase 2 (following sprint): #4, prepare #5 → 285–300 files/s, +4.5 hours
- Deprioritize: Lazy resolve (architectural shift), parallel WAL persist (unnecessary after DEC-034 R2), vectored I/O (marginal gain)

**Scaling Considerations (50k+ files):**
- Arena pools already handle large-file fallback
- Result queue contention negligible even at 50 threads
- Resolve phase becomes offline bottleneck; parallel resolve (#5) high-ROI at scale

### 2026-04-02: Persist Pipeline Overhaul Design (DEC-038)

**Designed 3-phase optimization targeting 351s persist + 95s contention at 100K files:**

1. **OPT-1 (Cold index skip):** `enable_cold_index_if_empty()` exists but was never wired up in cmd_index.cpp. One-line fix. Very low risk.

2. **OPT-2 (Multi-row INSERT):** Batch refs (80 rows/chunk) and edges (150 rows/chunk) into multi-row INSERT. Symbols stay single-row because `last_insert_rowid` is needed per symbol for ref/edge binding. Refs + edges are ~85% of INSERT volume.

3. **OPT-3 (Persist thread):** Bounded `PersistQueue` (capacity 512, mutex+2 CVs, backpressure on full). Persist thread owns full transaction lifecycle (begin_batch → persist_file → flush_if_needed → commit_batch). Main thread reduces to: dequeue → push → refill → display progress via `atomic<int> persisted_count`. Previous DEC-034 attempt crashed from ParserPool (thread_local heap corruption), not from persist thread itself — persist thread is parser-free, safe to retry.

**Key architectural decisions:**
- Queue bounded at 512 to cap memory (~5-10MB) and prevent runaway growth
- Connection ownership transfers via join() happens-before guarantee
- Progress file writes move to persist thread (after each COMMIT)
- profiler.persist removed from main thread; replaced by persist_wait (backpressure metric)
- Fatal DB error → atomic flag → main thread aborts gracefully

**Ordering:** OPT-1 → OPT-3 → OPT-2 (validate profiling → decouple pipeline → optimize internals).

### 2026-03-07: WAL Checkpoint Architecture Review (DEC-038 Post-Analysis)

**Regression observed:** Post-processing phases (idx_read, resolve_refs, idx_write, fts_rebuild) regressed 2.7x–6x at 100K despite unchanged code after DEC-038 OPT-3 implementation.

**Root cause identified:** Missing WAL checkpoint between persist completion and post-processing start.

**Key findings:**
1. **WAL bloat during persist:** 35M row insertions (100K files × ~350 INSERTs/file) → ~100 COMMIT transactions → 500MB–2GB WAL file with no checkpoint
2. **Persist thread correctly transfers Connection:** `in_batch_` flag properly managed (false after commit_batch), transaction state sound, join() happens-before guarantees ownership transfer
3. **PRAGMA persistence confirmed:** All per-connection settings (journal_mode=WAL, synchronous=NORMAL/OFF, wal_autocheckpoint=0 in turbo) remain in effect across thread boundary
4. **Lock contention mechanism:** Post-processing queries (idx_read CREATE INDEX, resolve_refs scanning + updates) now contend with massive WAL frame eviction and lock incompatibilities on table metadata writes

**Solution:** Add explicit `conn.wal_checkpoint()` between `persist_thread.join()` (line 808) and idx_read start (line 831) in cmd_index.cpp. This consolidates WAL frames into database file, frees memory, removes lock contention.

**Expected outcome:** 3–5x speedup on post-processing phases (back to pre-OPT-3 baseline), with persist phase flat or +2–3% overhead.

**Architectural validation:**
- DEC-038 persist thread design is sound — no thread safety or transaction state issues
- Problem is not in persist pipeline but in missing pipeline transition ritual (checkpoint)
- Post-DEC-038, WAL discipline must be explicit (was previously implicit in main-thread persist loop)

**Decision:** Fix is one-line code addition; no architectural rework needed. Defer deeper optimization (incremental checkpoint during persist) to DEC-039 if further tuning desired

### 2026-04-03: Full Index Pipeline Architecture Review (DEC-039)

**Reviewed entire run_index() pipeline end-to-end at 100K file scale (652s wall, 16 threads).**

**Key architectural findings:**

1. **Persist throughput is the pipeline ceiling.** 242s (37%) of main-thread backpressure proves persist can't keep up with 16 workers. Theoretical worker throughput is ~333 files/s but actual is 153 files/s — pipeline efficiency is only 54%. The single-row symbol INSERT (forced by `last_insert_rowid` dependency) is the dominant persist-phase cost.

2. **Main-thread relay adds unnecessary latency.** Data path is worker → result_queue → main → persist_queue → persist. Main thread only relays data + manages slots. Batch draining from result_queue (pop ALL ready results in one lock) is a quick win. Eliminating the relay entirely is a longer-term architectural option.

3. **Arena pool contention (87s/13.4%) traced to large-file pool.** Normal pool has 32 arenas for 18 workers (adequate). Large-file pool has only max(2, thread_count/4) = 4 arenas. When multiple large files queue, workers block.

4. **Post-processing is 210s (32%) all sequential.** idx_read (82s, 7 indexes), resolve_refs (72s), idx_write (27s), fts_rebuild (29s). Index audit may reveal redundant indexes. Resolve is well-optimized from DEC-015 but could benefit from incremental resolution on warm re-index.

5. **File read (35.3ms/file) still uses ostringstream** — DEC-037 #1 pre-allocated buffer not yet implemented. Largest single-phase cost.

6. **ThreadPool lost custom stack sizes** when slot system was replaced (DEC-024). Current std::thread uses default 1MB stacks. Not crash-critical (iterative DFS) but inconsistent with DEC-021 intent.

7. **Parser created per file** — 100K ts_parser_new/delete cycles. Thread-local pooling by language would eliminate this.

**Actionable optimization tiers identified:**
- Tier 1 (80–120s, low risk): Pre-allocated read buffer, batch drain, remove clear_bindings, parser pooling, language enum dispatch
- Tier 2 (50–80s, moderate risk): Batch symbol INSERT with ID arithmetic, index audit, larger persist batches
- Tier 3 (40–60s, architectural): Eliminate main-thread relay, lock-free arena distribution, incremental resolve

**Review written to:** `.squad/decisions/inbox/simon-index-design-review.md`. Integrated with Otho analysis into DEC-039 (decisions.md). Design accepted. Phase 1 implementation (batch symbol INSERT, turbo batch 5000) proceeding to Grag.

### 2026-04-03: Watchdog Timeout Redesign

**Problem:** 30s base watchdog timeout (effective 45s for 150KB files with +1s/10KB scaling) wastes massive wall time when pathological files are killed. At 10K scale, 2 YAML pipeline files hit the watchdog — each wasting ~45s of slot time. At 100K with 10-20 kills, this wastes 450-900s.

**Key findings:**
1. Three-layer timeout stack (tree-sitter parse, extractor deadline, watchdog) is architecturally correct but values are 600× the average processing time (48ms avg vs 30s timeout)
2. Killed YAML pipeline files are pathological for tree-sitter's YAML grammar (deeply nested CI/CD templates producing 500K+ AST nodes), not a file-size issue
3. Watchdog kill at 1.5× timeout leaves zombie threads in ThreadPool, reducing effective parallelism
4. The `--max-file-size 512` flag doesn't protect against grammar pathology on sub-512KB files

**Recommended strategy:**
- `parse_timeout_s`: 30 → 5s (still 100× P99)
- `extraction_timeout_s`: 10 → 5s
- Watchdog per-KB scaling: +1s/10KB → +10ms/KB
- Hard cap: 10s absolute maximum regardless of file size
- Kill threshold: 1.5× → 2× (more cooperative cancel time, max 20s)
- Keep all three timeout layers (complementary, not redundant)

**Impact:** 4 surgical code changes (~15 min effort). Saves 350-800s of wasted wall time at 100K scale — potentially more than the entire persist pipeline overhaul (DEC-038).

**Design written to:** `.squad/decisions/inbox/simon-watchdog-redesign.md`
