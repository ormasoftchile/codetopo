# codetopo

**A live software-topology engine for AI agents.**

codetopo parses source code with [Tree-sitter](https://tree-sitter.github.io/tree-sitter/) (13 languages), builds and persists a relational symbol graph in SQLite, and continuously synchronizes that graph as files change. It fuses static AST relationships, protocol-level HTTP interactions, and empirical runtime traces into an epistemically categorized topology model.

Exposing **44 MCP tools** over stdio (JSON-RPC 2.0), codetopo empowers AI coding agents to perform deterministic structural reasoning — navigating call graphs, calculating blast radius, summarizing architectural boundaries, computing semantic graph diffs, and evaluating measurable graph quality — without hallucinating code structure or blowing up context windows with raw source dumps.

Tested on enterprise codebases with 450K+ files — indexes 100K files in ~10 minutes with zero crashes.

## Features

- **Multi-language AST parsing** — C, C++, C#, Go, YAML, TypeScript, JavaScript, Python, Rust, Java, Bash, PowerShell, Batch (13 languages via Tree-sitter)
- **Structural-first persistence** — Relational symbol graph stored in SQLite with secondary query indexes, FTS5 symbol search, and trigram content search
- **Continuously synchronized** — Native file system watching (FSEvents / inotify / ReadDirectoryChangesW) with debounce, in-flight coalescing, and non-blocking committed reads
- **Targeted symbol attraction** — Incremental graph maintenance distinguishes file parsing from graph linking: unchanged files with dangling/ambiguous references are selectively rebound in $O(|\mathcal{D}_{\text{changed}}| \cdot \bar{d})$ time without repository-wide rescans
- **Epistemic edge provenance** — Schema v15 tracks explicit certainty and evidence provenance for every edge: `RESOLVED` (deterministic static AST), `PROBABLE` (type/arity heuristics), `POSSIBLE` (syntactic candidates), and `OBSERVED` (runtime-verified)
- **Runtime trace fusion** — Ingests empirical OpenTelemetry/OTLP traces and extracts protocol-level HTTP client calls to validate and boost static call graph edges
- **Measurable graph quality** — `codetopo quality` reports exact call resolution percentages by language, edge kind distributions, confidence histograms, and reference disambiguation metrics
- **PR blast radius & semantic diff** — BFS dependency traversal (`impact_of`), Git diff blast-radius analysis (`detect_changes`), and semantic graph diffing between commits or working tree (`diff` / `graph_diff`)
- **Architectural intelligence** — Structural PageRank centrality, directory-based clustering, cohesion, coupling, and extractability scoring (`get_architecture`, `dependency_cluster`)
- **44 MCP tools** — Rich JSON-RPC 2.0 interface built for agents following "structure before source" and "batch, don't bounce" workflows with durable stable keys and candidate fallbacks
- **Multi-root workspaces** — Independent background indexing and transactional remapped merges across multiple repositories (`codetopo workspace`)
- **Crash-resilient supervisor** — Multi-process architecture with automatic restart, file quarantine, progress tracking, and single-thread fallback (up to 10 retries)
- **Zero-fragmentation arena allocator** — Per-thread arena pools with dynamic overflow fallback for fast, low-overhead parsing
- **Zero-config editor setup & skills** — One command writes project-scoped `.mcp.json` (cross-agent standard), `.vscode/mcp.json`, Cursor/Windsurf/Copilot configs, and `.github/skills/`
- **Cross-platform & self-contained** — Single compiled binary + SQLite database; no external daemon, background Docker container, or heavy runtime dependencies

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

### Assess graph quality

```bash
codetopo quality --root /path/to/repo
```

Outputs a comprehensive fidelity report: call resolution rates by language, edge confidence distribution, epistemic provenance breakdown (AST vs. runtime vs. protocol), and reference disambiguation metrics. Add `--json` for machine-readable output.

### Semantic graph diff

```bash
# Compare working tree against HEAD
codetopo diff

# Compare two git revisions
codetopo diff main feature-branch
```

Reports structural symbol additions, deletions, and modifications; call edges created or severed; and structural fan-in topology changes. Add `--json` for machine-readable output.

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
| `watch` | Watch for file changes and continuously re‑index |
| `quality` | Report graph quality, language resolution rates, and confidence metrics |
| `diff` | Compute semantic graph diff between working tree, commits, or index |
| `workspace` | Add, remove, refresh, or list extra workspace roots |
| `query` | Run an ad‑hoc tool query from the CLI |
| `parse-file` | Parse a single file and show detailed diagnostics |
| `skills` | List or install agent skill files |
| `doctor` | Check database health and index integrity |
## Core Architectural Concepts

### Structural-First + Continuously Synchronized

CodeTopo is designed specifically to maintain a live structural model of a software system while the codebase is actively modified by human engineers and AI coding agents.

- **Incremental File Parsing vs. Incremental Graph Maintenance**:
  - *File Parsing*: Detects file modifications using xxHash64 content hashes and mtime metadata. Tree-sitter parses only modified files into CSTs in parallel worker threads.
  - *Graph Maintenance (Symbol Attraction)*: When a file introduces, renames, or deletes a symbol definition, unchanged files in the repository may contain unresolved or ambiguous references that can now resolve (or that were broken). Rather than scanning the entire repository, CodeTopo's **Symbol Attraction** mechanism queries the SQLite index for dangling or ambiguous references matching the changed symbol names, selectively re-resolving only the affected call sites in $O(|\mathcal{D}_{\text{changed}}| \cdot \bar{d})$ time.

### Epistemic Edge Provenance (Schema v15)

The code graph combines structural facts from multiple origins, associating each edge with an explicit confidence score ($c \in [0.0, 1.0]$) and evidence provenance:
- **`RESOLVED` ($c = 1.0$)**: Deterministic static AST resolution (unambiguous symbol in scope or unique global symbol).
- **`PROBABLE` ($c \in [0.8, 0.99]$)**: High-confidence heuristic resolution (e.g., receiver type inference, matching parameter arity, or single-candidate namespace lookup).
- **`POSSIBLE` ($c \in [0.3, 0.79]$)**: Syntactically plausible ambiguous candidate (e.g., widespread method names like `.get()` or `.parse()`).
- **`OBSERVED`**: Empirically confirmed by runtime call traces (OpenTelemetry / OTLP) or protocol-level HTTP client observations. Runtime observations boost static edge confidence and resolve dynamic dispatch ambiguity.

### Measurable Graph Quality

CodeTopo provides deterministic visibility into graph fidelity through the `codetopo quality` CLI command and `graph_quality` MCP tool. Rather than treating static analysis as an opaque black box, it reports exact call resolution percentages per language, edge kind distributions, confidence histograms, and reference disambiguation rates.

## MCP Tools

CodeTopo exposes **44 MCP tools** designed specifically for agentic workflows following the **"structure before source"** and **"batch, don't bounce"** philosophy. Agents query high-level topology and call graphs first, reading targeted raw source lines only when necessary.

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

### 1. Discovery & Directory Navigation

| Tool | Description |
|------|-------------|
| `dir_tree` | Return full directory subtree up to depth N with file sizes and detected languages |
| `dir_list` | List files and subdirectories in a given directory (one level) |
| `file_search` | Search files by GLOB path pattern (e.g. `*numa*`, `src/**/*.h`) |
| `file_deps` | File-level dependencies: which files a file includes/imports, and which include it |

### 2. Symbol Inspection & Structural Context

| Tool | Description |
|------|-------------|
| `context_by_name` | Resolve symbol by name and return full structural context, or disambiguation candidates |
| `context_for` | Full structural context: definition, source snippet, callers, callees, container, siblings, bases |
| `symbol_search` | Search symbols by name (FTS5) with optional `kind`, `file_pattern`, and `match` filters |
| `symbol_list` | Filter symbols by kind, file, or name glob without FTS (listing-safe, reports candidate counts) |
| `symbols_in_path` | List symbols under a directory subtree in lean format (`kind`, `name`, `signature`, `file`, `start_line`) |
| `symbol_get` | Get detailed symbol record and source snippet by reindex-proof `stable_key` or `node_id` |
| `symbol_get_batch` | Get details for multiple symbols at once by node IDs |
| `file_summary` | List all symbols defined in a file (functions, classes, structs, macros, variables) |
| `file_overview` | Top-level symbols in a file with signatures and doc comments (faster than `context_for`) |
| `entrypoints` | Find natural entry points (`main`, `DllMain`, etc.) optionally scoped to a path prefix |

### 3. Call Graph, Flow & Blast Radius

| Tool | Description |
|------|-------------|
| `callers_approx` | Find callers from call graph edges with lean summary mode and refs-backed candidate fallback |
| `callees_approx` | Find all functions/symbols called or referenced by a symbol (groupable by file/module/symbol) |
| `references` | Find all references/usages of a symbol across the workspace |
| `impact_of` | Transitive blast radius (BFS traversal) of changing a symbol from call graph edges |
| `detect_changes` | Git diff blast-radius analysis: maps git diff to changed symbols and walks reverse call edges |
| `subgraph` | Extract a local dependency neighborhood graph around seed symbol IDs with edge-kind filtering |
| `shortest_path` | Find the shortest dependency path between two symbols in the code graph |
| `find_implementations` | Find types implementing or inheriting from a given base type, interface, or trait |

### 4. Code, State & Architecture

| Tool | Description |
|------|-------------|
| `get_architecture` | Summarize repository architecture: clusters, hotspots, boundaries, cohesion, and coupling |
| `method_fields` | List `this.X` field accesses (reads/writes) and outgoing calls classified as internal or external |
| `dependency_cluster` | Group methods by shared field access patterns with extractability scores for refactoring |
| `find_similar` | Find near-duplicate functions using MinHash fingerprints of normalized AST leaf trigrams |
| `source_at` | Read exact raw source lines from a file by line range (`[start_line, end_line]`) |
| `code_search` | Substring search across all indexed source files using a trigram FTS index |
| `list_http_calls` | List extracted HTTP client call refs with URL paths and HTTP verbs |

### 5. Runtime Traces, Evidence & Quality

| Tool | Description |
|------|-------------|
| `ingest_traces` | Ingest empirical runtime call traces (OTLP/OpenTelemetry JSON) and boost matching edge confidence |
| `get_traces` | Query ingested runtime traces by caller/callee substring and minimum call count |
| `get_edge_evidence` | Query epistemic provenance and runtime observation evidence for call graph edges |
| `graph_quality` | Compute comprehensive graph quality report: resolution rates by language, edge kinds, confidence buckets |
| `graph_diff` | Compute semantic graph diff between working tree, commits, or index (symbols, call edges, fan-in deltas) |

### 6. Server, Health & Workspace Management

| Tool | Description |
|------|-------------|
| `server_info` | Server capabilities, schema version (v15), database path, and uptime |
| `server_health` | In-memory prompt readiness probe (`ready`/`busy`) without SQLite graph queries |
| `repo_stats` | Repository file counts and indexing metadata (uncomputed graph totals are null) |
| `reindex` | Trigger a background re-index with non-blocking committed reads |
| `workspace_add` | Start background extra-root indexing/merge job; returns `job_id` |
| `workspace_refresh` | Incrementally reindex an existing extra root, then merge it; returns `job_id` |
| `workspace_remove` | Start background extra-root removal job; returns `job_id` |
| `workspace_list` | List committed extra roots with scoped file, symbol, and edge counts |
| `workspace_job_status` | Poll a workspace job's phase, elapsed time, terminal result, or error |
| `workspace_job_cancel` | Request cancellation of a workspace job at the next safe phase boundary |

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
  cli/        Command handlers (index, init, mcp, watch, quality, diff, workspace, query, doctor, skills, parse-file)
  core/       Config, arena allocator, arena pool, thread pool
  db/         SQLite connection, schema migrations (v1-v15), FTS5, queries
  index/      Scanner, parser, extractor, persister, symbol attraction, quality metrics, semantic diff, pagerank, supervisor
  mcp/        MCP server, 44 tool handlers, JSON‑RPC dispatch, workspace jobs
  tui/        Terminal progress display
  util/       JSON helpers (yyjson), xxHash, logging, path utilities, file locking, git, process
  watch/      Native file system watcher (FSEvents, inotify, ReadDirectoryChangesW)
tests/
  unit/       Unit tests (Catch2)
  integration/ Integration tests — indexing, MCP round‑trip, crash recovery, watch, multi-root
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
