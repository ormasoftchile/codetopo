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
- **Editor integration** — one‑command setup for VS Code, Cursor, Windsurf, and Claude Desktop
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
2. Auto‑detect your editor (VS Code, Cursor, Windsurf) or fall back to VS Code
3. Write the MCP server configuration so your AI tools can use the code graph immediately

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

```bash
codetopo skills list
codetopo skills install refactor
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
| `query` | Run an ad‑hoc tool query from the CLI |
| `parse-file` | Parse a single file and show diagnostics |
| `skills` | List or install agent skill files |
| `doctor` | Check database health |

## MCP Tools

| Tool | Description |
|------|-------------|
| `server_info` | Server capabilities, schema version, uptime |
| `repo_stats` | File count, symbol count, edge count, last index time |
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
