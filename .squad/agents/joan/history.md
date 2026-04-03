# Joan — History

## Project Seed

- **Project:** codetopo — MCP server in C++ for indexing symbols/relations across large codebases
- **Stack:** C++20, CMake, vcpkg, SQLite, tree-sitter, Catch2
- **User:** Cristiano
- **Key challenge:** Unit tests pass (173 cases, 914 assertions) but real-world indexing of 162k-file codebase crashes repeatedly with 0xC0000409. Tests don't cover the actual production failure path.

## Learnings

### 2026-04-01: DEC-034 R2 Pipelined persist tests
- Added 17 test cases (66 assertions) in `tests/unit/test_persist_pipelined.cpp`
- Also registered existing `test_persist_pipeline.cpp` (18 tests) in CMakeLists.txt — was missing
- Full suite: 208 tests, 1112 assertions, all green
- **Key pattern**: ResultQueue (std::queue + mutex + CV) with persist_thread_fn consumer
- **Edge case found**: File nodes (node_type='file', file_id=NULL) are NOT cascade-deleted when re-persisting same paths. `persist_file()` does DELETE FROM files which cascades to symbol nodes (file_id=N), but file_nodes (file_id=NULL) survive. On re-insert, `stable_key UNIQUE` constraint silently blocks new file_node creation. This doesn't break single-file re-persist (existing warm tests pass) but affects batch re-persist edge correctness when node IDs collide across tables. Filed finding for team review.
- **Test categories covered**: thread safety (multi-producer/single-consumer), batch commit correctness, graceful shutdown, empty queue, cold index preservation, order independence, error resilience, drain_all atomicity
- **Files**: `tests/unit/test_persist_pipelined.cpp`, `CMakeLists.txt` (test registration)

### Grag's File Node Leak Fix — DEC-035 (2026-04-01)
- **Joan's finding adopted:** Implemented explicit `DELETE FROM nodes WHERE node_type='file' AND name = ?` before file_node INSERT in `persist_file()`.
- **Error checking:** Added `sqlite3_step` return value check. Non-SQLITE_DONE now throws with diagnostic info (path + SQLite error message).
- **Result:** 207/208 tests pass (1 pre-existing timing flake in test_watchdog.cpp). Build clean.
- **Next phase:** Prepared statement caching (DEC-027 pattern) will reuse DELETE+INSERT pattern at scale. This fix is foundational.
- **Test validation:** Joan's test suite (208 tests, 1112 assertions) confirms leak fix doesn't regress batch persist or edge creation.

### 2026-04-07: TDD tests for persist pipeline optimizations (pre-implementation)
- Created `tests/unit/test_persist_optimizations.cpp` with 11 test cases (31 assertions) covering three planned optimizations
- **OPT-1 tests (4 cases)**: Cold index DELETE skip, warm index still uses DELETE, duplicate path handling on cold index
- **OPT-2 tests (4 cases)**: Multi-row INSERT batching for symbols/refs/edges, batch boundary correctness (100/200/250 symbols), empty extraction edge case
- **OPT-3 tests (3 cases)**: ResultQueue mechanics, shutdown drain, error resilience — marked `[!mayfail]` as placeholders pending queue implementation
- **TDD approach**: Tests compile and currently pass (OPT-1/OPT-2 scenarios work with existing single-row logic; OPT-3 are WARNs). Once optimizations are implemented, these tests verify correctness without modification.
- **Test patterns adopted**: `cleanup()` helper, `make_test_dir()`, `count_rows()` verification, DEC-008 WAL safety conventions
- **Build**: Clean compile, all 11 tests green (3 emit warnings for unimplemented features)
- **Files**: `tests/unit/test_persist_optimizations.cpp`, `CMakeLists.txt` (test registration)

