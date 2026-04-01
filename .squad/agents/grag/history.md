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
