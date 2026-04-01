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
