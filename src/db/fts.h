#pragma once

#include "db/connection.h"
#include <sqlite3.h>
#include <string>
#include <fstream>
#include <filesystem>
#include <iostream>

namespace codetopo {

// T019: FTS5 index management — sync triggers and rebuild.
namespace fts {

inline void create_sync_triggers(Connection& conn) {
    // Trigger to keep nodes_fts in sync on INSERT
    conn.exec(R"SQL(
        CREATE TRIGGER IF NOT EXISTS nodes_fts_insert AFTER INSERT ON nodes BEGIN
            INSERT INTO nodes_fts(rowid, name, qualname, signature, doc)
            SELECT new.id, codetopo_camel_split(new.name), new.qualname, new.signature, new.doc
            WHERE new.node_type = 'symbol';
        END;
    )SQL");

    // Trigger to keep nodes_fts in sync on DELETE
    conn.exec(R"SQL(
        CREATE TRIGGER IF NOT EXISTS nodes_fts_delete AFTER DELETE ON nodes BEGIN
            INSERT INTO nodes_fts(nodes_fts, rowid, name, qualname, signature, doc)
            SELECT 'delete', old.id, codetopo_camel_split(old.name), old.qualname, old.signature, old.doc
            WHERE old.node_type = 'symbol';
        END;
    )SQL");

    // Trigger for UPDATE
    conn.exec(R"SQL(
        CREATE TRIGGER IF NOT EXISTS nodes_fts_update AFTER UPDATE ON nodes BEGIN
            INSERT INTO nodes_fts(nodes_fts, rowid, name, qualname, signature, doc)
            SELECT 'delete', old.id, codetopo_camel_split(old.name), old.qualname, old.signature, old.doc
            WHERE old.node_type = 'symbol';
            INSERT INTO nodes_fts(rowid, name, qualname, signature, doc)
            SELECT new.id, codetopo_camel_split(new.name), new.qualname, new.signature, new.doc
            WHERE new.node_type = 'symbol';
        END;
    )SQL");
}

// Drop FTS sync triggers for bulk loading (avoids per-row trigger overhead)
inline void drop_sync_triggers(Connection& conn) {
    conn.exec("DROP TRIGGER IF EXISTS nodes_fts_insert");
    conn.exec("DROP TRIGGER IF EXISTS nodes_fts_delete");
    conn.exec("DROP TRIGGER IF EXISTS nodes_fts_update");
}

inline void rebuild(Connection& conn) {
    conn.exec("INSERT INTO nodes_fts(nodes_fts) VALUES('delete-all')");
    conn.exec(
        "INSERT INTO nodes_fts(rowid, name, qualname, signature, doc) "
        "SELECT id, codetopo_camel_split(name), qualname, signature, doc "
        "FROM nodes WHERE node_type = 'symbol'");
}

} // namespace fts

// Content FTS management — per-line trigram index for source code content search.
// Each non-blank source line is stored as its own FTS5 row with file_id and line_no,
// so MATCH returns exact line numbers without needing to re-read and scan files.
namespace content_fts {

// Insert all lines of a file's content into content_fts using pre-prepared statements.
// ins: INSERT INTO content_fts(content, file_id, line_no) VALUES(?, ?, ?)
// trk: INSERT OR IGNORE INTO content_fts_tracker(file_id) VALUES(?)  (may be nullptr)
// Skips lines with fewer than 3 alphanumeric characters (eliminates bracket-only,
// punctuation-only, and trivial lines that add FTS overhead with zero search value).
// Truncates lines longer than 200 chars (code_search reads full lines from disk).
inline void insert_lines(sqlite3_stmt* ins, sqlite3_stmt* trk,
                         int64_t file_id, const std::string& content) {
    if (content.empty()) return;

    bool any_inserted = false;
    int line_no = 1;
    size_t pos = 0;
    while (pos < content.size()) {
        size_t eol = content.find('\n', pos);
        size_t len = (eol == std::string::npos) ? content.size() - pos : eol - pos;

        // Strip trailing \r
        size_t line_len = len;
        if (line_len > 0 && content[pos + line_len - 1] == '\r') --line_len;

        if (line_len >= 3) {
            // Count alphanumeric chars — lines with <3 are pure punctuation/braces
            // (e.g. "{", "};", "*/", "))") and have zero search value.
            int alnum_count = 0;
            for (size_t i = 0; i < line_len && alnum_count < 3; ++i) {
                unsigned char c = static_cast<unsigned char>(content[pos + i]);
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '_') ++alnum_count;
            }
            if (alnum_count >= 3) {
                int bind_len = static_cast<int>(std::min(line_len, size_t(200)));
                sqlite3_reset(ins);
                sqlite3_bind_text(ins, 1, content.data() + pos, bind_len, SQLITE_STATIC);
                sqlite3_bind_int64(ins, 2, file_id);
                sqlite3_bind_int(ins, 3, line_no);
                sqlite3_step(ins);
                any_inserted = true;
            }
        }

        ++line_no;
        pos = (eol == std::string::npos) ? content.size() : eol + 1;
    }

