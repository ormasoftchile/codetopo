#pragma once

#include "db/connection.h"
#include "index/scanner.h"
#include <string>
#include <unordered_set>
#include <ctime>
#include <vector>

namespace codetopo {

// T011: Schema DDL creation and version check logic per data-model.md.
// Schema version 1 = initial schema.
// Schema version 5 = roots table + root_id on files (unified workspace).
// Schema version 6 = call-ref metadata for approximate callgraph narrowing.
static constexpr int CURRENT_SCHEMA_VERSION = 6;
static constexpr const char* INDEXER_VERSION = "1.5.1";

namespace schema {

inline void create_tables(Connection& conn) {
    // roots table: extra workspace roots merged into this DB.
    // id=0 is implicit (main project, files with root_id IS NULL).
    conn.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS roots (
            id       INTEGER PRIMARY KEY,
            path     TEXT NOT NULL UNIQUE,
            added_at TEXT NOT NULL
        );
    )SQL");

    conn.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS files (
            id INTEGER PRIMARY KEY,
            path TEXT UNIQUE NOT NULL,
            language TEXT NOT NULL CHECK(language IN ('c','cpp','csharp','typescript','javascript','python','rust','java','go','bash','sql','yaml')),
            size_bytes INTEGER NOT NULL,
            mtime_ns INTEGER NOT NULL,
            content_hash TEXT NOT NULL,
            parse_status TEXT NOT NULL CHECK(parse_status IN ('ok','partial','failed','skipped')),
            parse_error TEXT,
            root_id INTEGER REFERENCES roots(id) ON DELETE CASCADE
        );
        CREATE INDEX IF NOT EXISTS idx_files_content_hash ON files(content_hash);
        CREATE INDEX IF NOT EXISTS idx_files_root ON files(root_id);
    )SQL");

    conn.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS nodes (
            id INTEGER PRIMARY KEY,
            node_type TEXT NOT NULL CHECK(node_type IN ('file','symbol')),
            file_id INTEGER REFERENCES files(id) ON DELETE CASCADE,
            kind TEXT NOT NULL,
            name TEXT NOT NULL,
            qualname TEXT,
            signature TEXT,
            start_line INTEGER,
            start_col INTEGER,
            end_line INTEGER,
            end_col INTEGER,
            is_definition INTEGER NOT NULL DEFAULT 1,
            visibility TEXT CHECK(visibility IN ('public','protected','private') OR visibility IS NULL),
            doc TEXT,
            stable_key TEXT NOT NULL
        );
        CREATE UNIQUE INDEX IF NOT EXISTS idx_nodes_stable_key ON nodes(stable_key);
        CREATE INDEX IF NOT EXISTS idx_nodes_file_id ON nodes(file_id);
        CREATE INDEX IF NOT EXISTS idx_nodes_type_kind_name ON nodes(node_type, kind, name);
        CREATE INDEX IF NOT EXISTS idx_nodes_qualname ON nodes(qualname);
        CREATE INDEX IF NOT EXISTS idx_nodes_name_type ON nodes(name, node_type);
    )SQL");

    conn.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS refs (
            id INTEGER PRIMARY KEY,
            file_id INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,
            kind TEXT NOT NULL CHECK(kind IN ('call','type_ref','include','inherit','field_access','other')),
            name TEXT NOT NULL,
            start_line INTEGER,
            start_col INTEGER,
            end_line INTEGER,
            end_col INTEGER,
            resolved_node_id INTEGER REFERENCES nodes(id) ON DELETE SET NULL,
            evidence TEXT,
            containing_node_id INTEGER REFERENCES nodes(id) ON DELETE SET NULL,
            arg_count INTEGER,
            arg_pattern TEXT,
            receiver_type_hint TEXT
        );
        CREATE INDEX IF NOT EXISTS idx_refs_file_id ON refs(file_id);
        CREATE INDEX IF NOT EXISTS idx_refs_kind_name ON refs(kind, name);
        CREATE INDEX IF NOT EXISTS idx_refs_resolved ON refs(resolved_node_id);
        CREATE INDEX IF NOT EXISTS idx_refs_containing ON refs(containing_node_id);
    )SQL");

    conn.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS edges (
            id INTEGER PRIMARY KEY,
            src_id INTEGER NOT NULL REFERENCES nodes(id) ON DELETE CASCADE,
            dst_id INTEGER NOT NULL REFERENCES nodes(id) ON DELETE CASCADE,
            kind TEXT NOT NULL CHECK(kind IN ('calls','includes','inherits','references','contains')),
            confidence REAL NOT NULL DEFAULT 1.0 CHECK(confidence >= 0.3),
            evidence TEXT
        );
        CREATE INDEX IF NOT EXISTS idx_edges_src ON edges(src_id, kind);
        CREATE INDEX IF NOT EXISTS idx_edges_dst ON edges(dst_id, kind);
    )SQL");

    conn.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS kv (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL
        );
    )SQL");

    conn.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS quarantine (
            path TEXT PRIMARY KEY,
            crash_count INTEGER NOT NULL DEFAULT 1,
            first_crash TEXT NOT NULL,
            last_crash TEXT NOT NULL,
            error TEXT
        );
    )SQL");
}

