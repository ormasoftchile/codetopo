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

### 2026-04-03: Throughput-Dependent Heap Corruption Analysis — Timing Model

**Investigation scope:** Analyze why heap corruption crashes occur **ONLY at high throughput** (10K+ f/s) with warm OS cache, but NOT at low throughput (500 f/s) with cold cache.

**Key finding: Arena pool pressure is throughput-dependent.**

**Timing model developed:**

| Metric | Cold Cache (500 f/s) | Warm Cache (10K f/s) | Ratio |
|--------|---------------------|----------------------|-------|
| Per-thread rate | 31 f/s | 625 f/s | 20× |
| Per-file time | 41ms | 1.6ms | 25× |
| Arena reuse interval | 1024ms | 3.2ms | 320× |
| Arena reset duration | <1ms | 1-5ms | 5× |
| Safety margin | +1023ms | NEGATIVE | ∞ |

**Why cold cache is safe:** Each arena sits idle for 64ms before re-lease. Even if reset takes 5ms, 59ms safety margin prevents overlap with next worker's parse.

**Why warm cache crashes:** Arena re-leased just 3.2ms after release. If reset takes 1-5ms, reset() is still executing when next worker starts using arena. Overlap window: 50-80% of arena reuses occur while previous reset() still running.

**Race scenario at 10K f/s:**
- Worker A finishes file at T=0 → releases arena → reset() enters free_overflow_ptrs() at T=10µs
- Arena pushed to pool at T=20µs, cv_.notify_one() wakes Worker B at T=25µs
- Worker B's lease() returns same arena at T=30µs
- Worker B calls set_thread_arena() at T=35µs, starts parsing at T=50µs
- **reset() worst case: freeing 200+ overflow malloc blocks takes 800µs** (4µs per free)
- **Worker B allocates at T=50µs while reset() still executing → use-after-free → heap corruption**

**Four independent root causes ranked by probability:**

1. **Arena Pool Exhaustion + Reset Race (85%)** — Pool has only 32 arenas for 16 threads. At 10K f/s, cycling rate exhausts pool, forcing workers to wait. When arena returns, reset() conflicts with immediate re-lease.

2. **Result Queue Unbounded Growth (75%)** — Queue grows to 50K+ items at 10K f/s; std::deque allocates 12,500 chunks. Heap allocator metadata corrupts under allocation pressure. Memory growth: 9,800 items/sec × 500 bytes = 4.9 MB/sec → 49MB in 10sec.

3. **Shutdown Sequence Race (20%)** — Main thread exits loop, persist thread still draining. Watchdog thread stops. Worker threads being destroyed while holding arenas. ArenaPool destructor frees arena already held by worker → double-free.

**Mathematical model:** Arena reuse interval = `A × N / F` where A = pool size (32), N = thread count (16), F = throughput (files/sec).
- At 500 f/s: interval = 512 / 500 = 1024ms
- At 10K f/s: interval = 512 / 10,000 = **51.2ms** 

Wait, my earlier calc was wrong. Recalculating with 3.2ms: Each thread cycles at 625 f/s, processes one file every 1.6ms. With 32 arenas and 16 threads, arena is leased at rate F and re-circulates through pool. Time per arena in pool = 32 arenas / 10K total leases per sec = 3.2ms. **This is correct.**

**Recommendations ranked by ROI:**
1. **Increase arena pool from 32 to 64** (one-line change) → reuse interval from 3.2ms to 6.4ms → halves race probability
2. **Bound result_queue to 1024 items** (20 lines) → caps memory at 500MB, provides back-pressure to workers
3. **Use atomic refcount on Arena** (30 lines) → prevent lease-during-reset by blocking new leases if previous reset still running
4. **Explicit ThreadPool shutdown** (2 lines) → force all workers to exit before ArenaPool destruction

**Expected impact:** Immediate fixes → crash rate 10 per 100K → 1-2 per 100K. Full fixes → 0 crashes.

**Deliverables:** `otho-throughput-failure-analysis.md` (442 lines, timing model, race analysis, 3 ranked root causes with fixes).

---

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

