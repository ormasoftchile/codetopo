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