    if (any_inserted && trk) {
        sqlite3_reset(trk);
        sqlite3_bind_int64(trk, 1, file_id);
        sqlite3_step(trk);
    }
}

// Delete all entries for a given file_id (used before re-inserting updated content)
inline void delete_file(Connection& conn, int64_t file_id) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(conn.raw(),
        "DELETE FROM content_fts WHERE file_id = ?", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, file_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    // Remove from tracker
    sqlite3_stmt* trk = nullptr;
    sqlite3_prepare_v2(conn.raw(),
        "DELETE FROM content_fts_tracker WHERE file_id = ?", -1, &trk, nullptr);
    sqlite3_bind_int64(trk, 1, file_id);
    sqlite3_step(trk);
    sqlite3_finalize(trk);
}

// Insert file content line-by-line into the trigram index.
inline void insert_file(Connection& conn, int64_t file_id, const std::string& content) {
    if (content.empty()) return;
    sqlite3_stmt* ins = nullptr;
    sqlite3_prepare_v2(conn.raw(),
        "INSERT INTO content_fts(content, file_id, line_no) VALUES(?, ?, ?)",
        -1, &ins, nullptr);
    sqlite3_stmt* trk = nullptr;
    sqlite3_prepare_v2(conn.raw(),
        "INSERT OR IGNORE INTO content_fts_tracker(file_id) VALUES(?)",
        -1, &trk, nullptr);
    insert_lines(ins, trk, file_id, content);
    sqlite3_finalize(ins);
    sqlite3_finalize(trk);
}

// Rebuild content FTS from scratch by reading all files from disk.
inline void rebuild_from_disk(Connection& conn, const std::string& repo_root) {
    namespace fs = std::filesystem;

    conn.exec("INSERT INTO content_fts(content_fts) VALUES('delete-all')");
    conn.exec("CREATE TABLE IF NOT EXISTS content_fts_tracker (file_id INTEGER PRIMARY KEY)");
    conn.exec("DELETE FROM content_fts_tracker");

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(conn.raw(),
        "SELECT id, path FROM files WHERE parse_status != 'skipped'",
        -1, &stmt, nullptr);

    sqlite3_stmt* ins = nullptr;
    sqlite3_prepare_v2(conn.raw(),
        "INSERT INTO content_fts(content, file_id, line_no) VALUES(?, ?, ?)",
        -1, &ins, nullptr);

    sqlite3_stmt* trk = nullptr;
    sqlite3_prepare_v2(conn.raw(),
        "INSERT OR IGNORE INTO content_fts_tracker(file_id) VALUES(?)",
        -1, &trk, nullptr);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t file_id = sqlite3_column_int64(stmt, 0);
        auto* path_text = sqlite3_column_text(stmt, 1);
        if (!path_text) continue;

        auto full_path = fs::path(repo_root) / reinterpret_cast<const char*>(path_text);
        std::ifstream f(full_path, std::ios::binary);
        if (!f) continue;

        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
        if (content.empty()) continue;
        if (content.size() > 1024 * 1024) content.resize(1024 * 1024);

        insert_lines(ins, trk, file_id, content);
    }

    sqlite3_finalize(stmt);
    sqlite3_finalize(ins);
    sqlite3_finalize(trk);
}

// Backfill content FTS for files in DB but missing from content_fts_tracker.
// Returns the number of files backfilled. Uses the tracker table because contentless
// FTS5 tables cannot be scanned without a MATCH clause.
inline int backfill_missing(Connection& conn, const std::string& repo_root) {
    namespace fs = std::filesystem;

    conn.exec("CREATE TABLE IF NOT EXISTS content_fts_tracker (file_id INTEGER PRIMARY KEY)");

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(conn.raw(),
        "SELECT f.id, f.path FROM files f "
        "WHERE f.parse_status != 'skipped' "
        "AND f.id NOT IN (SELECT file_id FROM content_fts_tracker)",
        -1, &stmt, nullptr);

    sqlite3_stmt* ins = nullptr;
    sqlite3_prepare_v2(conn.raw(),
        "INSERT INTO content_fts(content, file_id, line_no) VALUES(?, ?, ?)",
        -1, &ins, nullptr);

    sqlite3_stmt* trk = nullptr;
    sqlite3_prepare_v2(conn.raw(),
        "INSERT OR IGNORE INTO content_fts_tracker(file_id) VALUES(?)",
        -1, &trk, nullptr);

    int count = 0;
    conn.exec("BEGIN TRANSACTION");
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t file_id = sqlite3_column_int64(stmt, 0);
        auto* path_text = sqlite3_column_text(stmt, 1);
        if (!path_text) continue;

        auto full_path = fs::path(repo_root) / reinterpret_cast<const char*>(path_text);
        std::ifstream f(full_path, std::ios::binary);
        if (!f) continue;

        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
        if (content.empty()) continue;
        if (content.size() > 1024 * 1024) content.resize(1024 * 1024);

        insert_lines(ins, trk, file_id, content);
        ++count;

        if (count % 1000 == 0) {
            std::cerr << "\r\033[K  " << count << " files..." << std::flush;
        }
    }
    conn.exec("COMMIT");

    sqlite3_finalize(stmt);
    sqlite3_finalize(ins);
    sqlite3_finalize(trk);
    return count;
}

} // namespace content_fts
} // namespace codetopo
