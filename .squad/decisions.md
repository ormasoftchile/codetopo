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

## Governance

- All meaningful changes require team consensus
- Document architectural decisions here
- Keep history focused on work, decisions focused on direction
