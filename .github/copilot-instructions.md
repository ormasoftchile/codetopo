# Copilot Project Instructions

## File System Rules

- **NEVER** create, read, or write files outside the project working directory.
- All temporary files, test scripts, and scratch work must stay within the workspace.
- Do not use `/tmp`, home directories, or any path outside the repo root.

## Building & Testing (Windows)

`cmake` is not on PATH (it ships with VS 2022) and `VCPKG_ROOT` is not set. A running
`codetopo.exe` (the MCP server) locks the output binary, so stop it before building.

```powershell
$env:VCPKG_ROOT = "$PWD\vcpkg"
$env:PATH = "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;$env:PATH"
Get-Process codetopo -ErrorAction SilentlyContinue | Stop-Process -Force
cmake --build build --config Release
```

- Run tests: `.\build\Release\codetopo_tests.exe "[contract],[integration]"`

## SQLite Query Discipline

- **No query may cause a full table scan** on `nodes` or `edges`. Every query must be
  index-driven and bounded to a scope. The large test index at `C:\One\DsMainDev` has
  ~4.8M nodes / ~44.7M edges — a scan there means a multi-minute hang.
- The index carries **no `ANALYZE` statistics**, so the planner misestimates indexed
  equalities (e.g. `node_type='symbol'` is estimated at ~10 rows but matches millions)
  and will scan `nodes` unless forced otherwise.
  - To force the planner to drive from a small/temp table, use **`CROSS JOIN`** (it pins
    join order). Plain `JOIN`, `IN (subquery)`, and `INDEXED BY` are **not** reliable for
    `nodes` queries with a `node_type` predicate.
  - Bound edge queries with `edges.src_id`/`dst_id IN (SELECT id FROM temp...)` — these
    are safely index-driven via `idx_edges_src`/`idx_edges_dst`.
- Verify with `EXPLAIN QUERY PLAN` against the large index before shipping any new query.

## Branch Naming Convention

Always use this pattern when creating branches:

```
cristian/YYYY/MM/DD/name-of-the-branch
```

Example: `cristian/2026/03/06/add-readme`

- Use the current date
- Use lowercase kebab-case for the branch name
- Keep names short and descriptive
