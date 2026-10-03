# codetopo

A local code‑graph indexer and [MCP](https://modelcontextprotocol.io/) server. codetopo parses source files with [tree‑sitter](https://tree-sitter.github.io/tree-sitter/), extracts symbols and their relationships, stores everything in a SQLite database, and exposes the resulting code graph through 23 query tools — either from the command line or as an MCP server for AI‑assisted coding workflows.

Tested on enterprise repos with 450K+ files — indexes 100K files in ~10 minutes with zero crashes.

## Features

- **Multi‑language parsing** — C, C++, C#, Go, YAML, TypeScript, JavaScript, Python, Rust, Java, Bash, SQL (13 languages)
- **Symbol extraction** — functions, classes, structs, macros, variables, fields
- **Cross‑file references** — call edges, include/import dependencies, inheritance
- **Full‑text search** — FTS5‑powered symbol search by name
- **MCP server** — expose the code graph to AI tools over stdio (JSON‑RPC)
- **Incremental indexing** — detects changed files via content hashing (xxHash)
- **Crash‑resilient supervisor** — automatic restart with quarantine, progress tracking, and single‑thread fallback (up to 10 retries)
- **File watcher** — re‑indexes on save using native OS events (FSEvents / inotify / ReadDirectoryChangesW)
- **Arena allocator** — per‑thread arena pools with overflow fallback for fast, low‑fragmentation parsing
- **Editor integration** — one‑command setup writes a project‑scoped `.mcp.json` (cross‑agent standard) and `.vscode/mcp.json`, plus Cursor/Windsurf/Copilot configs. No user‑level/global config is touched.
- **Agent skills** — installable skill files for AI agents (e.g., refactoring workflows)
- **Cross‑platform** — macOS, Linux, Windows

## Quick Start

### Prerequisites

- CMake ≥ 3.20
- C++20 compiler (Clang, GCC, MSVC)
- [vcpkg](https://vcpkg.io/) with `VCPKG_ROOT` set

### Build

```bash
cmake --preset release
cmake --build build --config Release
```

### One‑command setup

The fastest way to get started — indexes the repo and configures your editor's MCP settings:

```bash
codetopo init --root /path/to/repo
```

This will:
1. Scan and index the repository
2. Write project‑scoped MCP configs — always `.mcp.json` (repo root, the cross‑agent
   standard) and `.vscode/mcp.json`, plus Cursor/Windsurf/Copilot configs when those
   editors are detected. No user‑level/global config is touched; all use a relative
   `--root` and merge with existing entries.
3. Install agent skills into `.github/skills/` (auto‑discovered by Copilot)
4. Write agent‑guidance files — `.github/copilot-instructions.md` and `AGENTS.md` —
   so coding agents prefer codetopo tools over grep/glob/raw file reads. These are
   written non‑destructively: existing files get a removable `codetopo` marker block
   appended, never overwritten.

### Index a repository

```bash
codetopo index --root /path/to/repo
```

This creates `.codetopo/index.sqlite` inside the repository root.

Common options:

```bash
# Use 8 threads, 256 MB arenas, turbo mode
codetopo index --root /path/to/repo --threads 8 --arena-size 256 --turbo

# Large repos: dedicated large‑file arena, file size cap, profiling
codetopo index --root /path/to/repo \
  --large-arena-size 1024 --large-file-threshold 150 \
  --max-file-size 512 --profile

# Limit to first 10K files (for testing/profiling)
codetopo index --root /path/to/repo --max-files 10000
```

### Add workspace roots

```bash
codetopo workspace add /path/to/extra/root --root /path/to/main/repo
```

Workspace add always merges files, symbols, edges, refs, and symbol FTS. Line-level
content FTS for added roots is off by default for faster large-root adds; opt in
only when code-content search over added roots is needed:

```bash
codetopo workspace add /path/to/extra/root --root /path/to/main/repo --with-content-fts
```

### Query the code graph

```bash
# Search for symbols
codetopo query --root /path/to/repo symbol_search '{"query": "main"}'

# Get full context for a symbol (definition + callers + callees)
codetopo query --root /path/to/repo context_for '{"node_id": 42}'

# Blast radius — what breaks if you change a symbol?
codetopo query --root /path/to/repo impact_of '{"node_id": 42, "depth": 3}'

# Shortest dependency path between two symbols
codetopo query --root /path/to/repo shortest_path '{"src_id": 1, "dst_id": 100}'
```

### Start the MCP server

```bash
codetopo mcp --root /path/to/repo
```

The server communicates over stdio using JSON‑RPC. Connect it to any MCP‑compatible client.

After client initialization, server diagnostics are sent both to stderr and as
MCP `notifications/message` events. This includes tool calls, watcher changes,
and reindex completion or failure. Clients can filter protocol logs with
`logging/setLevel`; stderr diagnostics remain available regardless of that level.

```bash
# With file watching for auto‑reindex
codetopo mcp --root /path/to/repo --watch --freshness eager
```

### Watch for changes

```bash
codetopo watch --root /path/to/repo
```

### Diagnose a single file

```bash
codetopo parse-file /path/to/file.cpp --symbols --refs --edges
```

### Install agent skills

`codetopo init` installs agent skills automatically into `.github/skills/`
(committed and auto-discovered by Copilot). To manage them manually:

```bash
codetopo skills list
codetopo skills install refactor   # writes .github/skills/codetopo-refactor/SKILL.md
```

### Health check

```bash
codetopo doctor --root /path/to/repo
```

## Commands

| Command | Description |
|---------|-------------|
| `init` | Index a repo and configure editor MCP settings |
| `index` | Build or update the code graph |
| `mcp` | Start the MCP server over stdio |
| `watch` | Watch for file changes and re‑index |
| `workspace` | Add, remove, or list extra workspace roots |
| `query` | Run an ad‑hoc tool query from the CLI |
| `parse-file` | Parse a single file and show diagnostics |
| `skills` | List or install agent skill files |
| `doctor` | Check database health |

## MCP Tools

File lookups accept repo-relative paths and absolute indexed workspace paths.
On Windows, native backslashes, forward slashes, and mixed separators are
equivalent. Returned paths retain their indexed spelling; relative paths resolve
against the primary repo root, never an arbitrary suffix in another root. Paths
with `..` components are rejected. POSIX path case remains significant.

`context_for` (and the other single-symbol graph tools) accepts one of
`{"stable_key":"..."}`, `{"node_id":42}`, or
`{"symbol":"methodName","file":"src/example.cpp"}`. Symbol lookup is exact by name
or qualified name, prefers definitions over declarations, and returns
`{"ambiguous":true,"candidates":[...]}` for multiple matching definitions.
Choose a candidate's handle to disambiguate. Missing files/symbols return
`not_found`; malformed selectors return `invalid_input`. `stable_key` takes
precedence over `node_id`, which takes precedence over symbol/file.
`expected_stable_key` still guards a supplied node ID. Context options such as
`include_source`, `max_source_lines`, and caller/callee caps work with every
selector. In JSON, escape each backslash once, e.g.
`{"symbol":"methodName","file":"src\\example.cpp"}`.

| Tool | Description |
|------|-------------|
| `server_info` | Server capabilities, schema version, uptime |
| `server_health` | Prompt readiness (`ready`/`busy`), without SQLite graph queries |
| `repo_stats` | File counts and index metadata; uncomputed graph totals are null |
| `file_search` | Search files by GLOB path pattern |
| `dir_list` | List files and subdirectories in a directory |
| `symbol_search` | Search symbols by name (FTS5) |
| `symbol_list` | Filter symbols by kind, file, or name glob (no FTS) |
| `symbol_get` | Get details for a symbol by node_id |
| `symbol_get_batch` | Get details for multiple symbols at once |
| `callers_approx` | Find all callers of a symbol (groupable) |
| `callees_approx` | Find all callees of a symbol (groupable) |
| `references` | Find all references to a symbol |
| `file_summary` | List all symbols defined in a file |
| `context_for` | Full context: definition, source, callers, callees, container, siblings |
| `entrypoints` | Find entry points (main, DllMain, etc.) |
| `impact_of` | Transitive blast radius of changing a symbol |
| `file_deps` | File‑level include/import dependencies |
| `subgraph` | Local dependency neighborhood around seed symbols |
| `shortest_path` | Shortest dependency path between two symbols |
| `find_implementations` | Find types implementing or inheriting from a base type |
| `method_fields` | Field accesses and outgoing calls made by a method |
| `dependency_cluster` | Group methods by shared field access for refactoring |
| `source_at` | Read raw source lines from a file by line range |
| `reindex` | Trigger a background re‑index |
| `workspace_add` | Start an extra-root indexing/merge job; returns `job_id` |
| `workspace_refresh` | Incrementally reindex an existing extra root, then merge it |
| `workspace_remove` | Start an extra-root removal job; returns `job_id` |
| `workspace_job_status` | Poll a job's phase, elapsed time, terminal result or error |
| `workspace_job_cancel` | Request cancellation at the next safe phase boundary |
| `workspace_list` | List committed extra roots and scoped counts |

### Workspace jobs and editor lifecycle

MCP workspace mutations are asynchronous; CLI `workspace add/remove` and `query`
retain their synchronous behavior. For MCP, call `workspace_add` or
`workspace_refresh` with `{"path":"C:\\projects\\reference"}`, then call
`workspace_job_status` with the returned `{"job_id":"workspace-1"}`.
Only one job runs at a time; only the latest job is retained in memory for the
session. IDs are session-local, not durable across reconnections.

Status contains `operation`, `path`, `status`, `phase`, `elapsed_ms`,
`cancel_requested` and `cancellation_policy`. States are `queued`, `running`,
`completed`, `failed`, or `cancelled`; `completed` includes `result`, and
`failed` includes `error`. Progress is phase-level, not a fabricated percentage:
`waiting_for_writer`, `acquiring_lock`, `opening_workspace`,
`indexing_extra_root`, `merging` (or `removing`), and `done`.
An add/refresh incrementally scans **only the chosen extra root**, detecting
new, changed and deleted files in its own index before merging. It does not run
a primary-root reindex. MCP background extra-root indexing uses at most four
threads. Source indexes must be standalone (no nested extra workspace roots).
Merge IDs are allocated above each destination table's indexed maximum in the
writer transaction and foreign keys are remapped together. Primary growth and
legacy high IDs cannot collide on refresh; cleanup uses root ownership, not ID
ranges. Stable symbol keys retain their root prefix across refreshes.
CLI add still reuses an existing source index; use MCP refresh or
`codetopo index --root <extra-root>` before a synchronous CLI add to update it.

`workspace_job_cancel` is explicit: client request timeouts and
`notifications/cancelled` do **not** cancel an accepted job. Cancellation is
observed between safe phases. An already-running index child finishes, then
cancellation can prevent the merge; the extra root's source index may therefore
have advanced even if the workspace job was cancelled. Merge/removal is one
noninterruptible safe phase: cancellation during it may end in `completed`,
with `cancel_requested:true`, rather than falsely report a rollback.

MCP graph reads remain available during startup indexing, explicit reindex and
workspace merge/removal. The stdio owner uses its own SQLite connection and a
request-scoped committed WAL snapshot: multi-query graph results see the prior
or new committed state, never a partially committed merge. Snapshots and cached
statement cursors are released before writing the response. Workers never use
or finalize the reader's statements; committed changes invalidate its cache at
the next request, including independent CLI writer commits.
`server_health` reports writer readiness, **not** read unavailability or a
database integrity guarantee. Write conflicts (for example `ingest_traces`)
still return JSON-RPC `busy` (`data.error_code:"busy"`, `data.retryable:true`).
Genuine SQLite read lock contention uses a 250ms busy timeout per SQLite
operation, not the writer-sized 30s timeout. No missing index is silently
created for a read.
Indexing and mutations retain serialized writer admission and CLI lock files;
source locks remain held through DETACH. No global graph totals, index rebuild
or full integrity scan is used as a health proxy. `repo_stats` reports file
counts and metadata; `symbol_count`/`edge_count` and per-root graph totals are
`null`, with `graph_counts_checked:false` and `graph_counts_status:"not_computed"`.
Workspace job results and `workspace_list` retain their scoped exact counts.

Editor stdio sessions default to **no idle shutdown** (`--idle-timeout 0`).
An explicitly positive idle timeout is enforced while waiting for input,
including an incomplete line. EOF, idle shutdown or transport error stops the
watcher, discards queued indexing work, requests job cancellation, and joins
owned workers before destroying their state. A running child or safe merge
finishes before shutdown returns; shutdown is not guaranteed bounded if that
phase stalls. Hosts opting into idle shutdown must reconnect/reinitialize.
`--freshness normal` and `eager` reconcile in the background while reads use the
last committed index (including unrelated extra roots). Watching stays active.
`lazy`/`off` skip startup indexing as before. Graph snapshots cover indexed data;
source snippets read from disk can reflect newer, not-yet-indexed file contents.
All index writers retain secondary query indexes and symbol FTS sync triggers
through bulk batches, including standalone CLI, watcher and workspace-refresh
subprocesses. A writer lock does not exclude readers; there is no offline
index-drop optimization. Symbol search tracks committed structural batches,
including when a bulk child restarts with a small remaining worklist.
Persistence batches acquire SQLite write admission with `BEGIN IMMEDIATE`.
Failed persistence statements roll back and terminate the indexing child with
an explicit error rather than letting a persistence-thread exception crash it.
Workspace imports allocate file, node and reference IDs above the destination
table's current maximum inside the writer transaction, remapping graph
endpoints. Refresh/removal uses root ownership, not fixed numeric ID ranges;
existing billion-offset IDs remain valid without a schema migration.

Lifecycle, job phases, child PID/exit status and timing go to **stderr only**,
not the protocol stdout stream. These explain observed shutdowns and failures;
an abruptly missing process still requires external exit capture to establish
its cause. They do not prove a historical crash, deadlock or memory leak.

Regression validation:
`codetopo_tests.exe "[workspace],[lock],[contract],[integration]"`, plus
`node tests\integration\workspace_jobs_stdio.js <path-to-Release-codetopo.exe>`
(Node 22+ with built-in SQLite)
from the repository root. Keep `TEMP`/`TMP` pointed at a workspace-local fixture
directory when running legacy tests. Query plans are validated without
`ANALYZE` on synthetic local fixtures; external multi-million-row indexes were
not used for this validation.
The stdio suite also pauses real >1000-file writers after a committed bulk batch
and checks every read tool, index availability and scoped node/edge query plans.
Use `--bulk-only` to run just the bulk-overlap scenarios (normal startup with
watching, watcher updates, MCP/CLI reindex, targeted/force CLI and workspace
refresh with a separate source-root MCP reader).
`--bulk-source-only` isolates source refresh; `--bulk-after-primary` checks source
refresh after primary bulk growth. `--acceptance-sequence` runs one connected
production lifecycle: an existing legacy-ID extra root, normal watched bulk
startup with committed reads, primary growth, second-root add, bulk source
refresh, both-root removal, and EOF during an active primary child. It checks
every supervised child exits successfully without quarantine, complete bulk
contents, unchanged primary identities and data across workspace mutations,
pipelined cache traffic, and owned lock/progress/worklist cleanup. Its fixtures
use independent Git repositories so the watcher does not inherit parent Git
metadata.

Lookup-only regressions: `codetopo_tests.exe "[lookup-api]"` checks lexical
normalization, selector semantics, and the actual scoped SQL query plans.
`node tests\integration\lookup_stdio.js <path-to-Release-codetopo.exe>` exercises
the production MCP dispatcher and advertised schemas, with primary/extra-root
fixtures entirely inside the repository. It uses a controlled symbol graph to
isolate lookup behavior from extractor duplicate-symbol behavior.
An explicit absolute primary-root path scopes listings to primary files;
`.` retains its workspace-wide behavior. Absolute primary-root `*` and `**`
patterns resolve to relative indexed paths without matching extra roots, with
native Windows and forward-slash separators accepted. Search patterns retain
SQLite GLOB semantics (`*` can match directory separators); they are not
filesystem-style single-level globs. Scoped plan regressions use the production
schema without `ANALYZE` and reject full node/edge index traversals.

## Project Structure

```
src/
  cli/        Command handlers (index, init, mcp, watch, query, doctor, skills, parse-file)
  core/       Config, arena allocator, arena pool, thread pool
  db/         SQLite connection, schema, FTS, queries
  index/      Scanner, parser, extractor, language ID, persister, supervisor
  mcp/        MCP server, tools, JSON‑RPC dispatch
  tui/        Terminal progress display
  util/       JSON helpers, hashing, logging, path utilities, file locking, git, process
  watch/      Native file system watcher
tests/
  unit/       Unit tests (18 files)
  integration/ Integration tests — indexing, MCP round‑trip, crash recovery, watch
  contract/   Contract tests — error envelopes, MCP schemas
```

## Dependencies

Managed via [vcpkg](https://vcpkg.io/):

| Library | Purpose |
|---------|---------|
| [SQLite3](https://www.sqlite.org/) (with FTS5) | Database and full‑text search |
| [tree‑sitter](https://tree-sitter.github.io/tree-sitter/) | Incremental parsing (13 language grammars) |
| [CLI11](https://github.com/CLIUtils/CLI11) | Command‑line interface |
| [yyjson](https://github.com/ibireme/yyjson) | Fast JSON parsing/generation |
| [FTXUI](https://github.com/ArthurSonzogni/FTXUI) | Terminal UI (progress bars) |
| [xxHash](https://github.com/Cyan4973/xxHash) | Fast content hashing |
| [Catch2](https://github.com/catchorg/Catch2) | Testing framework |

## Performance

Benchmarked on a 450K‑file enterprise C++/C# repository, Windows, 16 threads, NVMe SSD:

| Scale | Time | Throughput | Symbols | Edges |
|-------|------|-----------|---------|-------|
| 100K files (cold cache) | 659s | 240 files/s | 3.1M | 2.5M |
| 162K files (cold cache) | 971s | 260 files/s | 4.2M | 3.5M |

Key optimizations: batched SQLite inserts, deferred index building, WAL + mmap with 512 MB page cache, arena allocator with overflow fallback, crash‑resilient supervisor with quarantine.

## License

[MIT](LICENSE) — Copyright (c) 2026 Cristián Ormazábal
