# codetopo

A local code‑graph indexer and [MCP](https://modelcontextprotocol.io/) server. codetopo parses source files with [tree‑sitter](https://tree-sitter.github.io/tree-sitter/), extracts symbols and their relationships, stores everything in a SQLite database, and exposes the resulting code graph through a set of query tools — either from the command line or as an MCP server for AI‑assisted coding workflows.

## Features

- **Multi‑language parsing** — C, C++, C#, Go, YAML, TypeScript, JavaScript, Python, Rust, Java, Bash
- **Symbol extraction** — functions, classes, structs, macros, variables, fields
- **Cross‑file references** — call edges, include/import dependencies, inheritance
- **Full‑text search** — FTS5‑powered symbol search by name
- **MCP server** — expose the code graph to AI tools over stdio (JSON‑RPC)
- **Incremental indexing** — detects changed files via content hashing (xxHash)
- **File watcher** — re‑indexes on save using native OS events (FSEvents on macOS, inotify on Linux, ReadDirectoryChangesW on Windows)
- **Arena allocator** — per‑thread arena pools for fast, low‑fragmentation memory during parsing
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

### Index a repository

```bash
./build/codetopo index --root /path/to/repo
```

This creates a `codetopo.sqlite` database in the current directory.

### Query the code graph

```bash
# Search for symbols
./build/codetopo query --db codetopo.sqlite symbol_search '{"query": "main"}'

# Get full context for a symbol (definition + callers + callees)
./build/codetopo query --db codetopo.sqlite context_for '{"node_id": 42}'

# Blast radius — what breaks if you change a symbol?
./build/codetopo query --db codetopo.sqlite impact_of '{"node_id": 42, "depth": 3}'

# Shortest dependency path between two symbols
./build/codetopo query --db codetopo.sqlite shortest_path '{"src_id": 1, "dst_id": 100}'
```

### Start the MCP server

```bash
./build/codetopo mcp --db codetopo.sqlite
```

The server communicates over stdio using JSON‑RPC. Connect it to any MCP‑compatible client.

### Watch for changes

```bash
./build/codetopo watch --root /path/to/repo --db codetopo.sqlite
```

### Health check

```bash
./build/codetopo doctor --db codetopo.sqlite
```

## MCP Tools

| Tool | Description |
|------|-------------|
| `server_info` | Server capabilities, schema version, uptime |
| `repo_stats` | File count, symbol count, edge count, last index time |
| `symbol_search` | Search symbols by name (FTS5) |
| `symbol_get` | Get details for a symbol by node_id |
| `symbol_get_batch` | Get details for multiple symbols at once |
| `callers_approx` | Find all callers of a symbol |
| `callees_approx` | Find all callees of a symbol |
| `references` | Find all references to a symbol |
| `file_summary` | List all symbols defined in a file |
| `context_for` | Full context: definition, source, callers, callees |
| `entrypoints` | Find entry points (main, high in‑degree nodes) |
| `impact_of` | Transitive blast radius of changing a symbol |
| `file_deps` | File‑level include/import dependencies |
| `subgraph` | Local dependency neighborhood around seed symbols |
| `shortest_path` | Shortest dependency path between two symbols |

## Project Structure

```
src/
  cli/        Command handlers (index, mcp, watch, query, doctor)
  core/       Config, arena allocator, thread pool
  db/         SQLite connection, schema, FTS, queries
  index/      Scanner, parser, extractor, change detector, persister
  mcp/        MCP server, tools, error handling
  tui/        Terminal progress display
  util/       JSON helpers, hashing, logging, path utilities, file locking
  watch/      Native file system watcher
tests/
  unit/       Unit tests
  integration/ Integration tests (indexing, MCP round‑trip, watch)
  contract/   Contract tests (error envelopes, MCP schemas)
```

## Dependencies

Managed via [vcpkg](https://vcpkg.io/):

| Library | Purpose |
|---------|---------|
| [SQLite3](https://www.sqlite.org/) (with FTS5) | Database and full‑text search |
| [tree‑sitter](https://tree-sitter.github.io/tree-sitter/) | Incremental parsing |
| [CLI11](https://github.com/CLIUtils/CLI11) | Command‑line interface |
| [yyjson](https://github.com/ibireme/yyjson) | Fast JSON parsing/generation |
| [FTXUI](https://github.com/ArthurSonzogni/FTXUI) | Terminal UI (progress bars) |
| [xxHash](https://github.com/Cyan4973/xxHash) | Fast content hashing |
| [Catch2](https://github.com/catchorg/Catch2) | Testing framework |

## License

[MIT](LICENSE) — Copyright (c) 2026 Cristián Ormazábal