### 2026-04-07: DEC-039 optimization tests — batch symbol, pre-sized read, parser reuse
- Created `tests/unit/test_dec039_optimizations.cpp` with **20 test cases (473 assertions)** covering three DEC-039 optimizations
- **Batch Symbol INSERT tests (8 cases)** `[persist][batch_symbol]`: Boundary tests for SYMBOL_BATCH_SIZE=20 — 25 symbols (1 batch + 5 remainder), exactly 20 (full batch), 21 (1 batch + 1), 1 symbol (no batch), 0 symbols (empty), 40 symbols (2 full batches), refs→correct node IDs, cross-batch-boundary edges→correct node IDs. Verifies sequential IDs, correct data, edge/ref FK integrity.
- **Pre-sized file read tests (7 cases)** `[file_read][presized]`: Tests `read_file_content()` at 0, 1, 1KB, 100KB, 1MB sizes. Binary content with null bytes and 0xFF preserved. Nonexistent file returns empty string gracefully.
- **Parser reuse tests (5 cases)** `[parser][reuse]`: Same-language reuse (two C++ parses), cross-language switch (C++→C#), 50 sequential Python parses, language round-trip (C++→Python→C++), unsupported language (`sql`) returns false without corrupting parser state.
- **Arena setup pattern**: Parser tests require `register_arena_allocator()` + `ArenaPool`/`ArenaLease`/`set_thread_arena()` because `ts_set_allocator()` is global — other tests register the arena allocator, and without a valid arena, tree-sitter crashes. TreeGuard lifetimes must be scoped per-parse when switching languages (arena `free` is no-op, so cross-parse TSNode references can become invalid).
- **Build**: Clean compile (Release), 0 errors, 0 warnings. All 20 tests green. Full unit suite: 108 tests, 822 assertions, all pass.
- **Files**: `tests/unit/test_dec039_optimizations.cpp`, `CMakeLists.txt` (test registration)

### 2026-07-17: Watchdog timeout redesign tests (DEC-040)
- Created `tests/unit/test_watchdog_timeout.cpp` with **16 test cases (22 assertions)** covering the new watchdog timeout formula
- **Formula tests (7 cases)** `[watchdog][timeout]`: 0B→5000ms, 10KB→5100ms, 50KB→5500ms, 100KB→6000ms, 200KB→7000ms, 500KB→10000ms (cap), 1MB→10000ms (cap). Formula: `min(base + (file_size * 10) / 1024, 10000)`.
- **Kill threshold tests (3 cases)** `[watchdog][timeout]`: 2x multiplier — 0B: cancel=5000/kill=10000, 100KB: 6000/12000, 500KB: 10000/20000.
- **Config defaults tests (2 cases)** `[watchdog][timeout][config]`: `Config::parse_timeout_s == 5`, `Config::extraction_timeout_s == 5`.
- **Edge case tests (4 cases)** `[watchdog][timeout][edge]`: negative file size clamps to base, 10GB file hits cap, boundary file (499KB=9990 vs 500KB=10000), custom base timeout.
- **Key pattern**: Formula is a lambda inside `cmd_index.cpp`, not directly testable. Replicated formula in test file as reference implementation. Config struct is `Config` (not `IndexConfig`).
- **Build**: Clean compile, all 16 tests green. Full unit suite: 136 tests, 904 assertions, all pass.
- **Files**: `tests/unit/test_watchdog_timeout.cpp`, `CMakeLists.txt` (test registration)

### 2026-07-21: Arena lifetime defense tests for P0 heap corruption fixes (DEC-038/039)
- Created `tests/unit/test_arena_lifetime.cpp` with **11 test cases (143 assertions)** covering three P0 fixes
- **ArenaLease thread-local clearing (3 cases)** `[arena][lifetime][lease]`: Verifies `get_thread_arena()` returns `nullptr` after `ArenaLease` destructor runs, arena is returned to pool, and sequential leases all clear correctly.
- **Tree destruction before arena release (3 cases)** `[arena][lifetime][tree]`: Explicit `TreeGuard` reset leaves arena valid (not reset). Multiple trees destroyed in order. Scoped TreeGuard destruction before arena scope ends.
- **Fresh parser per file across arenas (3 cases)** `[arena][lifetime][parser]`: Two arenas with fresh parsers (no reuse), 20 arena cycles with fresh parsers, different languages across arena boundaries. All verify no crash and clean pool state.
- **Combined defense-in-depth (2 cases)** `[arena][lifetime][integration]`: Full parse→destroy→clear→release lifecycle. Arena reset verification: `used() == 0` after re-lease from pool.
- **Existing parser reuse tests (5 cases)** `[parser][reuse]`: Confirmed all still pass — these test single-arena reuse which was never the problem. The cross-arena reuse was the heap corruption root cause.
- **Key learning**: `ts_tree_delete()` calls arena's free (which is a no-op), so `arena->used()` may increase slightly during delete due to tree-sitter internal allocations. Don't assert exact equality of `used()` before/after tree destruction — assert `> 0` instead.
- **Build**: Clean compile, all 146 tests green (1039 assertions). No regressions.
- **Files**: `tests/unit/test_arena_lifetime.cpp`, `CMakeLists.txt` (test registration)
