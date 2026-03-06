# codetopo — Gaps & Future Work

## Branch-aware indexing

Currently codetopo indexes files on disk with no knowledge of git branches.

### What's missing
- `index_summary` should report the current branch name and commit hash
- On branch switch, the index becomes stale — no automatic detection
- No per-branch index snapshots — switching branches requires full re-index

### Proposed design
- Read `.git/HEAD` to detect current branch at index time
- Store branch name + commit hash in the `kv` table
- `index_summary` returns `"branch": "main"`, `"commit": "af01a45"`
- On re-index, detect if branch changed → optionally snapshot the old index
- Future: `.codetopo/branches/<branch>.sqlite` for per-branch snapshots

### Why it matters
- Developers switch branches frequently — stale indexes are confusing
- AI agents need to know which branch they're exploring
- Diff between branches could show architectural changes (future)
