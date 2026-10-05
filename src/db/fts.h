#pragma once

#include "db/connection.h"
#include <sqlite3.h>
#include <string>
#include <array>
#include <algorithm>
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

using StatementPtr = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

inline StatementPtr prepare(sqlite3* db, const char* sql) {
    sqlite3_stmt* stmt = nullptr;
    const int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    StatementPtr result(stmt, sqlite3_finalize);
    if (rc != SQLITE_OK) throw SqliteError(rc, "Content FTS prepare failed: " + std::string(sqlite3_errmsg(db)));
    return result;
}

inline void step_write(sqlite3_stmt* stmt) {
    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        const std::string message = sqlite3_errmsg(sqlite3_db_handle(stmt));
        sqlite3_reset(stmt);
        throw SqliteError(rc, "Content FTS write failed: " + message);
    }
}

inline constexpr auto make_alnum_table() {
    std::array<bool, 256> table{};
    for (int i = 0; i < 256; ++i) {
        table[i] = (i >= 'A' && i <= 'Z') || (i >= 'a' && i <= 'z') ||
                   (i >= '0' && i <= '9') || i == '_';
    }
    return table;
}
inline constexpr auto kAlnumOrUnderscore = make_alnum_table();

// Insert all lines of a file's content into content_fts using pre-prepared statements.
// ins: INSERT INTO content_fts(content, file_id, line_no) VALUES(?, ?, ?)
// trk: INSERT OR REPLACE INTO content_fts_tracker(file_id, min_rowid, max_rowid) VALUES(?,?,?)  (may be nullptr)
// Skips lines with fewer than 3 alphanumeric characters (eliminates bracket-only,
// punctuation-only, and trivial lines that add FTS overhead with zero search value).
// Truncates lines longer than 200 chars (code_search reads full lines from disk).
inline void insert_lines(sqlite3_stmt* ins, sqlite3_stmt* trk,
                         int64_t file_id, const std::string& content) {
    if (content.empty()) return;

    // Capture the rowid range of the lines we insert so delete_file can later remove
    // them by rowid (the only index-driven delete FTS5 contentless tables allow).
    // Lines are inserted consecutively, so the file's rows form one contiguous block.
    sqlite3* db = sqlite3_db_handle(ins);
    int64_t min_rowid = 0, max_rowid = 0;
    bool any_inserted = false;
    int line_no = 1;
    size_t pos = 0;

    // Parameter 2 (file_id) is identical across all lines in this file.
    // sqlite3_reset() preserves bindings, so bind once outside the line loop.
    sqlite3_bind_int64(ins, 2, file_id);

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
            for (size_t i = 0; i < line_len; ++i) {
                if (kAlnumOrUnderscore[static_cast<unsigned char>(content[pos + i])]) {
                    if (++alnum_count >= 3) break;
                }
            }
            if (alnum_count >= 3) {
                int bind_len = static_cast<int>(std::min(line_len, size_t(200)));
                sqlite3_reset(ins);
                sqlite3_bind_text(ins, 1, content.data() + pos, bind_len, SQLITE_STATIC);
                sqlite3_bind_int(ins, 3, line_no);
                step_write(ins);
                int64_t rid = sqlite3_last_insert_rowid(db);
                if (!any_inserted) min_rowid = rid;
                max_rowid = rid;
                any_inserted = true;
            }
        }

        ++line_no;
        pos = (eol == std::string::npos) ? content.size() : eol + 1;
    }

    if (any_inserted && trk) {
        sqlite3_reset(trk);
        sqlite3_bind_int64(trk, 1, file_id);
        sqlite3_bind_int64(trk, 2, min_rowid);
        sqlite3_bind_int64(trk, 3, max_rowid);
        step_write(trk);
    }
}

// Delete all entries for a given file_id (used before re-inserting updated content).
// content_fts is a contentless FTS5 table with file_id UNINDEXED, so deleting by
// file_id would scan the ENTIRE trigram index (every line of every file). Instead we
// look up the file's stored rowid range (recorded by insert_lines) and delete by
// rowid — the only index-driven delete FTS5 contentless tables support. A file's
// lines are always inserted as one contiguous rowid block, so [min,max] is exact.
inline void delete_file(Connection& conn, int64_t file_id) {
    int64_t min_rowid = 0, max_rowid = 0;
    bool have_range = false;
    {
        auto q = prepare(conn.raw(),
            "SELECT min_rowid, max_rowid FROM content_fts_tracker WHERE file_id = ?");
        sqlite3_bind_int64(q.get(), 1, file_id);
        const int rc = sqlite3_step(q.get());
        if (rc != SQLITE_ROW && rc != SQLITE_DONE)
            throw SqliteError(rc, "Content FTS tracker read failed: " + std::string(sqlite3_errmsg(conn.raw())));
        if (rc == SQLITE_ROW && sqlite3_column_type(q.get(), 0) != SQLITE_NULL &&
            sqlite3_column_type(q.get(), 1) != SQLITE_NULL) {
            min_rowid = sqlite3_column_int64(q.get(), 0);
            max_rowid = sqlite3_column_int64(q.get(), 1);
            have_range = true;
        }
    }

    StatementPtr stmt(nullptr, sqlite3_finalize);
    if (have_range) {
        stmt = prepare(conn.raw(), "DELETE FROM content_fts WHERE rowid >= ? AND rowid <= ?");
        sqlite3_bind_int64(stmt.get(), 1, min_rowid);
        sqlite3_bind_int64(stmt.get(), 2, max_rowid);
    } else {
        // Legacy fallback: rows indexed before rowid ranges were tracked (NULL range).
        // This scans the FTS index once; the subsequent re-insert records a range so
        // future deletes of this file are fast. A --force reindex populates all ranges.
        stmt = prepare(conn.raw(), "DELETE FROM content_fts WHERE file_id = ?");
        sqlite3_bind_int64(stmt.get(), 1, file_id);
    }
    step_write(stmt.get());

    // Remove from tracker
    auto trk = prepare(conn.raw(), "DELETE FROM content_fts_tracker WHERE file_id = ?");
    sqlite3_bind_int64(trk.get(), 1, file_id);
    step_write(trk.get());
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
        "INSERT OR REPLACE INTO content_fts_tracker(file_id, min_rowid, max_rowid) VALUES(?, ?, ?)",
        -1, &trk, nullptr);
    insert_lines(ins, trk, file_id, content);
    sqlite3_finalize(ins);
    sqlite3_finalize(trk);
}

// Rebuild content FTS from scratch by reading all files from disk.
inline void rebuild_from_disk(Connection& conn, const std::string& repo_root) {
    namespace fs = std::filesystem;

    conn.exec("INSERT INTO content_fts(content_fts) VALUES('delete-all')");
    conn.exec("CREATE TABLE IF NOT EXISTS content_fts_tracker (file_id INTEGER PRIMARY KEY, min_rowid INTEGER, max_rowid INTEGER)");
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
        "INSERT OR REPLACE INTO content_fts_tracker(file_id, min_rowid, max_rowid) VALUES(?, ?, ?)",
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

    conn.exec("CREATE TABLE IF NOT EXISTS content_fts_tracker (file_id INTEGER PRIMARY KEY, min_rowid INTEGER, max_rowid INTEGER)");

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
        "INSERT OR REPLACE INTO content_fts_tracker(file_id, min_rowid, max_rowid) VALUES(?, ?, ?)",
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
