# Spec: Local Code Graph Indexer + MCP Server (C++)

**Project codename**: `codetopo`  
**Status**: Draft  
**Primary goal**: Index a large codebase locally into a persistent graph (SQLite) and expose it via a local MCP server for AI agents and developer tools.

---

## 1. Problem Statement

Large repositories (especially C++) are hard to navigate, summarize, and query reliably with LLMs using only “chunked text + embeddings”. We want a local-first system that:

- builds a *structural* model of the codebase (symbols + relationships)
- stores it in a portable local database (SQLite)
- exposes query primitives to agents via MCP (tool calls)
- supports incremental re-indexing for continuous use
- scales to “huge C++” without requiring a cloud service

---

## 2. Goals

### G1 — Local-first indexing
- Index runs on the developer machine.
- Output is a single `codetopo.sqlite` (and optional sidecar files).
- No required cloud service.

### G2 — Useful structural graph
At minimum, capture:
- symbol definitions and declarations
- file/module relationships (`#include`, imports)
- references/call sites (even if unresolved)
- class inheritance edges (best-effort)

### G3 — MCP access
Provide an MCP server that exposes graph queries as tools with stable IDs, file spans, and **source snippets**. Agent tools should return enough context in a single call that the agent rarely needs to read files separately.

### G4 — Incremental updates
Re-index only changed files; keep DB consistent and queryable.

### G5 — Performance & scale
- Handle large C++ repos (many files, heavy headers).
- Parse in parallel.
- Keep memory bounded.

---

## 3. Non-Goals

- Full C++ semantic correctness (template instantiations, overload resolution, macro-expanded AST). Tree-sitter provides structural parsing; clang semantic enrichment is a future enhancement.
- Cross-repo federation (multi-repo graph).
- Remote hosting / enterprise auth.

---

## 4. User Scenarios

### S1 — Agent wants structured context
An agent asks: “show me callers of `Foo::Bar`” and receives candidates + locations without scanning the entire repo.

### S2 — Engineer debugging architecture
Engineer queries include dependency paths between modules and views the shortest include/ref chain.

### S3 — Incremental work loop
Engineer edits files; index updates quickly; MCP queries reflect changes.

### S4 — Huge C++ repo onboarding
Engineer asks: “where is the entrypoint?”, “what owns config?”, “what are the main subsystems?”—graph helps guide exploration.

---

## 5. Architecture Overview

### Components
1. **Indexer CLI** (`codetopo index`)
   - Scans repo
   - Parses files
   - Extracts entities and edges
   - Writes SQLite

2. **MCP Server** (`codetopo mcp`)
   - Loads SQLite read-only
   - Serves MCP tools over stdio (JSON-RPC-like)

3. **Watcher** (`codetopo watch`)
   - Monitors filesystem changes
   - Triggers incremental indexing
   - Keeps index fresh for MCP server

### Execution Flow

```
repo -> indexer (Tree-sitter) -> SQLite graph
SQLite graph -> MCP server -> agent/tools
```

---

## 6. Technology Choices

### Parsing
- **Tree-sitter (C API)** with C++ wrapper layer.
- Supported languages: 
  - C# (primary — AzureSQLTools, DRI Workbench)
  - C, C++
  - TypeScript
  - Go
  - YAML (structural keys/values, not full schema)

### Storage
- **SQLite3** (WAL mode for concurrent reads).
- **FTS5** for symbol/doc text search.

### CLI
- `CLI11` (or equivalent).

### JSON
- `yyjson` (fast C) OR `nlohmann/json` (C++ convenience).
- MCP messages are JSON.

### Concurrency
- Thread pool (std::jthread / std::thread) for parsing shards.

### 6.1 Build System & Dependencies

- **Build**: CMake 3.20+
- **Package manager**: vcpkg (manifest mode) or FetchContent
- **C++ standard**: C++20
- **CI**: GitHub Actions matrix (Linux gcc/clang, macOS clang, Windows MSVC)

Pinned dependencies:

