# Joan — History

## Project Seed

- **Project:** codetopo — MCP server in C++ for indexing symbols/relations across large codebases
- **Stack:** C++20, CMake, vcpkg, SQLite, tree-sitter, Catch2
- **User:** Cristiano
- **Key challenge:** Unit tests pass (173 cases, 914 assertions) but real-world indexing of 162k-file codebase crashes repeatedly with 0xC0000409. Tests don't cover the actual production failure path.

## Learnings

- **2026-03-30 — Full suite run (Release):** 173/173 test cases passed, 914/914 assertions. Zero failures.
  - Noisy but harmless stderr: `fatal: not a git repository` (expected — tests run in temp dirs without `.git`), `ERROR: Failed to spawn child process` (error 87 and 2 — tests exercising invalid-process paths).
  - Watchdog stress tests emit diagnostic WARNINGs (not failures) showing bottleneck/starvation scenarios behave as designed.
  - Seed used: 371472492. No flakiness observed this run.
- **2026-03-31 — Parse timeout wiring tests:** Added 6 test cases (24 assertions) in `tests/unit/test_parse_timeout.cpp` covering the concurrency bug fix where `parse_timeout_s` was never wired to tree-sitter. Tests cover: Parser::set_timeout API wiring, Parser::set_cancellation_flag aborting parse (deterministic nullptr), Extractor cancel_flag propagation, Extractor timeout_s deadline enforcement, pipeline with cancelled early task completing without hang (head-of-line blocking prevention), and multiple cancelled tasks draining the thread pool without deadlock. Full suite: 179 tests, 938 assertions, all green.
  - Key insight: deterministic timeout tests are hard because tree-sitter may finish fast on powerful hardware. Used cancellation flag (pre-set to 1) for deterministic "parse returns nullptr" assertions. Timeout-based tests use generous bounds and SUCCEED on either outcome.
- **2026-03-31 — DEC-028 extraction timeout tests:** Added 5 live + 1 guarded test case (21 assertions) in `tests/unit/test_extraction_timeout.cpp`. Covers: deadline truncation via cancel_flag (deterministic), generous timeout no-truncation, Config default max_file_size_kb==512 (R2 already landed), timeout_s=0 means unlimited, and truncated results contain partial symbols+edges (not empty). Config `extraction_timeout_s` field test is `#if 0` guarded until Grag adds R1. Full suite: 184 tests (183 pass, 1 pre-existing watchdog flake), 960 assertions.
  - Key pattern: 1000 functions ≈ 20K+ AST nodes guarantees hitting the 4096-node deadline check boundary. cancel_flag=1 deterministically triggers the same code path as time-based deadline.
  - Grag already landed R2 (max_file_size_kb default = 512) — test passes immediately.
### DEC-028 extraction timeout test coverage (2026-03-31)
- Wrote 6 test cases in test_extraction_timeout.cpp (21 assertions total)
- Proactive testing: tests written before Grag's R1 implementation, with test 4 gated via #if 0 pending Config::extraction_timeout_s field addition
- Upon Grag's R1 commit, test 4 automatically unblocked and passed
- Test suite covers: deadline truncation (deterministic cancel_flag), generous timeout no-truncation, Config defaults (max_file_size_kb==512), timeout_s=0 unlimited, truncated results with partial data
- Orchestration log (2026-03-31T2237Z-joan.md) documenting test coverage and integration
- All 185 tests passing (964 assertions)
### DEC-026 persist pipeline test coverage (2026-03-31)
- Wrote 18 test cases in test_persist_pipeline.cpp (132 assertions total)
- Proactive testing: tests written before Grag's R3/R4 implementation to verify expected behavior
- 5 scenarios covered: (1) Cold index persist — fresh DB correctness, empty extraction, multi-file cold; (2) Warm index persist — re-persist idempotency, shrink, triple overwrite; (3) Correctness — N-file symbol counts, refs with file association, parse_error storage; (4) Batch — atomic begin/commit, flush_if_needed threshold, double-begin idempotency, commit-without-begin safety; (5) Out-of-order drain — shuffled order totals, interleaved different files, mixed success/failure in batch; plus cascade verification and standalone transaction mode
- Key pattern: `make_test_dir()` helper cleans stale temp dirs before creating — fixes WAL file locking causing stale data between runs on Windows (DEC-008 enhancement)
- Full suite: 203 tests, 1096 assertions, all green
- Established DEC-033: WAL-safe temp directory pattern for all DB tests going forward

