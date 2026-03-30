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
static constexpr int CURRENT_SCHEMA_VERSION = 1;
static constexpr const char* INDEXER_VERSION = "1.0.0";

namespace schema {

inline void create_tables(Connection& conn) {
    conn.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS files (
            id INTEGER PRIMARY KEY,
            path TEXT UNIQUE NOT NULL,
            language TEXT NOT NULL CHECK(language IN ('c','cpp','csharp','typescript','javascript','python','rust','java','go','bash','sql','yaml')),
            size_bytes INTEGER NOT NULL,
            mtime_ns INTEGER NOT NULL,
            content_hash TEXT NOT NULL,
            parse_status TEXT NOT NULL CHECK(parse_status IN ('ok','partial','failed','skipped')),
            parse_error TEXT
        );
        CREATE INDEX IF NOT EXISTS idx_files_content_hash ON files(content_hash);
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
            stable_key TEXT NOT NULL UNIQUE
        );
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
            containing_node_id INTEGER REFERENCES nodes(id) ON DELETE SET NULL
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

// Drop secondary indexes for bulk loading. Leaves PRIMARY KEY and UNIQUE constraints.
inline void drop_bulk_indexes(Connection& conn) {
    conn.exec("DROP INDEX IF EXISTS idx_files_content_hash");
    conn.exec("DROP INDEX IF EXISTS idx_nodes_file_id");
    conn.exec("DROP INDEX IF EXISTS idx_nodes_type_kind_name");
    conn.exec("DROP INDEX IF EXISTS idx_nodes_qualname");
    conn.exec("DROP INDEX IF EXISTS idx_nodes_name_type");
    conn.exec("DROP INDEX IF EXISTS idx_refs_file_id");
    conn.exec("DROP INDEX IF EXISTS idx_refs_kind_name");
    conn.exec("DROP INDEX IF EXISTS idx_refs_resolved");
    conn.exec("DROP INDEX IF EXISTS idx_edges_src");
    conn.exec("DROP INDEX IF EXISTS idx_edges_dst");
}

// Rebuild secondary indexes after bulk loading.
inline void rebuild_indexes(Connection& conn) {
    conn.exec("CREATE INDEX IF NOT EXISTS idx_files_content_hash ON files(content_hash)");
    conn.exec("CREATE INDEX IF NOT EXISTS idx_nodes_file_id ON nodes(file_id)");
    conn.exec("CREATE INDEX IF NOT EXISTS idx_nodes_type_kind_name ON nodes(node_type, kind, name)");
    conn.exec("CREATE INDEX IF NOT EXISTS idx_nodes_qualname ON nodes(qualname)");
    conn.exec("CREATE INDEX IF NOT EXISTS idx_nodes_name_type ON nodes(name, node_type)");
    conn.exec("CREATE INDEX IF NOT EXISTS idx_refs_file_id ON refs(file_id)");
    conn.exec("CREATE INDEX IF NOT EXISTS idx_refs_kind_name ON refs(kind, name)");
    conn.exec("CREATE INDEX IF NOT EXISTS idx_refs_resolved ON refs(resolved_node_id)");
    conn.exec("CREATE INDEX IF NOT EXISTS idx_edges_src ON edges(src_id, kind)");
    conn.exec("CREATE INDEX IF NOT EXISTS idx_edges_dst ON edges(dst_id, kind)");
}

// Initialize or verify schema. Returns exit code (0=ok, 3=mismatch).
inline int ensure_schema(Connection& conn) {
    int version = get_schema_version(conn);

    if (version == 0) {
        // Fresh DB
        create_tables(conn);
        create_fts(conn);
        set_kv(conn, "schema_version", std::to_string(CURRENT_SCHEMA_VERSION));
        return 0;
    }

    if (version == CURRENT_SCHEMA_VERSION) {
        return 0;  // Compatible
    }

    if (version > CURRENT_SCHEMA_VERSION) {
        return 3;  // Newer version — abort
    }

    // Older version — recreate from scratch
    // Drop all tables and recreate
    conn.exec("DROP TABLE IF EXISTS nodes_fts");
    conn.exec("DROP TABLE IF EXISTS edges");
    conn.exec("DROP TABLE IF EXISTS refs");
    conn.exec("DROP TABLE IF EXISTS nodes");
    conn.exec("DROP TABLE IF EXISTS files");
    conn.exec("DROP TABLE IF EXISTS kv");
    conn.exec("DROP TABLE IF EXISTS quarantine");

    create_tables(conn);
    create_fts(conn);
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

} // namespace schema
} // namespace codetopo