| Dependency | Version | Purpose |
|------------|---------|---------|
| tree-sitter | 0.22+ | C API for parsing |
| tree-sitter-c | latest | C grammar |
| tree-sitter-cpp | latest | C++ grammar |
| tree-sitter-c-sharp | latest | C# grammar |
| tree-sitter-typescript | latest | TypeScript grammar |
| tree-sitter-go | latest | Go grammar |
| tree-sitter-yaml | latest | YAML grammar |
| sqlite3 | 3.45+ | Storage (with FTS5 enabled) |
| CLI11 | 2.4+ | CLI argument parsing |
| yyjson | 0.9+ | JSON (MCP messages) |

The `CMakeLists.txt` must produce two targets: `codetopo` (CLI + indexer) and unit tests.

---

## 7. Data Model (SQLite Schema)

### 7.1 Tables

#### `files`

- `id INTEGER PRIMARY KEY`
- `path TEXT UNIQUE NOT NULL` — normalized: forward slashes, relative to repo root, no trailing slash, symlinks resolved
- `language TEXT NOT NULL CHECK(language IN ('c', 'cpp', 'csharp', 'typescript', 'go', 'yaml'))`
- `size_bytes INTEGER NOT NULL`
- `mtime_ns INTEGER NOT NULL`
- `content_hash TEXT NOT NULL` — xxhash64, hex-encoded (fast, non-cryptographic; sufficient for change detection)
- `parse_status TEXT NOT NULL CHECK(parse_status IN ('ok', 'partial', 'failed', 'skipped'))` — `ok`: fully parsed, `partial`: parsed with errors, `failed`: parse aborted, `skipped`: excluded by config
- `parse_error TEXT`

Indexes:

- `UNIQUE(path)`
- `INDEX(content_hash)`

**Path normalization rules**: All paths stored relative to `repo_root` using forward slashes (`/`). No leading `./`. Symlinks resolved to real path before storage. Case preserved as-is (case-sensitive comparison).

---

#### `nodes`

Unified node table for both files and symbols. Provides a single `node_id` space so edges can use real foreign keys.

- `id INTEGER PRIMARY KEY`
- `node_type TEXT NOT NULL CHECK(node_type IN ('file', 'symbol'))` — discriminator
- `file_id INTEGER` — NULL for file nodes; FK to `files(id)` for symbol nodes
- `kind TEXT NOT NULL CHECK(kind IN ('file', 'namespace', 'class', 'struct', 'union', 'enum', 'enum_value', 'function', 'method', 'variable', 'field', 'typedef', 'macro', 'include', 'interface', 'property', 'event', 'delegate', 'package', 'type_alias', 'mapping_key'))` — extensible in future schema versions
  - C#: `interface`, `property`, `event`, `delegate`, `namespace`
  - Go: `interface`, `package`, `type_alias`
  - YAML: `mapping_key`
- `name TEXT NOT NULL`
- `qualname TEXT`
- `signature TEXT`
- `start_line INTEGER`
- `start_col INTEGER`
- `end_line INTEGER`
- `end_col INTEGER`
- `is_definition INTEGER NOT NULL DEFAULT 1`
- `visibility TEXT CHECK(visibility IN ('public', 'protected', 'private', NULL))`
- `doc TEXT`
- `stable_key TEXT NOT NULL`

Foreign keys:

- `file_id -> files(id) ON DELETE CASCADE`

Indexes:

- `INDEX(file_id)`
- `INDEX(node_type, kind, name)`
- `INDEX(qualname)`
- `UNIQUE(stable_key)`

**File nodes**: Every indexed file gets a `nodes` row with `node_type='file'`, `kind='file'`, `name=<relpath>`, `stable_key=<relpath>::file` (no ambiguity since path is already unique).

---