inline void create_fts(Connection& conn) {
    conn.exec(R"SQL(
        CREATE VIRTUAL TABLE IF NOT EXISTS nodes_fts USING fts5(
            name, qualname, signature, doc,
            content='nodes', content_rowid='id'
        );
    )SQL");
}

// Content FTS: trigram index for arbitrary substring search across source files.
// Uses contentless-delete so we can update/delete without storing full content in SQLite.
// file_id is UNINDEXED but stored (contentless_unindexed=1) for joining back to files table.
inline void create_content_fts(Connection& conn) {
    conn.exec(R"SQL(
        CREATE VIRTUAL TABLE IF NOT EXISTS content_fts USING fts5(
            content,
            file_id UNINDEXED,
            line_no UNINDEXED,
            tokenize="trigram",
            content='',
            contentless_delete=1,
            contentless_unindexed=1
        );
    )SQL");
    // Tracker table: contentless FTS5 tables cannot be scanned without MATCH,
    // so we track which file_ids have been indexed in a regular table.
    conn.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS content_fts_tracker (
            file_id INTEGER PRIMARY KEY
        );
    )SQL");
}

// Check schema version. Returns:
//   0 = fresh DB (no kv table)
//   positive = version number
//   -1 = error
inline int get_schema_version(Connection& conn) {
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(conn.raw(),
        "SELECT value FROM kv WHERE key='schema_version'", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return 0;  // Table doesn't exist yet → fresh DB

    int version = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        version = std::atoi(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
    }
    sqlite3_finalize(stmt);
    return version;
}

inline void set_kv(Connection& conn, const std::string& key, const std::string& value) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(conn.raw(),
        "INSERT OR REPLACE INTO kv(key, value) VALUES(?, ?)", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

inline std::string get_kv(Connection& conn, const std::string& key, const std::string& def = "") {
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(conn.raw(),
        "SELECT value FROM kv WHERE key=?", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return def;
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    std::string result = def;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        result = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return result;
}

inline bool table_has_column(Connection& conn, const char* table, const char* column) {
    sqlite3_stmt* probe = nullptr;
    std::string sql = std::string("PRAGMA table_info(") + table + ")";
    bool found = false;
    sqlite3_prepare_v2(conn.raw(), sql.c_str(), -1, &probe, nullptr);
    while (sqlite3_step(probe) == SQLITE_ROW) {
        const char* col = reinterpret_cast<const char*>(sqlite3_column_text(probe, 1));
        if (col && std::string(col) == column) { found = true; break; }
    }
    sqlite3_finalize(probe);
    return found;
}

inline bool table_exists(Connection& conn, const char* table) {
    sqlite3_stmt* stmt = nullptr;
    bool exists = false;
    sqlite3_prepare_v2(conn.raw(),
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=? LIMIT 1", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, table, -1, SQLITE_TRANSIENT);
    exists = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return exists;
}

inline void ensure_refs_call_metadata_schema(Connection& conn) {
    if (!table_exists(conn, "refs")) return;
    if (!table_has_column(conn, "refs", "arg_count"))
        conn.exec("ALTER TABLE refs ADD COLUMN arg_count INTEGER");
    if (!table_has_column(conn, "refs", "arg_pattern"))
        conn.exec("ALTER TABLE refs ADD COLUMN arg_pattern TEXT");
    if (!table_has_column(conn, "refs", "receiver_type_hint"))
        conn.exec("ALTER TABLE refs ADD COLUMN receiver_type_hint TEXT");
}

// Drop secondary indexes for bulk loading. Leaves PRIMARY KEY and UNIQUE constraints.
inline void drop_bulk_indexes(Connection& conn) {
    conn.exec("DROP INDEX IF EXISTS idx_files_content_hash");
    conn.exec("DROP INDEX IF EXISTS idx_files_root");
    conn.exec("DROP INDEX IF EXISTS idx_nodes_file_id");
    conn.exec("DROP INDEX IF EXISTS idx_nodes_type_kind_name");
    conn.exec("DROP INDEX IF EXISTS idx_nodes_qualname");
    conn.exec("DROP INDEX IF EXISTS idx_nodes_name_type");
    conn.exec("DROP INDEX IF EXISTS idx_refs_file_id");
    conn.exec("DROP INDEX IF EXISTS idx_refs_kind_name");
    conn.exec("DROP INDEX IF EXISTS idx_refs_resolved");
    conn.exec("DROP INDEX IF EXISTS idx_refs_containing");
    conn.exec("DROP INDEX IF EXISTS idx_edges_src");
    conn.exec("DROP INDEX IF EXISTS idx_edges_dst");
    conn.exec("DROP INDEX IF EXISTS idx_nodes_stable_key");
}

// Rebuild secondary indexes after bulk loading.
inline void rebuild_indexes(Connection& conn) {
    conn.exec("CREATE INDEX IF NOT EXISTS idx_files_content_hash ON files(content_hash)");
    conn.exec("CREATE INDEX IF NOT EXISTS idx_files_root ON files(root_id)");
    conn.exec("CREATE INDEX IF NOT EXISTS idx_nodes_file_id ON nodes(file_id)");
    conn.exec("CREATE INDEX IF NOT EXISTS idx_nodes_type_kind_name ON nodes(node_type, kind, name)");
    conn.exec("CREATE INDEX IF NOT EXISTS idx_nodes_qualname ON nodes(qualname)");
    conn.exec("CREATE INDEX IF NOT EXISTS idx_nodes_name_type ON nodes(name, node_type)");
    conn.exec("CREATE INDEX IF NOT EXISTS idx_refs_file_id ON refs(file_id)");
    conn.exec("CREATE INDEX IF NOT EXISTS idx_refs_kind_name ON refs(kind, name)");
    conn.exec("CREATE INDEX IF NOT EXISTS idx_refs_resolved ON refs(resolved_node_id)");
    conn.exec("CREATE INDEX IF NOT EXISTS idx_refs_containing ON refs(containing_node_id)");
    conn.exec("CREATE INDEX IF NOT EXISTS idx_edges_src ON edges(src_id, kind)");
    conn.exec("CREATE INDEX IF NOT EXISTS idx_edges_dst ON edges(dst_id, kind)");
    conn.exec("CREATE UNIQUE INDEX IF NOT EXISTS idx_nodes_stable_key ON nodes(stable_key)");
}

// Initialize or verify schema. Returns exit code (0=ok, 3=mismatch).
inline int ensure_schema(Connection& conn) {
    int version = get_schema_version(conn);

    if (version == 0) {
        // Fresh DB
        create_tables(conn);
        create_fts(conn);
        create_content_fts(conn);
        set_kv(conn, "schema_version", std::to_string(CURRENT_SCHEMA_VERSION));
        return 0;
    }

    if (version == CURRENT_SCHEMA_VERSION) {
        return 0;  // Compatible
    }

    if (version > CURRENT_SCHEMA_VERSION) {
        return 3;  // Newer version — abort
    }

    // v3→v4: content_fts gained line_no column. Preserve structural index,
    // only recreate content_fts + tracker. Backfill will repopulate.
    if (version == 3) {
        conn.exec("DROP TABLE IF EXISTS content_fts_tracker");
        conn.exec("DROP TABLE IF EXISTS content_fts");
        // FTS5 shadow tables may survive DROP on contentless tables
        conn.exec("DROP TABLE IF EXISTS content_fts_content");
        conn.exec("DROP TABLE IF EXISTS content_fts_data");
        conn.exec("DROP TABLE IF EXISTS content_fts_idx");
        conn.exec("DROP TABLE IF EXISTS content_fts_docsize");
        conn.exec("DROP TABLE IF EXISTS content_fts_config");
        create_content_fts(conn);
        // Fall through to v4→v5 migration below
        version = 4;
    }

    // v4→v5: add roots table + root_id column on files (unified workspace).
    // Additive migration — existing data is preserved; new column is nullable.
    if (version == 4) {
        // Create roots table
        conn.exec(R"SQL(
            CREATE TABLE IF NOT EXISTS roots (
                id       INTEGER PRIMARY KEY,
                path     TEXT NOT NULL UNIQUE,
                added_at TEXT NOT NULL
            );
        )SQL");

        // Add root_id column to files (nullable for backward compat — NULL means main project)
        // Use a probe to check if the column already exists (idempotent)
        {
            sqlite3_stmt* probe = nullptr;
            bool has_root_id = false;
            sqlite3_prepare_v2(conn.raw(), "PRAGMA table_info(files)", -1, &probe, nullptr);
            while (sqlite3_step(probe) == SQLITE_ROW) {
                const char* col = reinterpret_cast<const char*>(sqlite3_column_text(probe, 1));
                if (col && std::string(col) == "root_id") { has_root_id = true; break; }
            }
            sqlite3_finalize(probe);
            if (!has_root_id) {
                conn.exec("ALTER TABLE files ADD COLUMN root_id INTEGER REFERENCES roots(id) ON DELETE CASCADE");
            }
        }
        conn.exec("CREATE INDEX IF NOT EXISTS idx_files_root ON files(root_id)");

        version = 5;
    }

    // v5→v6: add lightweight call-site metadata to refs.
    // Additive migration — existing rows keep NULL metadata and are still usable.
    if (version == 5) {
        ensure_refs_call_metadata_schema(conn);
        set_kv(conn, "schema_version", std::to_string(CURRENT_SCHEMA_VERSION));
        return 0;
    }

    // Older version — recreate from scratch
    // Drop all tables and recreate
    conn.exec("DROP TABLE IF EXISTS content_fts_tracker");
    conn.exec("DROP TABLE IF EXISTS content_fts");
    conn.exec("DROP TABLE IF EXISTS nodes_fts");
    conn.exec("DROP TABLE IF EXISTS edges");
    conn.exec("DROP TABLE IF EXISTS refs");
    conn.exec("DROP TABLE IF EXISTS nodes");
    conn.exec("DROP TABLE IF EXISTS files");
    conn.exec("DROP TABLE IF EXISTS kv");
    conn.exec("DROP TABLE IF EXISTS quarantine");

    create_tables(conn);
    create_fts(conn);
    create_content_fts(conn);
    set_kv(conn, "schema_version", std::to_string(CURRENT_SCHEMA_VERSION));
    return 0;
}

// Ensure quarantine table exists (additive migration — safe to call on any schema version)
inline void ensure_quarantine_table(Connection& conn) {
    conn.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS quarantine (
            path TEXT PRIMARY KEY,
            crash_count INTEGER NOT NULL DEFAULT 1,
            first_crash TEXT NOT NULL,
            last_crash TEXT NOT NULL,
            error TEXT
        );
    )SQL");
}

// Load all quarantined file paths into a set
inline std::unordered_set<std::string> load_quarantine(Connection& conn) {
    std::unordered_set<std::string> result;
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(conn.raw(),
        "SELECT path FROM quarantine", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        result.insert(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
    }
    sqlite3_finalize(stmt);
    return result;
}

// Add a file to quarantine (or increment crash_count if already there)
inline void quarantine_file(Connection& conn, const std::string& path, const std::string& error = "") {
    auto now = std::to_string(std::time(nullptr));
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(conn.raw(),
        "INSERT INTO quarantine(path, crash_count, first_crash, last_crash, error) "
        "VALUES(?, 1, datetime('now'), datetime('now'), ?) "
        "ON CONFLICT(path) DO UPDATE SET "
        "crash_count = crash_count + 1, last_crash = datetime('now'), error = ?",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, error.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, error.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

// Get quarantine count
inline int quarantine_count(Connection& conn) {
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(conn.raw(),
        "SELECT COUNT(*) FROM quarantine", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return 0;
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

// Rehabilitate quarantined files whose content has changed (e.g. after branch switch).
// Returns count of files removed from quarantine.
inline int rehab_quarantine(Connection& conn, const std::vector<ScannedFile>& scanned) {
    auto quarantined = load_quarantine(conn);
    if (quarantined.empty()) return 0;

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(conn.raw(),
        "SELECT path, mtime_ns, size_bytes FROM files WHERE path = ?",
        -1, &stmt, nullptr);

    std::vector<std::string> rehab_paths;

    for (const auto& file : scanned) {
        if (!quarantined.count(file.relative_path)) continue;

        sqlite3_reset(stmt);
        sqlite3_bind_text(stmt, 1, file.relative_path.c_str(), -1, SQLITE_TRANSIENT);

        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int64_t old_mtime = sqlite3_column_int64(stmt, 1);
            int64_t old_size = sqlite3_column_int64(stmt, 2);

            if (old_mtime != file.mtime_ns || old_size != file.size_bytes) {
                rehab_paths.push_back(file.relative_path);
            }
        } else {
            // File not in DB (new on this branch) — rehab
            rehab_paths.push_back(file.relative_path);
        }
    }
    sqlite3_finalize(stmt);

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

// Clear all quarantine entries (used on fresh init to reset stale state)
inline void clear_quarantine(Connection& conn) {
    conn.exec("DELETE FROM quarantine");
}

// Ensure roots table and root_id column exist in an existing DB.
// Idempotent — safe to call on any DB version (including fresh v5 DBs).
// Used by WorkspaceDB to make the workspace tables available without running
// the full ensure_schema() migration.
inline void ensure_roots_schema(Connection& conn) {
    conn.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS roots (
            id       INTEGER PRIMARY KEY,
            path     TEXT NOT NULL UNIQUE,
            added_at TEXT NOT NULL
        );
    )SQL");

    // Add root_id column if not present (ALTER TABLE is idempotent via try/ignore)
    bool has_root_id = false;
    sqlite3_stmt* probe = nullptr;
    sqlite3_prepare_v2(conn.raw(), "PRAGMA table_info(files)", -1, &probe, nullptr);
    while (sqlite3_step(probe) == SQLITE_ROW) {
        const char* col = reinterpret_cast<const char*>(sqlite3_column_text(probe, 1));
        if (col && std::string(col) == "root_id") { has_root_id = true; break; }
    }
    sqlite3_finalize(probe);
    if (!has_root_id) {
        conn.exec("ALTER TABLE files ADD COLUMN root_id INTEGER REFERENCES roots(id) ON DELETE CASCADE");
    }
    conn.exec("CREATE INDEX IF NOT EXISTS idx_files_root ON files(root_id)");
    ensure_refs_call_metadata_schema(conn);
}

} // namespace schema
} // namespace codetopo
