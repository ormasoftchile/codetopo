# Index Freshness & Invalidation Proposal

## Problem Statement

The codetopo MCP server opens a read-only SQLite connection at startup and serves
from it indefinitely. It has no awareness of file changes, branch switches, or
git operations. The user silently gets stale data with no indication.

---

## Current Architecture

| Process | Change awareness | Writes | Reads |
|---|---|---|---|
| `codetopo index` | Scans disk, compares mtime/size/hash vs DB | Yes | No |
| `codetopo mcp` | **None** — opens DB once, never re-checks | No | Yes (stale) |
| `codetopo watch` | Filesystem events (ReadDirectoryChangesW / polling) | Yes (incremental) | No |

The `kv` table stores metadata but **nothing about git state**:

| Key | Value |
|---|---|
| `schema_version` | `1` |
| `indexer_version` | `1.0.0` |
| `repo_root` | absolute path |
| `last_index_time` | ISO 8601 |
| `language_coverage` | comma-separated |

No `git_branch`, no `git_commit`, no `git_head`.

---

## Invalidation Scenarios

### 1. Branch Switch — CRITICAL

`git checkout` / `git switch` can change thousands of files simultaneously:
- Files only on old branch → **phantom entries** (symbols that don't exist on disk)
- Files only on new branch → **missing entries** (invisible to queries)
- Files differing between branches → **stale symbols/edges** (wrong lines, deleted functions still visible)

**Current behavior**: MCP serves old branch's data silently. No indication to client.

### 2. File Edits During Active MCP Session — HIGH

- Line numbers in `symbol_get` snippets drift
- New functions invisible, deleted functions still appear
- `callers_approx` / `impact_of` return stale call graphs

**Current behavior**: `watch` can update the DB, but `mcp` doesn't know to invalidate
its `QueryCache`.

### 3. git stash / merge / rebase / pull — HIGH

Many files change atomically. Same phantom/missing/stale problems as branch switch.

### 4. File deletion outside editor — MEDIUM

Deleted files leave stale entries until next `index` or `watch` cycle.

### 5. Schema version mismatch — LOW

Already handled by `ensure_schema()`.

---

## Gaps

| Gap | Severity | Description |
|---|---|---|
| No git HEAD tracking | Critical | DB doesn't record which commit/branch was indexed |
| MCP is fire-and-forget | Critical | Opens DB once, never re-checks |
| watch and mcp are separate | High | `watch` updates DB but `mcp` doesn't invalidate QueryCache |
| No staleness indicator | Medium | Tool responses don't include freshness info |
| Quarantine is permanent | Medium | Files quarantined on one branch stay blacklisted on others |

---

## Design Decision: Single DB vs Per-Branch DB

### Option A — Single DB, incremental catch-up (RECOMMENDED)

One `.codetopo/index.sqlite` per repo across all branches. On branch switch,
`ChangeDetector` incrementally catches up via mtime+hash diff.

**Pros**: Simple, no disk proliferation, no orphan cleanup. `Persister::persist_file()`
cascade-deletes old data per file. `ChangeDetector::detect()` catches files in DB
that don't exist on disk.

**Cons**: Brief stale window during catch-up (~2-10s for typical branch switches).

### Option B — Per-branch DB

`.codetopo/index-main.sqlite`, `.codetopo/index-feature-x.sqlite`, etc.

**Pros**: Instant branch switch.

**Cons**: Massive disk usage (~2-5 GB per branch for 150K-file repos), requires orphan
cleanup on branch deletion (git hooks or LRU eviction), cold start on first branch visit.

**Decision**: Option A. The incremental catch-up is fast enough and eliminates all
branch-lifecycle complexity.

### Why Single-DB Needs No Branch Cleanup

With one DB per repo, branch deletion requires **zero cleanup**:

- `Persister::persist_file()` cascade-deletes old file data (nodes → edges, refs)
  before re-inserting the current branch's version
- `ChangeDetector::detect()` catches files in the DB that no longer exist on disk
  and marks them for deletion via `prune_deleted()`
- After switching away from `feature/x` to `main` and re-indexing, all of
  `feature/x`'s unique files are already pruned and all shared files are
  overwritten with `main`'s content
- When `git branch -d feature/x` runs, the DB already holds `main`'s data —
  there's nothing stale to clean up

---

## Re-indexing Strategy: Spawned Child vs In-Process

A critical design question: when the MCP server detects staleness, **how** does
it re-index?

### The problem with in-process re-indexing

The existing `cmd_watch.h` does in-process incremental re-indexing — it calls
`Parser::parse()` and `Extractor::extract()` directly. But the main
`codetopo index` command deliberately uses a **supervisor pattern**
(`supervisor.h`): it spawns itself as a child process with `--supervised` so
that if a file crashes the parser (SEH exception, stack overflow, deep AST
recursion), only the child dies. The supervisor detects the crash, quarantines
the offending file, and restarts.

If the MCP server re-indexes in-process and a malformed file crashes the parser,
**it takes down the MCP server**. VS Code will restart the stdio process, but
the user loses their session state.

### Decision: Spawn `codetopo index` as a child process

The MCP server never parses files itself. Instead, it spawns `codetopo index`
as a child process when re-indexing is needed:

```
codetopo index --root <root> --db <db> --supervised
```

**How it works**:
1. MCP server detects staleness (R2) or receives file-change events (R3/R4)
2. MCP server spawns `codetopo index` as a background child process
3. The child uses the full supervisor/quarantine machinery for crash safety
4. MCP server **continues serving from the existing DB** during re-index
   (with `"stale": true` flag in responses)
5. SQLite WAL mode means the reader (MCP) and writer (child indexer) don't
   block each other — concurrent read/write is safe
6. When the child exits with code 0, the MCP server calls `QueryCache::clear()`
7. Next tool call reads fresh data; `stale` flag clears

**Deduplication**: Only one child indexer runs at a time. If a re-index is
already in progress when a new trigger arrives, the trigger is queued and
collapsed — multiple rapid file changes don't spawn multiple children.

```cpp
// In McpServer — child process management
struct ReindexState {
    std::atomic<bool> running{false};
    std::atomic<bool> queued{false};     // another reindex requested while one runs
    std::thread monitor_thread;

    void trigger(const std::string& exe, const std::string& root, const std::string& db,
                 std::function<void()> on_complete) {
        if (running.exchange(true)) {
            queued = true;  // collapse into next run
            return;
        }
        monitor_thread = std::thread([=, this]() {
            do {
                queued = false;
                // Spawn child and wait for exit
                std::string cmd = exe + " index --root \"" + root
                    + "\" --db \"" + db + "\" --supervised";
                int rc = std::system(cmd.c_str());
                if (rc == 0) on_complete();
            } while (queued.load());
            running = false;
        });
        monitor_thread.detach();
    }
};
```

**Why not in-process for small changes?** Simplicity. One code path, one crash
boundary, predictable behavior. The child indexer's incremental mode is already
fast — `ChangeDetector` skips unchanged files in O(1) via mtime+size, so even a
"full" re-index after a single file edit takes < 1s on most repos.

---

## Recommendations

### R1: Record git HEAD in DB at index time

**Files**: `src/index/persister.h` (write_metadata), new `src/util/git.h`

Add a utility to capture git state:

```cpp
// src/util/git.h
#pragma once
#include <string>
#include <cstdio>
#include <array>

namespace codetopo {

// Run a git command and return trimmed stdout. Empty string on failure.
inline std::string git_command(const std::string& repo_root, const std::string& args) {
    std::string cmd = "git -C \"" + repo_root + "\" " + args;
#ifdef _WIN32
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) return "";
    std::array<char, 256> buf;
    std::string result;
    while (fgets(buf.data(), buf.size(), pipe)) {
        result += buf.data();
    }
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;
}

inline std::string get_git_head(const std::string& repo_root) {
    return git_command(repo_root, "rev-parse HEAD");
}

inline std::string get_git_branch(const std::string& repo_root) {
    return git_command(repo_root, "rev-parse --abbrev-ref HEAD");
}

} // namespace codetopo
```

Then in `Persister::write_metadata()`:

```cpp
auto head = get_git_head(repo_root);
auto branch = get_git_branch(repo_root);
if (!head.empty()) schema::set_kv(conn_, "git_head", head);
if (!branch.empty()) schema::set_kv(conn_, "git_branch", branch);
```

**Complexity**: Trivial
**Priority**: P0

---

### R2: Staleness check on MCP tool calls

**Files**: `src/mcp/server.h` (main loop)

Before dispatching any tool call, cheaply detect if the index might be stale:

1. **Fast path**: `stat(".git/HEAD")` — if mtime unchanged since last check, skip (sub-microsecond)
2. **If mtime changed**: read `.git/HEAD`, resolve to commit, compare with `git_head` in DB
3. **If mismatch**: set a `stale_` flag; include in tool responses:

```json
{
  "results": ["..."],
  "_meta": {
    "stale": true,
    "indexed_branch": "main",
    "indexed_commit": "abc1234",
    "current_branch": "feature/x"
  }
}
```

The MCP server tracks staleness state with minimal overhead:

```cpp
// In McpServer class
struct StalenessState {
    std::filesystem::file_time_type last_head_mtime{};
    std::string indexed_head;
    std::string indexed_branch;
    bool stale = false;
    std::string current_branch;
};
```

**Complexity**: Low
**Priority**: P1

---

### R3: Embed watcher inside MCP server + auto-reindex via spawned child

**Files**: `src/cli/cmd_mcp.h`, `src/mcp/server.h`

Add `--watch` flag to `mcp` subcommand. When active:

1. **Startup reconciliation** (R8): immediately spawn `codetopo index` to catch
   up with any changes that happened while the server wasn't running
2. Start a `Watcher` thread on the repo root (and `.git/HEAD`)
3. On file events (debounced), spawn `codetopo index` as a child process
4. MCP server continues serving stale data with `"stale": true` during re-index
5. When child exits 0, call `QueryCache::clear()` — next queries see fresh data
6. SQLite WAL mode means the reader (MCP) and writer (child) coexist safely

The MCP server **never parses files itself** — it delegates to the child indexer
which has full supervisor/quarantine crash protection.

Child deduplication: only one `codetopo index` runs at a time. Rapid changes
collapse into a single re-index (see `ReindexState` in the strategy section).

```
codetopo mcp --watch --root .
```

**Sequence diagram**:
```
User edits file.cpp
  → Watcher detects FILE_NOTIFY_CHANGE_LAST_WRITE
  → 1s debounce
  → MCP spawns: codetopo index --root . --db .codetopo/index.sqlite --supervised
  → MCP continues serving (stale=true)
  → Child: scan → detect 1 changed file → parse → persist → exit 0
  → MCP: QueryCache::clear(), stale=false
  → Next tool call returns fresh data
```

**Complexity**: Medium
**Priority**: P2

---

### R4: Branch switch detection via `.git/HEAD` watch

**Files**: `src/watch/watcher.h`, `src/mcp/server.h`

The watcher (R3) monitors `.git/HEAD` in addition to source files. When HEAD changes:

1. MCP server immediately sets `stale = true`
2. Spawn `codetopo index` child (same mechanism as R3)
3. The child runs `Scanner::scan()` via `git ls-files` (~2s on 150K files)
4. `ChangeDetector::detect()` diffs scan against DB — mtime+size fast path
   skips unchanged files in O(1)
5. Quarantine rehab runs (R7) — quarantined files with changed content get
   another chance
6. Child updates `git_head` and `git_branch` in kv table, exits 0
7. MCP server clears `QueryCache`, clears `stale` flag

Branch switches that change ~100 files take ~3-5s end-to-end. The MCP server
serves stale-but-honest responses during this window.

**Complexity**: Medium
**Priority**: P2

---

### R5: Freshness metadata in server_info and repo_stats

**Files**: `src/mcp/tools.cpp` (server_info, repo_stats)

Include in `server_info` response:

```json
{
  "index_age_seconds": 3600,
  "indexed_branch": "main",
  "indexed_commit": "abc1234...",
  "current_branch": "feature/x",
  "current_commit": "def4567...",
  "stale": true
}
```

This lets the AI client make informed decisions and can warn the user.

**Complexity**: Trivial
**Priority**: P0

---

### R6: QueryCache invalidation

**Files**: `src/db/queries.h`

Add a `clear()` method to `QueryCache`:

```cpp
void clear() {
    for (auto& [key, stmt] : cache_) {
        sqlite3_finalize(stmt);
    }
    cache_.clear();
}
```

Called after any re-index cycle completes (for R3). Without this, prepared
statements may return cached result sets even though SQLite WAL has new data.

**Complexity**: Trivial
**Priority**: P1

---

### R7: Quarantine rehab on branch switch

**Files**: `src/index/persister.h`, `src/db/schema.h`

A file quarantined because it crashed the parser on branch A might be perfectly
fine on branch B (different content, different size, maybe even a different
language). Currently, quarantine is permanent — once a file crashes the parser,
it stays blacklisted forever across **all** branches.

**Fix**: After a branch switch (detected by git HEAD change), give quarantined
files another chance if their content has changed:

```cpp
// In schema namespace
inline int rehab_quarantine(Connection& conn, const std::vector<ScannedFile>& scanned) {
    auto quarantined = load_quarantine(conn);
    if (quarantined.empty()) return 0;

    // Load current file records for quarantined paths
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(conn.raw(),
        "SELECT path, mtime_ns, size_bytes FROM files WHERE path = ?",
        -1, &stmt, nullptr);

    std::vector<std::string> rehab_paths;

    for (const auto& file : scanned) {
        if (!quarantined.count(file.relative_path)) continue;

        // Check if the file on disk is different from what was indexed
        sqlite3_reset(stmt);
        sqlite3_bind_text(stmt, 1, file.relative_path.c_str(), -1, SQLITE_TRANSIENT);

        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int64_t old_mtime = sqlite3_column_int64(stmt, 1);
            int64_t old_size = sqlite3_column_int64(stmt, 2);

            // If mtime or size changed, the file content is likely different
            if (old_mtime != file.mtime_ns || old_size != file.size_bytes) {
                rehab_paths.push_back(file.relative_path);
            }
        } else {
            // File wasn't in DB at all (new on this branch) — definitely rehab
            rehab_paths.push_back(file.relative_path);
        }
    }
    sqlite3_finalize(stmt);

    // Remove rehabilitated files from quarantine
    if (!rehab_paths.empty()) {
        sqlite3_stmt* del = nullptr;
        sqlite3_prepare_v2(conn.raw(),
            "DELETE FROM quarantine WHERE path = ?", -1, &del, nullptr);
        for (const auto& path : rehab_paths) {
            sqlite3_reset(del);
            sqlite3_bind_text(del, 1, path.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(del);
        }
        sqlite3_finalize(del);
    }

    return static_cast<int>(rehab_paths.size());
}
```

Call this in `run_index()` after scanning, before building the work list:

```cpp
// After scanning, before building work list
auto old_head = schema::get_kv(conn, "git_head", "");
auto new_head = get_git_head(repo_root.string());
bool head_changed = (!old_head.empty() && !new_head.empty() && old_head != new_head);

if (head_changed) {
    int rehabbed = schema::rehab_quarantine(conn, scanned_files);
    if (rehabbed > 0)
        std::cerr << "Quarantine rehab: " << rehabbed
                  << " file(s) given another chance after branch switch\n";
}
```

This ensures quarantine doesn't become a permanent cross-branch blacklist. Files
that still crash on the new branch will simply be re-quarantined by the supervisor.

**Complexity**: Low
**Priority**: P1

---

### R8: Startup reconciliation — catch up on missed changes

**Files**: `src/cli/cmd_mcp.h`, `src/mcp/server.h`

When the MCP server starts (especially with `--watch`), it may be launching after
a gap during which files were edited, branches switched, or commits pulled.
The watcher only captures events that happen **after** it starts — it cannot
recover changes that occurred while it wasn't running.

**Fix**: On MCP startup, spawn a `codetopo index` child to reconcile. The
aggression of this catch-up is governed by a **freshness policy** (see R9) that
lets the user trade startup latency for index accuracy. The default is tuned
for a good experience on large codebases.

The spawned child uses the existing `ChangeDetector` which already does exactly
the right thing:
1. Loads every `(path, mtime_ns, size_bytes, content_hash)` from the `files` table
2. Runs `Scanner::scan()` via `git ls-files` to enumerate current disk state
3. Compares the two sets:
   - **mtime+size unchanged** → skip (O(1) per file, no I/O)
   - **mtime or size changed** → compute xxHash64, compare with stored hash
   - **In DB but not on disk** → mark as deleted
   - **On disk but not in DB** → mark as new
4. Produces exact `new_files` / `changed_files` / `deleted_paths` lists
5. `Persister` applies the delta

This works regardless of what caused the drift:
- Uncommitted file edits (different mtime/size)
- Branch switches (different files, different content)
- `git stash pop` (same HEAD, different working tree)
- `git pull` / `git merge` (new commits, changed files)
- Files created or deleted outside the editor

**Startup sequence with `--watch`**:
```
1. MCP server starts, opens DB read-only
2. Immediately spawns: codetopo index --root . --db .codetopo/index.sqlite --supervised
3. MCP server begins accepting tool calls (stale=true while child runs)
4. Child: scan → detect deltas → reindex changed files → exit 0  
5. MCP: QueryCache::clear(), stale=false
6. Watcher thread starts — captures all future changes from this point on
```

**Without `--watch`**: R2 still detects git HEAD mismatch and flags `stale=true`
in responses, but no automatic re-index occurs. The user would need to run
`codetopo index` manually. This is the `--freshness=lazy` mode for users who
prefer explicit control and fastest possible startup.

**Cost**: The startup reindex is cheap when nothing changed — `ChangeDetector`
scans 150K files in ~2s via `git ls-files`, and the mtime+size fast path means
zero files are actually re-parsed if nothing drifted. For a typical "stop MCP,
edit 5 files, start MCP" scenario, the catch-up takes < 3s.

However, on very large codebases (150K+ files, slow I/O, network mounts), even
the scan phase can take 5-15s, which delays MCP readiness. The freshness policy
(R9) lets the user choose the right trade-off.

**Complexity**: Low (reuses `ReindexState.trigger()` from the strategy section)
**Priority**: P1

---

### R9: Configurable freshness policy

**Files**: `src/core/config.h`, `src/main.cpp`, `src/cli/cmd_mcp.h`, `src/mcp/server.h`

Different users and codebases need different trade-offs between startup speed
and index accuracy. A single `--freshness` setting controls the policy:

```
codetopo mcp --watch --freshness=eager    # full reindex at startup + live watch
codetopo mcp --watch --freshness=normal   # deferred reindex + live watch (default)
codetopo mcp --watch --freshness=lazy     # no startup reindex, watch only
codetopo mcp --freshness=off              # no watch, no reindex, stale flag only
```

| Mode | Startup reindex | File watcher | Branch detect | Staleness flag | Best for |
|---|---|---|---|---|---|
| `eager` | **Blocking** — child must finish before first tool call | Yes | Yes | Yes | Small/medium repos (<50K files) where users want guaranteed-fresh on first query |
| `normal` | **Deferred** — child runs in background, first queries may be stale | Yes | Yes | Yes | Large repos. Default. First query arrives instantly (stale=true), fresh within seconds |
| `lazy` | **None** — no startup reindex | Yes | Yes | Yes | Huge repos on slow I/O where even scanning is costly. Watcher catches future changes only |
| `off` | **None** | No | No | Yes (R2 only) | Maximum control. User runs `codetopo index` manually. MCP only flags staleness |

**Config addition**:

```cpp
// In Config struct
enum class FreshnessPolicy { eager, normal, lazy, off };
FreshnessPolicy freshness = FreshnessPolicy::normal;
```

**CLI wiring**:

```cpp
std::string freshness_str = "normal";
sub_mcp->add_option("--freshness", freshness_str,
    "Index freshness policy: eager|normal|lazy|off (default: normal)")
    ->default_val("normal");
```

**Behavior by mode**:

- **`eager`**: On startup, spawn `codetopo index` and **block** the stdio loop
  until it exits. Only then call `handle_initialize()`. The client sees no
  staleness — every response is guaranteed fresh from the first query.
  Trade-off: MCP server takes 3-30s to become ready depending on repo size.

- **`normal`** (default): On startup, spawn `codetopo index` in the background
  via `ReindexState.trigger()`. The stdio loop starts immediately. First few
  tool calls may return `"stale": true`. When the child finishes,
  `QueryCache::clear()` runs and subsequent calls are fresh. Best balance for
  most repos.

- **`lazy`**: Skip startup reindex entirely. Start the watcher immediately.
  Only changes **after** MCP starts are caught. Any drift from before startup
  remains until a file in the stale set is touched again (watcher picks it up)
  or the user runs `codetopo index` manually. Best for huge repos on NFS/CIFS
  where `git ls-files` itself takes 10+ seconds.

- **`off`**: No watcher, no startup reindex. R2 still detects git HEAD mismatch
  and flags `stale=true`. Purely passive. User runs `codetopo index` when they
  want to refresh.

**Debounce tuning** (for `eager`/`normal`/`lazy` with `--watch`):

```
codetopo mcp --watch --freshness=normal --debounce=2000
```

The `--debounce` setting (milliseconds, default 1000) controls how long the
watcher waits after the last file event before triggering re-index. Larger
values reduce re-index frequency on rapid edits at the cost of slightly
staler data. On huge repos, `--debounce=5000` prevents re-index storms during
large operations like `git checkout` (which fires hundreds of events).

**Complexity**: Low
**Priority**: P1

---

## Implementation Priority

| Priority | Item | Files | Complexity |
|---|---|---|---|
| **P0** | R1 — Git HEAD/branch in kv | `persister.h`, new `util/git.h` | Trivial |
| **P0** | R5 — Freshness in server_info | `tools.cpp` | Trivial |
| **P1** | R2 — Staleness flag on tool calls | `server.h` | Low |
| **P1** | R6 — QueryCache::clear() | `queries.h` | Trivial |
| **P1** | R7 — Quarantine rehab on branch switch | `schema.h`, `cmd_index.cpp` | Low |
| **P1** | R8 — Startup reconciliation | `cmd_mcp.h`, `server.h` | Low |
| **P1** | R9 — Configurable freshness policy | `config.h`, `cmd_mcp.h`, `main.cpp` | Low |
| **P2** | R3 — Embed watcher in MCP + startup reindex | `cmd_mcp.h`, `server.h` | Medium |
| **P2** | R4 — Branch switch detection | `watcher.h` | Medium |