#### `refs`\n\nUnresolved or resolved references in code.\n\n- `id INTEGER PRIMARY KEY`\n- `file_id INTEGER NOT NULL` — FK to `files(id)`\n- `kind TEXT NOT NULL CHECK(kind IN ('call', 'type_ref', 'include', 'inherit', 'field_access', 'other'))`\n- `name TEXT NOT NULL`\n- `start_line INTEGER`\n- `start_col INTEGER`\n- `end_line INTEGER`\n- `end_col INTEGER`\n- `resolved_node_id INTEGER` — FK to `nodes(id)`, NULL if unresolved\n- `evidence TEXT`\n\nForeign keys:\n\n- `file_id -> files(id) ON DELETE CASCADE`\n- `resolved_node_id -> nodes(id) ON DELETE SET NULL`\n\nIndexes:\n\n- `INDEX(file_id)`\n- `INDEX(kind, name)`\n- `INDEX(resolved_node_id)`

---

#### `edges`

Graph edges between nodes (symbols or files). Uses real foreign keys into the unified `nodes` table. **Edges are stored in one direction only** — the reverse is derived via query (e.g., `WHERE dst_id = ? AND kind = 'calls'` finds callers). This halves storage.

- `id INTEGER PRIMARY KEY`
- `src_id INTEGER NOT NULL` — FK to `nodes(id)`
- `dst_id INTEGER NOT NULL` — FK to `nodes(id)`
- `kind TEXT NOT NULL CHECK(kind IN ('calls', 'includes', 'inherits', 'references', 'contains'))` — forward direction only; reverse derived via index
- `confidence REAL NOT NULL DEFAULT 1.0` — 1.0 = certain (e.g. `#include`), <1.0 = heuristic (e.g. name-matched call site)
- `evidence TEXT`

Foreign keys:

- `src_id -> nodes(id) ON DELETE CASCADE`
- `dst_id -> nodes(id) ON DELETE CASCADE`

Indexes:

- `INDEX(src_id, kind)`
- `INDEX(dst_id, kind)` — enables efficient reverse lookups (e.g., "who calls X?")

**Cascade behavior**: When a file is re-indexed and its `nodes` rows are deleted, all edges from/to those nodes are automatically removed via `ON DELETE CASCADE`. This prevents dangling edges.

---

#### `kv`

- `key TEXT PRIMARY KEY`
- `value TEXT NOT NULL`

Must include:

- `schema_version`
- `indexer_version`
- `repo_root`
- `last_index_time`
- `language_coverage`

---

### 7.2 Stable IDs

