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

### 2026-04-03: Heap Corruption Concurrency Analysis — 5 Issues Identified

**Investigation scope:** Full concurrency architecture review under high throughput (10K+ files/sec). Context: crashes (0xC0000374, 0xC0000791) only occur when OS file cache is warm (0.4ms read vs 33ms cold).

**Five concurrency issues identified (Critical to Low):**
1. **Arena Reset Race — Use-After-Free in TreeGuard (CRITICAL)** — Arena released back to pool BEFORE TreeGuard's destructor runs. Arena is instantly re-leased to another worker at 10K f/s (3.2ms window). TreeGuard destructs while arena is in use by next worker → heap corruption.
2. **arena_realloc Trusted Header Without Validation (CRITICAL)** — Blindly trusts ArenaAllocHeader size field without magic number or bounds check. If header corrupt (arena reset + reuse), memcpy can overrun → 0xC0000374 or 0xC0000791.
3. **thread_local Arena Pointer Stale After Release (MEDIUM)** — Race window exists where t_current_arena points to reset/reused arena before next `set_thread_arena()` call.
4. **Result Queue Unbounded (MEDIUM)** — Grows to 1K-5K items × 500KB per file = 500MB-2.5GB heap allocation at 10K f/s; amplifies Issue #1 timeline window.
5. **Shutdown Sequence Race (LOW-MEDIUM)** — Persist thread still draining queue when main thread destruction begins; no explicit join-before-destruct ordering enforced.

**Root cause summary:** Issue #1 (Arena Reset Race) + Issue #2 (Trusted Header) combine to cause all crashes. TreeGuard arena dependency outlives the lease scope. At high throughput, race window narrows to milliseconds.

**Key timing insight:** Cold cache (500 f/s) arena reuse interval is ~1024ms; warm cache (10K f/s) is ~3.2ms. Safety margin flips from +1023ms to NEGATIVE.

**Architecture flaw:** Arena MUST outlive TSTree it allocated. Currently: Arena lease scope (lines 423-546 in worker) << TreeGuard lifetime (destructs in main thread ~5-10ms later).

**Recommended fixes:** Extend arena lease to match tree lifetime OR move tree destruction into worker before lease returns. See DEC-038, DEC-039 for full recommendations.

**Deliverables:** `simon-heap-corruption-concurrency.md` (618 lines, 6 issues analyzed with code locations + fixes + testing strategy).

---

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

### 2026-04-03: Heap Corruption Under High Throughput — Critical Concurrency Issue

**Problem:** Warm OS cache (0.4ms file_read vs 33ms cold) → 10K+ files/sec throughput → 7-10 heap corruption crashes per 100K-162K run (exit codes 0xC0000374, 0xC0000791). Cold cache runs (500 files/sec) had ZERO crashes. One crash occurred AFTER completion during shutdown.

**Root Cause Identified:**

**CRITICAL Issue #1 (Arena Reset Race):** Arena is released and reset while TreeGuard still holds TSTree pointers allocated from that arena.

**Timeline:**
1. Worker: ArenaLease destructor → `pool_.release(arena_)` → `arena->reset()` (lines 64-66 arena_pool.h, line 98 arena.h)
2. Arena buffer instantly reused by another worker (pool size = 2× thread count, rapid cycling at 10K/sec)
3. Worker's ParsedFile returned to main thread, moved through result_queue → persist_queue
4. TreeGuard destructs 10-50ms later when ParsedFile is destroyed in persist thread (line 772+ cmd_index.cpp)
5. `ts_tree_delete()` walks corrupted tree structure → heap corruption

**CRITICAL Issue #2 (Untrusted Arena Header):** `arena_realloc` blindly trusts `ArenaAllocHeader.size` with no magic number or bounds check (lines 151-177 arena.h). When arena is reset while tree-sitter still references it, realloc reads corrupt header → `memcpy` overruns → 0xC0000374 (heap corruption) or 0xC0000791 (stack buffer overrun).

**Key architectural insight:** Arena MUST outlive TSTree. Current design:
- Arena lease scope: worker lambda (lines 423-546)
- TreeGuard lifetime: created in worker, destructs in main/persist thread after result processing (~10-50ms later)
- At 500 files/sec (cold cache), 500ms reuse latency — TreeGuard destructs before arena reuse
- At 10K files/sec (warm cache), 0.5-2ms reuse latency — TreeGuard destructs DURING arena reuse by another worker

**5 other issues found:**
- #3: thread_local arena pointer stale after lease (arena.cpp:9, cmd_index.cpp:432) — Medium
- #4: result_queue unbounded (cmd_index.cpp:557-559) — Medium (memory pressure, not direct corruption)
- #5: Shutdown race (persist thread + main thread Connection access, lines 810-832) — explains "crash after completion"
- #6: Large-file arena pool contention (87s at 100K files) — timing amplifier

**Recommended fixes:**
1. **Option A (immediate):** Move ArenaLease + TreeGuard ownership into ParsedFile. Arena destructs AFTER TreeGuard (RAII member order). Requires 546 arenas (thread + window + persist_queue depth) at 128MB = 70GB address space.
2. **Option B (proper architecture):** Copy ExtractionResult in worker, destruct tree before returning. Arena released immediately. No arena pool increase.
3. **Arena header validation:** Add magic number (0xA4EAA4EA) to ArenaAllocHeader, validate in arena_realloc. Return nullptr on corruption → parse fails cleanly instead of heap corruption.
4. **Clear thread_local:** `set_thread_arena(nullptr)` in ArenaLease destructor before release.
5. **Bound result_queue:** Replace std::queue with bounded deque (capacity = window_size + thread_count). Backpressure on workers.
6. **Shutdown error handling:** Wrap persist thread in try-catch, set fatal_error flag. Main thread checks before touching Connection.

**Critical code locations:**
- `src/core/arena.h` lines 151-177: arena_realloc trusted header (no validation)
- `src/core/arena_pool.h` lines 64-66: ArenaLease destructor releases arena
- `src/cli/cmd_index.cpp` lines 423-546: Worker lambda arena lease scope
- `src/cli/cmd_index.cpp` lines 484-517: TreeGuard created and passed to extractor
- `src/cli/cmd_index.cpp` lines 772+: ParsedFile destroyed in main thread (TreeGuard destructs here)
- `src/index/parser.h` lines 104-117: TreeGuard RAII (calls ts_tree_delete in destructor)

**Analysis written to:** `.squad/decisions/inbox/simon-heap-corruption-concurrency.md`

**Implementation priority:**
1. Arena header validation (Fix #2) — 1hr, immediate safety net
2. Extend arena lease into ParsedFile (Fix #1 Option A) — 10min, test hypothesis
3. Stress test with warm cache — confirm crashes stop
4. Proper fix: Copy result + release arena in worker (Fix #1 Option B) — 2hr
5. Defense-in-depth: Fix #3, #4, #5 — 2hr

**Confidence:** 95% that Issue #1 + #2 explain the throughput-dependent crash pattern. Crash probability per file is ~0.005% (7/162K), increases with file size and throughput due to timing window.
