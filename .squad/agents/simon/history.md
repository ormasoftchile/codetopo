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