`stable_key` is deterministic and **does not include line numbers** (so edits above a symbol don't invalidate its key):

```
stable_key = "<relpath>::<kind>::<qualname or name>"
```

**Collision handling**: If two symbols in the same file have the same `relpath::kind::qualname` (e.g. overloaded functions), append an ordinal: `<relpath>::<kind>::<qualname>#2`, `#3`, etc. Ordinals are assigned by source order within the file.

**Stability guarantee**: A symbol's `stable_key` changes only if the symbol is renamed, moved to a different file, or its kind changes. Edits to other parts of the file do not affect it.

Note: For future semantic enrichment, mapping to Clang USR is preferred and would replace the current scheme.

---

## 8. Indexing Pipeline

### 8.1 Repo scan

Candidate files:

- `.c`, `.cc`, `.cpp`, `.cxx`, `.h`, `.hpp`, `.hh`, `.hxx` (C/C++)
- `.cs` (C#)
- `.ts`, `.tsx` (TypeScript)
- `.go` (Go)
- `.yaml`, `.yml` (YAML)

Exclude directories:

- `.git/`
- `build/`
- `out/`
- `bazel-*`

(configurable ignore list)

**`.gitignore`-aware exclusion**: In addition to the hardcoded exclude list, read and respect the repo's `.gitignore` (and nested `.gitignore` files). Use the same glob semantics as git. Can be disabled with `--no-gitignore`.

**Symlink handling**: Resolve symlinks to their real path before indexing. If the real path is outside the repo root, skip the file. If two symlinks resolve to the same real path, index it once under the real path.

---

### 8.2 Change detection

For each file:

1. compare `(mtime,size)`
2. if changed → compute hash
3. if hash differs → reparse
4. otherwise skip

---

### 8.3 Parse & extract

Using Tree-sitter:

Extract:

- symbol defs/decls
- includes
- inheritance
- call sites
- type references

Populate:

- `nodes` (one `file` node + symbol nodes per file)
- `refs`
- `edges`

**Resource limits per file** (prevents OOM on generated code):

| Limit | Default | Flag |
|-------|---------|------|
| Max file size | 10 MB | `--max-file-size` |
| Max symbols per file | 50,000 | `--max-symbols-per-file` |
| Max AST depth | 500 | (hardcoded) |

Files exceeding limits are indexed with `parse_status = 'partial'` and a `parse_error` explaining what was truncated.

---

### 8.4 Persist

For each changed file (transaction):

1. delete `nodes` rows where `file_id = <this file>` — `ON DELETE CASCADE` automatically removes associated `edges` and `refs`
2. insert new `nodes`, `refs`, `edges` rows
3. update `files` row

Batch transactions every N files (default: 100, configurable with `--batch-size`).

**Deleted-file pruning**: After the repo scan, detect files in the `files` table that no longer exist on disk. Remove their `files` row (which cascades to `nodes` → `edges`/`refs`). This runs every indexing pass, before any re-parsing.

**Dangling edge cleanup**: Because `edges` has `ON DELETE CASCADE` on both `src_id` and `dst_id`, deleting a file's nodes automatically removes all edges pointing from or to those nodes. No orphaned edges survive re-indexing.

**Crash recovery**: Each file's insert/delete is wrapped in a SQLite transaction. If the indexer crashes mid-batch:
- Completed files are committed and consistent.
- The in-progress file's transaction is rolled back by SQLite.
- On next run, change detection re-processes the interrupted file.
- The `kv` key `last_completed_file` is updated per batch to aid debugging.

---

### 8.5 FTS

Create:

```
nodes_fts(name, qualname, signature, doc)
```

Using SQLite FTS5. Required for `symbol_search` quality.

---

### 8.6 Schema migration

`kv.schema_version` stores the current schema version as an integer (initial = `1`).

On startup, the indexer checks `schema_version`:

| Condition | Action |
|-----------|--------|
| Missing `kv` table | Fresh DB — create all tables |
| `schema_version` matches | Proceed normally |
| `schema_version` < current | Re-index from scratch (delete DB, recreate). Log a warning. |
| `schema_version` > current | Abort with error: "DB was created by a newer indexer" |

The MCP server also checks `schema_version` on startup and refuses to serve if incompatible.

---

## 9. MCP Server Spec

### Transport

- MCP over **stdio**
- JSON request/response
- SQLite opened read-only
- **Protocol version**: `codetopo-mcp/1.0`. Reported in `server_info` tool. Clients should check version before relying on tool schemas.
- **Default tool timeout**: 10 seconds for all tools (not just traversals). Configurable via `--tool-timeout`.
- **Idle timeout**: Server exits after 30 minutes of inactivity (configurable with `--idle-timeout`, 0 to disable).

### Response size limits

All MCP tool responses are capped at **512 KB**. If a response (including source snippets) would exceed this:
- Source snippets are truncated to first/last N lines with `"source_truncated": true`
- If still over limit, results are truncated with `"truncated": true` and `"truncated_reason": "response_size"`

This prevents the MCP server from sending payloads that crash or stall the client.

### Source snippet security

The MCP server reads source files from disk using `repo_root` + `file_path`. To prevent path traversal attacks:

1. `file_path` is canonicalized (`realpath`) before use
2. The resolved path must start with `repo_root` — if not, the request is rejected with `invalid_input` error
3. Paths containing `..` are rejected before canonicalization
4. Only regular files are read (no symlinks to outside repo, no device files)

### Source staleness detection

When reading source for a snippet, the MCP server compares the file's current `mtime` against the indexed `mtime_ns` in the `files` table. If they differ:
- The response includes `"stale": true` and `"stale_reason": "file modified since last index"`
- The source returned is the **current** file content (fresh), but line spans may be wrong
- Agents should treat stale results with lower confidence

### Error response schema

All tools return errors in a standard envelope:

```json
{
  "error": {
    "code": "<error_code>",
    "message": "<human-readable message>"
  }
}
```

Error codes:

| Code | Meaning |
|------|---------|
| `not_found` | Symbol/file ID does not exist in the graph |
| `invalid_input` | Missing or malformed parameter |
| `query_timeout` | Graph traversal exceeded time/depth limit |
| `db_error` | SQLite error (busy, corrupt, etc.) |
| `limit_exceeded` | Result set exceeded max allowed rows |

Agents must distinguish "no results" (empty array, no error) from "query failed" (error object present).

---

### Tool: `server_info`

Returns server metadata for capability negotiation and health check.

```json
{
  "protocol_version": "codetopo-mcp/1.0",
  "schema_version": 1,
  "indexer_version": "1.0.0",
  "capabilities": ["fts5", "source_snippets", "context_for", "impact_of"],
  "db_path": "codetopo.sqlite",
  "repo_root": "/home/user/myrepo",
  "uptime_seconds": 3600,
  "db_status": "ok"
}
```

`db_status` is `"ok"`, `"stale"` (index older than 1 hour), or `"error"` (integrity check failed).

---

### Tool: `repo_stats`

Returns:

- file count
- symbol count
- edge count
- last_index_time
- indexer_version

---

### Tool: `symbol_search`

Inputs:

- `query` — search string (substring match, or FTS5 query if available)
- `kind?` — filter by symbol kind
- `limit?` — max results (default: 50, max: 500)
- `offset?` — for pagination (default: 0)
- `include_source?` — include source snippet in results (default: false)

Returns:

```json
[
  {
    "node_id": 42,
    "kind": "function",
    "name": "Foo::Bar",
    "qualname": "ns::Foo::Bar",
    "file_path": "src/foo.cpp",
    "span": { "start_line": 10, "end_line": 25 },
    "source": "void Foo::Bar(int x) {\n  ...\n}"  // only if include_source=true
  }
]
```

**Pagination**: All list-returning tools accept `limit` and `offset`. Response includes `"has_more": true` when more results exist beyond the returned set.

---

### Tool: `symbol_get`

Input:

- `node_id` — from `symbol_search` or other queries
- `include_source?` — include source code (default: **true**)

Returns symbol metadata, span, containing file path, and source code. Returns `not_found` error if ID doesn't exist.

```json
{
  "node_id": 42,
  "kind": "function",
  "name": "Foo::Bar",
  "qualname": "ns::Foo::Bar",
  "signature": "void Bar(int x)",
  "file_path": "src/foo.cpp",
  "span": { "start_line": 10, "end_line": 25 },
  "source": "void Foo::Bar(int x) {\n    validate(x);\n    data_.push_back(x);\n}",
  "doc": "/// Adds x to the internal buffer."
}
```

**Source snippets**: The MCP server reads source from disk at query time using `repo_root` + `file_path` (with path traversal protection — see §9 Security). This ensures freshness (even if the index is slightly stale, the source returned is current). If the file is missing or unreadable, `source` is `null` and `source_error` explains why. If the file changed since indexing, `"stale": true` is included.

---

### Tool: `symbol_get_batch`

Fetch multiple symbols in one call. Avoids N round-trips when the agent already knows what it needs.

Inputs:

- `node_ids` — list of `node_id`s (max: 50)
- `include_source?` — default: true

Returns: array of symbol objects (same schema as `symbol_get`). Missing IDs are omitted, not errored.

---

### Tool: `references`

Inputs:

- `node_id`
- `kind?` — filter by reference kind
- `limit?` — default: 50, max: 500
- `offset?` — for pagination

Returns locations and evidence. Includes `"has_more"` field.

---

### Tool: `callers_approx`

Returns **approximate** candidate callers. Results may include false positives from heuristic name matching.

Inputs:

- `node_id`
- `limit?` — default: 50, max: 500
- `include_source?` — include the call-site line (default: **true**)

Returns:

```json
[
  {
    "caller_node_id": 99,
    "caller_name": "main",
    "file_path": "src/main.cpp",
    "call_site": {
      "line": 45,
      "source": "  result = foo.Bar(config.count);"
    },
    "confidence": 0.85
  }
]
```

The `call_site.source` line lets the agent see *how* the function is called without a separate `read_file`.

Uses:

- resolved edges if available
- otherwise heuristic name matching.

**Parse quality note**: Tree-sitter C++ cannot resolve templates, macros, or overloads. For `callers_approx` and `callees_approx`, expect ~70-85% precision on typical C++ code. False positives come from name collisions across namespaces; false negatives from macro-wrapped call sites.

---

### Tool: `callees_approx`

Returns approximate callees (same precision caveats as `callers_approx`).

Inputs:

- `node_id`
- `limit?` — default: 50, max: 500
- `include_source?` — include callee signature (default: true)

---

### Tool: `file_summary`

Returns a structural summary of a file — enough for an agent to understand what's in it without reading the entire file.

Input:

- `path` — relative file path

Returns:

```json
{
  "path": "src/foo.cpp",
  "lines": 342,
  "language": "cpp",
  "includes": ["<vector>", "<memory>", "\"bar.h\"", "\"config.h\""],
  "symbols": [
    { "kind": "class", "name": "Foo", "qualname": "ns::Foo", "span": { "start_line": 15, "end_line": 200 },
      "members": [
        { "kind": "method", "name": "Bar", "visibility": "public", "signature": "void Bar(int)" },
        { "kind": "method", "name": "Baz", "visibility": "public", "signature": "std::string Baz()" },
        { "kind": "field", "name": "data_", "visibility": "private" },
        { "kind": "field", "name": "config_", "visibility": "private" }
      ]
    },
    { "kind": "function", "name": "helper", "qualname": "ns::helper", "signature": "void helper(const Foo&)" }
  ],
  "included_by": ["src/main.cpp", "test/test_foo.cpp"]
}
```

**Purpose**: Replaces the agent's "read the whole file to understand it" pattern. The structural map + member list lets the agent pick exactly which symbols to inspect further with `symbol_get`.

---

### Tool: `context_for`

The agent's power tool. Returns everything needed to understand a symbol in **one call**, replacing the typical 5-8 round-trip pattern (`search → read definition → find callers → read caller 1 → ...`).

Inputs:

- `node_id` — the symbol to understand
- `depth?` — how many levels of callers/callees to include (default: 1, max: 2)
- `max_callers?` — default: 10
- `max_callees?` — default: 10

Returns:

```json
{
  "symbol": {
    "node_id": 42,
    "kind": "function",
    "name": "Bar",
    "qualname": "ns::Foo::Bar",
    "signature": "void Bar(int x)",
    "file_path": "src/foo.cpp",
    "span": { "start_line": 10, "end_line": 25 },
    "source": "void Foo::Bar(int x) {\n    validate(x);\n    data_.push_back(x);\n}",
    "doc": "/// Adds x to the internal buffer."
  },
  "container": {
    "kind": "class",
    "name": "Foo",
    "qualname": "ns::Foo",
    "members": [
      { "kind": "method", "name": "Bar", "visibility": "public" },
      { "kind": "method", "name": "Baz", "visibility": "public" },
      { "kind": "field", "name": "data_", "visibility": "private" }
    ]
  },
  "callers": [
    {
      "caller_name": "main",
      "file_path": "src/main.cpp",
      "call_site": { "line": 45, "source": "  result = foo.Bar(config.count);" },
      "confidence": 0.85
    }
  ],
  "callees": [
    {
      "callee_name": "validate",
      "qualname": "ns::validate",
      "signature": "bool validate(int)",
      "confidence": 1.0
    },
    {
      "callee_name": "push_back",
      "qualname": "std::vector::push_back",
      "confidence": 0.7
    }
  ],
  "includes": ["<vector>", "\"config.h\""],
  "included_by": ["src/main.cpp"]
}
```

**Design rationale**: Coding agents (Copilot, Cursor, etc.) typically need 5-8 MCP calls or file reads to understand a single symbol: find it, read it, find callers, read each caller, find callees. `context_for` collapses this into one call. The response is bounded by `max_callers` + `max_callees` to stay under ~50KB.

---

### Tool: `entrypoints`

Returns the natural starting points for understanding the codebase.

Inputs:

- `limit?` — default: 20

Returns:

```json
[
  { "node_id": 1, "kind": "function", "name": "main", "file_path": "src/main.cpp", "span": { "start_line": 5, "end_line": 30 } },
  { "node_id": 200, "kind": "class", "name": "Application", "file_path": "src/app.h", "span": { "start_line": 10, "end_line": 150 } }
]
```

Heuristics for selecting entrypoints:
1. Functions named `main`, `Main`, `wmain`
2. Symbols with highest in-degree (most referenced)
3. Top-level classes/namespaces (no parent container)
4. Files with highest include in-degree ("hub" headers)

**Purpose**: Saves the agent's "grep for main" / "what are the key files" onboarding dance.

---

### Tool: `impact_of`

Answers: "if I modify this symbol, what else might break?" Returns transitive dependents.

Inputs:

- `node_id` — the symbol being modified
- `depth?` — how many levels of transitive dependents (default: 2, max: 3)
- `max_nodes?` — default: 50, max: 200

Returns:

```json
{
  "symbol": { "name": "Foo::Bar", "file_path": "src/foo.cpp" },
  "impacted": [
    {
      "node_id": 99,
      "name": "main",
      "file_path": "src/main.cpp",
      "relationship": "calls",
      "distance": 1,
      "call_site": { "line": 45, "source": "  result = foo.Bar(config.count);" }
    },
    {
      "node_id": 150,
      "name": "TestBar",
      "file_path": "test/test_foo.cpp",
      "relationship": "calls",
      "distance": 1
    }
  ],
  "impacted_files": ["src/main.cpp", "test/test_foo.cpp"],
  "truncated": false
}
```

**Purpose**: Agents should check impact before editing. Today they can't do this efficiently — they'd need to walk the call graph manually. This gives them the answer in one call.

---

### Tool: `file_deps`

Input:

```
path
```

Returns include relationships.

---

### Tool: `subgraph`

Inputs:

- `seed_symbols` — list of `node_id`s
- `depth` — max traversal depth (**required**, max: 5)
- `edge_kinds` — filter by edge type
- `max_nodes?` — max nodes in result (default: 200, max: 1000)

Returns nodes + edges. Returns `query_timeout` error if traversal exceeds 5 seconds.

**Bound enforcement**: `depth` is capped at 5 server-side regardless of client input. If the result set exceeds `max_nodes`, traversal stops and the response includes `"truncated": true`.

---

### Tool: `shortest_path`

Inputs:

- `src_node_id`
- `dst_node_id`
- `max_depth?` — default: 10, max: 20
- `edge_kinds?` — filter by edge type

Returns graph path (list of nodes + edges). Returns empty if no path found within depth limit. Returns `query_timeout` error if search exceeds 5 seconds.

---

## 10. CLI Spec

Commands:

```
codetopo index
codetopo mcp
codetopo watch
codetopo query
codetopo doctor
```

Example:

```
codetopo index --root repo --threads 16
codetopo mcp --db codetopo.sqlite
codetopo watch --root repo --db codetopo.sqlite
codetopo doctor --db codetopo.sqlite
```

Exit codes:

```
0 success
1 partial errors
2 usage error
3 schema mismatch
```

---

## 11. Performance Requirements

- parallel parsing (thread pool, default = CPU count)
- incremental hashing (mtime fast-path)
- WAL SQLite mode
- prepared statements
- batched inserts (default batch: 100 files)
- optional header skip mode

### DB concurrency contract

- **Single writer**: Only one indexer process writes to the DB at a time. The indexer acquires a file lock (`codetopo.lock`) on startup. The lock file contains the PID and timestamp. **Stale-lock detection**: on startup, if a lock file exists, the indexer checks if the PID is still alive. If the PID is dead, the lock is broken automatically and a warning is logged. This prevents a crashed indexer from blocking all future indexing.
- **Multiple readers**: The MCP server (and `codetopo query`) open the DB in `SQLITE_OPEN_READONLY` with WAL mode, allowing concurrent reads during indexing.
- **Busy timeout**: All connections use `PRAGMA busy_timeout = 5000` (5 seconds). If the writer holds a lock longer, readers get `SQLITE_BUSY` and the MCP server returns a `db_error`.
- **No cross-process coordination beyond file lock**: The MCP server does not need to coordinate with the indexer. WAL mode handles concurrent access.
- **WAL checkpoint**: The indexer runs `PRAGMA wal_checkpoint(PASSIVE)` after each full indexing pass to prevent the WAL file from growing unbounded. The MCP server does NOT checkpoint (read-only).

### Disk space estimation

| Repo size | Approx DB size | Notes |
|-----------|---------------|-------|
| 1K files | ~5-15 MB | Small project |
| 10K files | ~50-150 MB | Medium C++ repo |
| 100K files | ~500 MB - 1.5 GB | Large monorepo |

Estimates assume ~50 symbols/file average. FTS5 adds ~20% overhead.

---

## 12. Observability

Logging:

- parse errors
- file counts
- timings
- deleted-file pruning counts
- stale-lock recovery events

Trace file:

```
index_trace.jsonl
```

**Log rotation**: `index_trace.jsonl` is rotated at 50 MB. Keep last 3 files (configurable with `--max-trace-files`). Prevents unbounded log growth.

Metrics stored in `kv`.

---

## 13. Risks

### C++ semantics complexity

Tree-sitter provides structural parsing only — no template instantiation, overload resolution, or macro expansion. This means:
- `callers_approx` / `callees_approx` are heuristic (name-matched), not semantic.
- Template-heavy code will have lower-quality edges.
- Macro-wrapped definitions may be missed.

Mitigation:
- structural graph is the baseline; clang semantic enrichment is a future enhancement
- all approximate tools clearly labeled `_approx` with documented precision expectations (~70-85%)

---

### Large repo indexing time

Mitigation:

- incremental parsing
- skip unchanged files
- parallel thread pool
- resource limits prevent single-file OOM

---

### Large query payloads

Mitigation:

- all list-returning MCP tools have `limit` (max 500) and `offset`
- graph traversals have `max_depth` (capped at 5/20) and `max_nodes` (capped at 1000)
- 5-second timeout on traversals

---

### DB corruption / crash

Mitigation:

- WAL mode + per-file transactions
- crash recovery via change detection on next run
- `codetopo doctor` validates DB integrity (`PRAGMA integrity_check`)

---

### Build portability

Mitigation:

- CMake-based build with pinned dependencies (see §6.1)
- CI matrix for Linux, macOS, Windows

---

## 14. Future Extensions

- clang semantic enrichment (resolve templates, macros, overloads)
- compile_commands.json support
- graph visualization
- multi-repo indexing
- IDE extension (VS Code)
- commit-based snapshots
- remote MCP server (network transport, auth)

---

## 15. Acceptance Criteria

- indexer produces `codetopo.sqlite`
- repo >10k files indexed successfully
- deleted files pruned from DB on re-index
- MCP server responds to all tools:
  - server_info (with uptime, db_status)
  - repo_stats
  - symbol_search (with pagination, FTS5)
  - symbol_get (with source snippets)
  - symbol_get_batch
  - references (with pagination)
  - callers_approx / callees_approx (with call-site source)
  - file_summary
  - context_for (one-shot symbol understanding)
  - entrypoints
  - impact_of
  - file_deps
  - subgraph (with depth and node limits)
  - shortest_path (with depth limit)
- incremental indexing works (changed files only, edges cleaned via cascade)
- queries <200ms typical for common operations
- `stable_key` survives unrelated edits (no line-number dependency)
- schema version checked on startup; mismatch triggers re-index
- error responses follow the standard error schema
- `codetopo doctor` runs `PRAGMA integrity_check` and reports schema version
- `context_for` returns complete symbol context (source + callers + callees) in a single call
- response size capped at 512 KB with graceful truncation
- source snippet paths validated against repo root (no path traversal)
- stale source detected and flagged in response
- lock file uses PID; stale locks auto-broken
- `codetopo watch` monitors filesystem and triggers incremental re-index
- edges stored in one direction only; reverse derived via index
- log rotation on `index_trace.jsonl`
- WAL checkpoint after each indexing pass
- MCP server idle timeout (default 30 min)