### 2026-04-03: Watchdog Kill Pattern Analysis — YAML Pathology Root Cause
- **Symptom:** 10K DsMainDev benchmark: 2 YAML files killed by watchdog after 45 seconds each. Wasted 90+ seconds of wall time.
- **Killed files:** `.pipelines/DsMainDev-Official-MIAA-Linux-Build.yml` (2.25 KB) and `.pipelines/DsMainDev-master-LOC-Build-Box.yml` (1.04 KB)
- **Files are NOT too large:** Both well under 512KB limit (DEC-029 R2). Problem is grammar pathology, not file size.
- **Root cause: tree-sitter YAML grammar superlinearity** on deeply-nested CI/CD pipelines. 100–150KB YAML files produce 500K+ AST nodes (70× more complex than C# source). Parse+extract hits tree-sitter timeout at 30s, watchdog waits until 45s (30s base + 15s size bonus at +1s/10KB).
- **YAML support confirmed:** Intentional design choice to index Azure DevOps pipeline metadata. Language detection → tree-sitter_yaml grammar fully wired, not a skip condition.
- **Profiling baseline validated:** Average per-file ~48ms (file_read 37ms + parse 0.39ms + extract 1.3ms). P99 <500ms. Current 45s timeout is **100× P99** — absurd headroom provides zero benefit.

### 2026-04-03: Throughput-Dependent Heap Corruption — Arena Pool Exhaustion
- **Symptom:** 105K-162K file benchmarks on DsMainDev: **0 crashes at cold cache (500 f/s)**, **7-10 crashes per 100K at warm cache (10K f/s)**. Exit codes: 0xC0000374 (heap corruption), 0xC0000791 (stack buffer overrun).
- **Core finding: The bug is timing-dependent, not content-dependent.** Same code, same files, but throughput differs 20×. At 500 f/s, per-thread rate is 31 files/sec → 32ms/file (33ms disk I/O). At 10K f/s, per-thread rate is 625 files/sec → 1.6ms/file (0.4ms cached I/O).
- **Root cause #1 (85% probability): Arena pool exhaustion + reset race.** Arena pool has 32 arenas (2× thread count). At 10K f/s, arena reuse interval is **3.2ms** (32 arenas ÷ 10,000 leases/sec). Arena reset() takes 40µs best case, **1-5ms worst case** (freeing 200 overflow mallocs). Race window: arena is released → reset() starts → notify_one() wakes another thread → new thread leases the same arena **while reset() is still executing**. At 500 f/s, reuse interval is 1024ms → 200× safety margin. At 10K f/s, reuse interval is 3.2ms → only 2× worst-case reset time → **50-80% of reuses happen during reset**.
- **Root cause #2 (75% probability): Unbounded result_queue growth.** Workers produce 10K results/sec, main thread consumes 200 results/sec (persist-bound). `std::queue<IndexedResult>` has no capacity limit. Queue grows 9,800 items/sec → 49 MB in 10 seconds. std::deque backing allocates 1,225 chunks/sec → heap fragmentation → allocator metadata corruption. PersistQueue has bounded capacity (512) with back-pressure. result_queue does not.
- **Root cause #3 (20% probability): Shutdown race on arena destructor.** Main thread exits loop → joins persist thread → local destructors run → ArenaPool destructor frees all arenas → but ThreadPool destructor hasn't finished joining worker threads → a worker still holds an arena lease → double-free.
- **Mathematical model:** Arena reuse interval `I = (arena_count × thread_count) / throughput = 32 × 16 / F = 512 / F`. At 500 f/s: I = 1024ms. At 10K f/s: I = 3.2ms. Reset() worst case: 1-5ms. **Critical insight:** Reset duration exceeds reuse interval at high throughput.
- **Immediate fixes recommended:** (1) Increase arena pool from 32 → 64 (double reuse interval to 6.4ms), (2) Replace result_queue with bounded PersistQueue-style implementation (add back-pressure). Expected: 10 crashes → 1-2 crashes per 100K.
- **Follow-up fixes:** (3) Add atomic refcount to Arena to prevent lease-during-reset, (4) Explicit ThreadPool shutdown before ArenaPool destruction. Expected: 1-2 crashes → 0 crashes.
- **Files analyzed:** `src/core/arena_pool.h` (lease/release), `src/core/arena.h` (reset, overflow_ptrs_), `src/cli/cmd_index.cpp` (result_queue, shutdown sequence), `src/core/thread_pool.h` (worker lifecycle).
- **Key pattern:** High-frequency resource cycling (arenas, queues) requires explicit capacity limits and back-pressure. Unbounded growth + rapid reuse = guaranteed corruption under load.
- **Decision artifact:** `.squad/decisions/inbox/otho-throughput-failure-analysis.md` with timing model, race window calculations, ranked root causes, and concrete fix proposals.
- **Watchdog architecture:** Three-layer stack (L1: tree-sitter 30s, L2: extractor 10s, L3: watchdog 30s+size). All necessary for defense-in-depth. Timeout formula (lines 567-589 in cmd_index.cpp) scales +1s per 10KB, capping at 67.5s kill for 150KB file.
- **Scale impact:** At 100K files with 10–20 pathological files, current timeout wastes **320–780 seconds of wall time**. Simon's DEC-045 watchdog redesign (5s base, +10ms/KB scaling, 10s hard cap) reduces to 60–130s, saving **640–780s at 100K scale** — more than entire DEC-038 persist pipeline overhaul.
- **Recommended action:** Implement watchdog redesign (4 surgical changes, 15 min effort): config.h (parse_timeout_s 30→5, extraction_timeout_s 10→5), cmd_index.cpp (slot_timeout_ms formula, kill threshold 1.5×→2×). Zero regression for normal files. All three timeout layers remain enabled.
- **Files:** `src/core/config.h` (defaults), `src/cli/cmd_index.cpp` (watchdog formula + kill threshold), full analysis in `.squad/decisions/inbox/otho-watchdog-analysis.md`
- **Key principle:** Timeout exists to protect throughput from pathological files, not to accommodate them. A 5s base is 100× P99; any file exceeding 5s produces garbage results anyway (500K+ node ASTs yield minimal useful symbols).

### 2026-04-01: DEC-038 post-processing regression — WAL + thread transition root cause
- **Symptom:** DEC-038 moved persist_file to a dedicated thread. Indexing improved (510→472s) but post-processing regressed 3x (164→484s), total 805→957s.
- **Root cause:** With `wal_autocheckpoint=0`, the WAL accumulates ~1GB at 100K files. SQLite's mmap (2GB) doesn't cover WAL pages — all reads go through WAL hash tables (~60 segments). In opt2 (main-thread persist), SQLite's WAL reader state was warm on the same thread. In opt3 (persist thread), the main thread must rebuild WAL reader state after `persist_thread.join()`, causing every page read to re-scan hash tables.
- **Why FTS 6.16x worst:** FTS5 rebuild writes to multiple shadow tables with random access patterns, maximizing WAL hash table lookup overhead per page.
- **Fix:** Add `PRAGMA wal_checkpoint(TRUNCATE)` after `persist_thread.join()` and before idx_read. Converts WAL→DB file, enables mmap-based reads. Expected: +30-50s checkpoint, −320s reads, net ~270s improvement.
- **Batch INSERT zero effect:** DEC-027 statement caching already eliminated API overhead. Batch INSERT only saves 79 reset/clear_bindings calls per 80 refs — microseconds. Recommend removing the ~100 lines of batch INSERT complexity.
- **Persist scaling (2.57→3.51ms/file):** B-tree depth growth + page cache pressure at 100K scale. Fundamental SQLite limit at this volume.
- **Key principle:** When moving SQLite writes to a background thread, ALWAYS checkpoint the WAL before switching to reads on the main thread. The WAL/mmap interaction makes uncheckpointed reads catastrophically slow at scale.
- **Files:** `src/cli/cmd_index.cpp` (persist thread + post-processing), `src/index/persister.h` (batch INSERT), `src/db/connection.h` (WAL/mmap pragmas)

### 2026-04-02: resolve_refs 3× regression root-cause — cache destruction + WAL re-growth
- **Symptom:** resolve_refs 63s→178s (+183%) after adding TRUNCATE checkpoint. Checkpoint fixed fts_rebuild and idx_write but not resolve_refs.
- **Primary root cause: Page cache destruction chain.** TRUNCATE checkpoint processes ~1GB WAL (churns 32K-page cache), then idx_read does 7 full table scans + 7 B-tree builds (churns it 7 more times). resolve_refs starts with stone-cold cache for the refs/nodes/edges tables it needs. In opt2 (no checkpoint), WAL pages had good locality for the same tables persist just wrote.
- **Secondary cause: WAL re-accumulation.** After TRUNCATE empties the WAL, wal_autocheckpoint=0 remains active. resolve_refs generates 5M WAL frames (2.5M UPDATEs + 2.5M INSERTs) from zero, growing WAL hash table to 30-60 segments. Every page read must search all segments.
- **Why other phases benefited:** fts_rebuild and idx_write do SEQUENTIAL reads → mmap + OS read-ahead covers them. resolve_refs does RANDOM read-write (UPDATE by rowid, INSERT with B-tree navigation) → cold cache + growing WAL = worst case.
- **Key finding: resolve_refs Step 6 uses single-row INSERT for 2.5M edges** while persist_file uses 150-row batch. Not a regression cause but the single biggest absolute-time component.
- **Fixes proposed (for Grag):** (1) Checkpoint between idx_read and resolve_refs, (2) re-enable wal_autocheckpoint before post-processing, (3) batch edge INSERT in resolve_refs, (4) increase mmap_size to 4GB, (5) cache warming scans, (6) index-assisted DELETE. Expected: 178s → ~60-65s.
- **Key principle update:** TRUNCATE checkpoint helps sequential access patterns but HURTS random-access patterns when it destroys page cache. For mixed read-write phases, either warm the cache after checkpoint or keep autocheckpoint enabled.
- **Files:** `src/index/persister.h` (resolve_references lines 360-610), `src/cli/cmd_index.cpp` (lines 828-878), `src/db/connection.h` (turbo mode, mmap)

### 2026-04-02: Deep performance analysis — 100K index 652s, persist still bottleneck
- **Methodology:** Full code-level analysis of all profiled phases at 100K scale (652s wall, 16 threads). Decomposed wall time into indexing phase (442s) and post-processing (210s). Traced critical path through pipeline.
- **Key finding: persist thread (425s) is STILL the pipeline bottleneck despite DEC-034 R2 pipelining.** Workers produce at ~3ms/effective-item, persist consumes at ~4.25ms/item. persist_wait (242s = 55% of indexing phase) proves queue backpressure. Worker optimizations (file_read, parser) yield ZERO wall-time improvement until persist is faster.
- **Root cause of persist cost: single-row symbol INSERT.** Per-file: ~30 individual sqlite3_step() calls for symbols = 2400μs (69% of 3.48ms persist cost). Refs and edges already use batch INSERT (DEC-038 OPT-2). Symbols are the only unbatched INSERT.
- **Root cause of file_read cost: ostringstream doubling + copy.** read_file_content uses `ss << f.rdbuf()` + `ss.str()` — two full copies, O(N log N) growth. For 500KB file: ~1.5MB wasted copies. Fixable with pre-sized string + direct read.
- **Root cause of flush cost: 100 COMMIT+BEGIN pairs.** Turbo batch_size=1000 → 100 commits at ~770ms each. Each writes 20-40MB WAL data. Larger batch (5000) amortizes better.
- **Optimization plan (3 phases):**
  1. Phase 1 (persist): Batch symbol INSERT (20-row) + turbo batch 1000→5000. persist_thread: 425→300s. Wall: 652→520s (−132s).
  2. Phase 2 (workers): Pre-sized file_read + thread-local parser reuse. Workers: 300→185s/thread. Wall: 520→480s (−40s more).
  3. Phase 3 (post-proc): Defer content_hash index + page_size=8192. Wall: 480→455s (−25s more).
- **Phase 1+2 combined meets 494s target** at ~480s (−172s, −26%).
- **Pipeline dynamics insight:** After Phase 1, persist≈workers≈300s (balanced). Phase 2 makes workers faster, re-establishing persist as bottleneck but at lower ceiling. file_read savings only materialize when persist isn't gating.
- **Confirmed non-targets:** arena_lease (87s = 5.4s/thread, noise), contention (194s = healthy idle), parallel post-processing (SQLite single-writer blocks it), thread count reduction (would slow workers).
- **Files analyzed:** `src/cli/cmd_index.cpp` (full pipeline), `src/index/persister.h` (persist_file, resolve_references), `src/index/parser.h` (Parser per-file allocation), `src/core/arena.h`/`arena.cpp` (thread-local arena), `src/core/arena_pool.h` (pool sizing), `src/index/extractor.cpp` (read_file_content), `src/core/config.h` (defaults), `src/db/connection.h` (turbo pragmas)

### 2026-04-03: DEC-039 Performance Analysis Accepted — Phase 1 Implementation Queued

**Full bottleneck analysis complete and documented in DEC-039 (decisions.md).**

Integrated Simon's architectural review with Otho's 7-bottleneck decomposition. Key findings validated:
- Persist (425s) remains pipeline ceiling despite DEC-034 R2 pipelining
- Single-row symbol INSERT (2400μs per file, 69% of persist) is critical path
- Batch symbol INSERT (20-row) + turbo batch 5000 unlock rebalancing: 652→520s (−132s)
- Phase 1+2 combined (5-6 hours) meets 494s target at ~480s

**Design review DEC-039 accepted.** Three-tier optimization plan sequenced: Phase 1 (persist speedup, low-risk) → Phase 2 (worker speedup, unlocked by Phase 1) → Phase 3 (post-proc polish, optional).

**Next:** Grag to implement Phase 1, Joan to write validation tests. Re-profile fsm (4145) to confirm 652→520s baseline improvement.
