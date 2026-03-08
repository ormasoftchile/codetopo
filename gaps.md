# codetopo — Branch-Aware Indexing

## Problem

codetopo indexes files on disk with no knowledge of git branches.
On branch switch the index goes stale silently — agents and humans both get misleading results.

---

## Phase 0 — Confidence Raiser Results

Benchmarked on 2026-03-06 using the codetopo repo itself (48 files, 2,132 symbols, 927 edges, 1.4 MB index).

### 0.1 Incremental Re-Index on Branch Switch

| Direction | Changed files | Wall clock |
|-----------|--------------|------------|
| `feat/codetopo-dir-convention` → `main` | 2 of 48 | **0.63 s** |
| `main` → `feat/codetopo-dir-convention` | 2 of 48 | **0.42 s** |

**Verdict**: Sub-second. The existing `change_detector` (mtime + xxHash) handles branch switches with zero new code.
For DsMainDev (144K files), a typical feature branch touching ~50-200 files should re-index in seconds — the expensive part is stat-scanning all files, not parsing.

> **Decision**: Per-branch snapshots are not justified for small-to-medium repos. Defer to Phase 2 only if DsMainDev benchmark exceeds 60 s.

### 0.2 Git Worktree Independence

| Check | Result |
|-------|--------|
| Worktree gets own `.codetopo/index.sqlite` | **Yes** |
| Original keeps its own `.codetopo/index.sqlite` | **Yes** |
| Indexes have different content (SHA-256) | **Yes** |
| Full index of worktree (48 files, cold) | **1.54 s** |

**Verdict**: Git worktrees are a zero-code solution for power users who need simultaneous branch indexes.
Each worktree is a separate directory → separate `.codetopo/` → no conflicts. Document as recommended workflow.

### 0.3 Delta Size Between Branch Indexes

| Metric | Value |
|--------|-------|
| Total SQLite pages (4 KB each) | 362 |
| Different pages | **2** |
| Delta | **0.6 %** (8 KB of 1,448 KB) |

**Verdict**: Almost identical. Even if snapshots were needed, delta-based storage (e.g., `sqldiff`) would be far more efficient than full copies.

---

## Action Plan

### Phase 1 — Branch Metadata (cheap, do regardless)

| # | Task | Detail |
|---|------|--------|
| 1.1 | **Read branch + commit at index time** | Call `git rev-parse --abbrev-ref HEAD` and `git rev-parse HEAD` (subprocess, not raw `.git/HEAD` parsing — handles detached HEAD, worktrees, submodules). Store as kv keys `branch` and `commit`. |
| 1.2 | **Surface in `index_summary`** | Add `"branch"` and `"commit"` fields to the MCP JSON response. |
| 1.3 | **Staleness flag** | On any MCP tool call, compare stored commit with live `git rev-parse HEAD`. If they differ, include `"stale": true` in the response so agents know to re-index. |

### Phase 2 — Per-Branch Snapshots (only if DsMainDev benchmark exceeds 60 s)

| # | Task | Detail |
|---|------|--------|
| 2.1 | **Snapshot on branch change** | When `index` detects a different branch in kv vs. current HEAD, copy (or `VACUUM INTO`) the current DB to `.codetopo/snapshots/<sanitized-branch>.sqlite` before re-indexing. Sanitize branch names: replace `/` with `--` (e.g., `dev--ormasoftchile--2026--03--06--fix.sqlite`). |
| 2.2 | **Restore on branch return** | On `index`, if a snapshot exists for the current branch, restore it and run incremental diff on top (reconcile mtime changes since snapshot). This is the existing `change_detector` codepath. |
| 2.3 | **LRU eviction** | Keep max **3** snapshots (configurable). Evict oldest by mtime. At 500 MB per index (DsMainDev scale), cap is ~1.5 GB. |

### Phase 3 — Auto-Trigger (polish)

| # | Task | Detail |
|---|------|--------|
| 3.1 | **Watch `.git/HEAD`** | Add `.git/HEAD` to the file-watcher in `cmd_watch.h`. On change, trigger incremental re-index. |
| 3.2 | **VS Code hook** | In codetopo-viz, listen to git extension events to prompt/auto re-index on branch switch. |

---

## Design Constraints

- **Disk budget**: Per-branch snapshots are full SQLite copies. For large repos (~500 MB per index), LRU cap of 3 keeps total under ~2 GB. Phase 0.3 data suggests delta storage could cut this to ~10 MB per snapshot.
- **Concurrency**: MCP server may be serving queries during re-index. SQLite WAL mode handles concurrent reads. Snapshot swap must use copy-then-swap, not in-place rename.
- **Branch name safety**: Names like `dev/owner/2026/03/06/desc` must not create nested directories. Sanitize with `--` separator.
- **Detached HEAD**: `git rev-parse --abbrev-ref HEAD` returns `HEAD` when detached. Store raw SHA as branch name.
- **Git worktrees**: Each worktree gets independent `.codetopo/`. No special handling needed — document as recommended alternative.

## Out of Scope (Future)

- Cross-branch architectural diff (compare symbol graphs between snapshots)
- Merge-base aware indexing (index only files changed since merge-base)
- Remote branch indexing without checkout
