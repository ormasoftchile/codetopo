---
name: "sqlite-bulk-load"
description: "SQLite PRAGMA tuning and schema patterns for high-throughput bulk insert at 100K+ row scale"
domain: "performance, database"
confidence: "high"
source: "earned — root-cause analysis of 6x persist degradation at 100K files"
---

## Context

When bulk-inserting into SQLite databases that grow beyond ~200MB (100K+ files, millions of rows), default PRAGMAs and schema constraints cause severe per-row cost growth. This skill covers the PRAGMA stack, index management, and constraint handling needed for bulk-insert throughput.

## Patterns

### PRAGMA Stack for Bulk Insert
```cpp
// Connection defaults (always set):
PRAGMA journal_mode=WAL;
PRAGMA busy_timeout=5000;
PRAGMA synchronous=NORMAL;      // safe default
PRAGMA cache_size=-65536;       // 64MB
PRAGMA mmap_size=268435456;     // 256MB

// Bulk/turbo mode (set before bulk insert begins):
PRAGMA synchronous=OFF;         // skip WAL fsync — safe against app crash, not OS crash
PRAGMA wal_autocheckpoint=0;    // no mid-run checkpoints — single checkpoint at end
PRAGMA temp_store=MEMORY;       // temp tables in RAM
PRAGMA cache_size=-131072;      // 128MB page cache
PRAGMA mmap_size=2147483648;    // 2GB — must cover expected final DB size
PRAGMA foreign_keys=OFF;        // disable FK checks during bulk insert
```

### Index Management Pattern
1. **Drop** secondary indexes before bulk insert (`DROP INDEX IF EXISTS ...`)
2. Column-level UNIQUE constraints (e.g., `stable_key TEXT UNIQUE`) **cannot be dropped** — they survive `DROP INDEX`. For highest throughput, create tables without UNIQUE during bulk, then add constraint after.
3. Rebuild indexes **after** bulk insert — sequential sort is 10-50x faster than per-row B-tree maintenance.
4. Split indexes into **read-path** (needed by post-insert processing) and **write-path** (needed only for queries). Build read-path first, run processing, then build write-path.

### mmap_size Sizing Rule
Set mmap_size ≥ expected final database size. When the DB exceeds the mmap window, every B-tree page access beyond the window falls back to `read()` system calls. This is the primary cause of non-linear per-row cost growth.

### Batch COMMIT Sizing
- batch_size=500 is fine for <10K rows
- batch_size=1000-5000 for 10K-100K rows
- batch_size=5000-50000 for 100K+ rows
- With synchronous=OFF, per-COMMIT cost is lower, but journal bookkeeping still has fixed overhead.

## Examples

From codetopo: `src/db/connection.h` (enable_turbo), `src/db/schema.h` (drop_bulk_indexes/rebuild_indexes), `src/cli/cmd_index.cpp` (bulk_mode flag gating).

## Anti-Patterns

- **Dead flag bug:** Setting a config flag (`config.turbo = true`) without calling the function that applies the PRAGMAs. Always verify flags reach their codepath with a test or log.
- **DROP INDEX doesn't drop UNIQUE column constraints.** They're part of the table definition, not secondary indexes.
- **FK checks during bulk insert with self-generated IDs.** When all FKs reference IDs from `last_insert_rowid()`, FK violations are structurally impossible. Disable them.
- **Small mmap_size on large databases.** 256MB mmap on a 2GB database means 87% of page accesses use slow `read()` calls instead of memory-mapped access.
