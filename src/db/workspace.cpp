// Multi-root workspace database implementation.
// Merges per-root index.sqlite databases directly into the main index.sqlite
// with globally unique node IDs (root_id * 10^9 + local_id) and absolute file paths.
// Uses identical table names so the MCP server works unchanged.

#include "db/workspace.h"
#include "db/schema.h"
#include "db/fts.h"
#include "index/supervisor.h"
#include "util/repo.h"
#include <sqlite3.h>
#include <filesystem>
#include <stdexcept>
#include <iostream>
#include <fstream>

namespace codetopo {

// ID re-keying constant: each root gets a 10^9 ID space.
static constexpr int64_t ID_SPACE = 1000000000LL;

WorkspaceDB::WorkspaceDB(const std::string& main_db_path)
    : conn_(main_db_path) {
    ensure_schema();
}

void WorkspaceDB::ensure_schema() {
    conn_.exec("PRAGMA foreign_keys = ON");
    // Ensure roots table and root_id column exist (additive, idempotent).
    // On a fresh v5 DB the roots table already exists from create_tables();
    // on an old DB this is the migration path.
    schema::ensure_roots_schema(conn_);
}

WorkspaceDB::AddResult WorkspaceDB::add_root(const std::string& root_path, const Config& cfg) {
    namespace fs = std::filesystem;

    auto abs_root = fs::canonical(root_path).string();

    // Check for overlap with existing roots (warn only)
    check_overlap(abs_root);

    // Ensure the root is indexed (run supervisor if index.sqlite missing)
    std::string root_index = default_db(abs_root);
    if (!fs::exists(root_index)) {
        std::cerr << "Indexing " << abs_root << "...\n";
        Config index_cfg = cfg;
        index_cfg.repo_root = abs_root;
        index_cfg.db_path = root_index;
        ensure_codetopo_dir(abs_root);
        int rc = run_index_supervisor(index_cfg);
        if (rc != 0 && rc != 1) {
            throw std::runtime_error("Failed to index root: " + abs_root + " (exit " + std::to_string(rc) + ")");
        }
    }

    // Checkpoint WAL on source DB so ATTACH gets a clean read
    {
        sqlite3* src_db = nullptr;
        if (sqlite3_open_v2(root_index.c_str(), &src_db,
                            SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK) {
            sqlite3_exec(src_db, "PRAGMA wal_checkpoint(TRUNCATE)", nullptr, nullptr, nullptr);
            sqlite3_close(src_db);
        }
    }

    // ATTACH source DB BEFORE the transaction (SQLite disallows ATTACH inside exclusive tx)
    std::string attach_sql = "ATTACH DATABASE '" + root_index + "' AS src";
    conn_.exec(attach_sql);

    // Insert root (or get existing)
    conn_.exec("BEGIN EXCLUSIVE");
    try {
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(conn_.raw(),
            "INSERT OR IGNORE INTO roots(path, added_at) VALUES(?, datetime('now'))", -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, abs_root.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        // Get root_id
        stmt = nullptr;
        sqlite3_prepare_v2(conn_.raw(),
            "SELECT id FROM roots WHERE path = ?", -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, abs_root.c_str(), -1, SQLITE_TRANSIENT);
        int64_t root_id = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            root_id = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);

        if (root_id == 0) {
            conn_.exec("ROLLBACK");
            conn_.exec("DETACH DATABASE src");
            throw std::runtime_error("Failed to insert workspace root: " + abs_root);
        }

        // Clear existing data for this root (idempotent re-add)
        // Delete FTS entries first (before cascade removes nodes)
        int64_t offset = root_id * ID_SPACE;
        std::string fts_del =
            "INSERT INTO nodes_fts(nodes_fts, rowid, name, qualname, signature, doc) "
            "SELECT 'delete', id, name, qualname, signature, doc FROM nodes "
            "WHERE id >= " + std::to_string(offset) + " AND id < " + std::to_string(offset + ID_SPACE);
        conn_.exec(fts_del);

        // Clear content_fts for this root's file ID range (contentless_delete=1 allows this)
        conn_.exec("DELETE FROM content_fts WHERE file_id >= " + std::to_string(offset) +
            " AND file_id < " + std::to_string(offset + ID_SPACE));
        conn_.exec("DELETE FROM content_fts_tracker WHERE file_id >= " + std::to_string(offset) +
            " AND file_id < " + std::to_string(offset + ID_SPACE));

        // Delete all nodes in this root's ID range (covers file-type nodes which
        // have file_id=NULL and are NOT cascade-deleted when files are removed).
        std::string del_nodes =
            "DELETE FROM nodes WHERE id >= " + std::to_string(offset) +
            " AND id < " + std::to_string(offset + ID_SPACE);
        conn_.exec(del_nodes);

        // Delete files for this root (cascades symbol nodes, edges, refs via FK).
        stmt = nullptr;
        sqlite3_prepare_v2(conn_.raw(),
            "DELETE FROM files WHERE root_id = ?", -1, &stmt, nullptr);
        sqlite3_bind_int64(stmt, 1, root_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        // Merge from the already-attached src DB
        merge_root_attached(root_id, abs_root);

        conn_.exec("COMMIT");
    } catch (...) {
        conn_.exec("ROLLBACK");
        conn_.exec("DETACH DATABASE src");
        throw;
    }

    // DETACH AFTER commit (outside transaction)
    conn_.exec("DETACH DATABASE src");

    // Query stats
    AddResult result;
    result.root_id = 0;

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(conn_.raw(),
        "SELECT id FROM roots WHERE path = ?", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, abs_root.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) result.root_id = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);

    int64_t root_id = result.root_id;

    stmt = nullptr;
    sqlite3_prepare_v2(conn_.raw(),
        "SELECT COUNT(*) FROM files WHERE root_id = ?", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, root_id);
    if (sqlite3_step(stmt) == SQLITE_ROW) result.files = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);

    stmt = nullptr;
    sqlite3_prepare_v2(conn_.raw(),
        "SELECT COUNT(*) FROM nodes n JOIN files f ON n.file_id = f.id WHERE f.root_id = ?", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, root_id);
    if (sqlite3_step(stmt) == SQLITE_ROW) result.symbols = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);

    stmt = nullptr;
    sqlite3_prepare_v2(conn_.raw(),
        "SELECT COUNT(*) FROM edges e JOIN nodes n ON e.src_id = n.id "
        "JOIN files f ON n.file_id = f.id WHERE f.root_id = ?", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, root_id);
    if (sqlite3_step(stmt) == SQLITE_ROW) result.edges = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);

    return result;
}

WorkspaceDB::RemoveResult WorkspaceDB::remove_root(const std::string& root_path) {
    namespace fs = std::filesystem;
    auto abs_root = fs::canonical(root_path).string();

    // Get root_id
    RemoveResult result;
    sqlite3_stmt* stmt = nullptr;

    sqlite3_prepare_v2(conn_.raw(),
        "SELECT id FROM roots WHERE path = ?", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, abs_root.c_str(), -1, SQLITE_TRANSIENT);
    int64_t root_id = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        root_id = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);

    if (root_id == 0) {
        std::cerr << "WARNING: Root not found in workspace: " << abs_root << "\n";
        return result;
    }

    // Count before delete
    stmt = nullptr;
    sqlite3_prepare_v2(conn_.raw(),
        "SELECT COUNT(*) FROM files WHERE root_id = ?", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, root_id);
    if (sqlite3_step(stmt) == SQLITE_ROW) result.files = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);

    stmt = nullptr;
    sqlite3_prepare_v2(conn_.raw(),
        "SELECT COUNT(*) FROM nodes n JOIN files f ON n.file_id = f.id WHERE f.root_id = ?", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, root_id);
    if (sqlite3_step(stmt) == SQLITE_ROW) result.symbols = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);

    stmt = nullptr;
    sqlite3_prepare_v2(conn_.raw(),
        "SELECT COUNT(*) FROM edges e JOIN nodes n ON e.src_id = n.id "
        "JOIN files f ON n.file_id = f.id WHERE f.root_id = ?", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, root_id);
    if (sqlite3_step(stmt) == SQLITE_ROW) result.edges = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);

    // Remove FTS entries before cascade deletes nodes
    int64_t offset = root_id * ID_SPACE;
    std::string fts_del =
        "INSERT INTO nodes_fts(nodes_fts, rowid, name, qualname, signature, doc) "
        "SELECT 'delete', id, name, qualname, signature, doc FROM nodes "
        "WHERE id >= " + std::to_string(offset) + " AND id < " + std::to_string(offset + ID_SPACE);
    conn_.exec(fts_del);

    // Remove content_fts entries for this root's file ID range
    conn_.exec("DELETE FROM content_fts WHERE file_id >= " + std::to_string(offset) +
        " AND file_id < " + std::to_string(offset + ID_SPACE));
    conn_.exec("DELETE FROM content_fts_tracker WHERE file_id >= " + std::to_string(offset) +
        " AND file_id < " + std::to_string(offset + ID_SPACE));

    // CASCADE delete: DELETE FROM roots → files → nodes → edges/refs
    stmt = nullptr;
    sqlite3_prepare_v2(conn_.raw(),
        "DELETE FROM roots WHERE id = ?", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, root_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return result;
}

std::vector<WorkspaceDB::RootInfo> WorkspaceDB::list_roots() {
    std::vector<RootInfo> roots;

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(conn_.raw(),
        "SELECT r.id, r.path, "
        "(SELECT COUNT(*) FROM files WHERE root_id = r.id), "
        "(SELECT COUNT(*) FROM nodes n JOIN files f ON n.file_id = f.id WHERE f.root_id = r.id), "
        "(SELECT COUNT(*) FROM edges e JOIN nodes n ON e.src_id = n.id JOIN files f ON n.file_id = f.id WHERE f.root_id = r.id) "
        "FROM roots r ORDER BY r.id", -1, &stmt, nullptr);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        RootInfo info;
        info.id = sqlite3_column_int64(stmt, 0);
        info.path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        info.files = sqlite3_column_int64(stmt, 2);
        info.symbols = sqlite3_column_int64(stmt, 3);
        info.edges = sqlite3_column_int64(stmt, 4);
        roots.push_back(std::move(info));
    }
    sqlite3_finalize(stmt);
    return roots;
}

void WorkspaceDB::check_overlap(const std::string& new_root_path) {
    namespace fs = std::filesystem;
    auto new_path = fs::path(new_root_path);

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(conn_.raw(),
        "SELECT path FROM roots", -1, &stmt, nullptr);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        auto existing = fs::path(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
        auto new_str = new_path.string();
        auto exist_str = existing.string();
        if (new_str.find(exist_str) == 0 && new_str.size() > exist_str.size()) {
            std::cerr << "WARNING: " << new_root_path << " is inside " << exist_str
                      << " — files may be double-indexed.\n";
        } else if (exist_str.find(new_str) == 0 && exist_str.size() > new_str.size()) {
            std::cerr << "WARNING: " << exist_str << " is inside " << new_root_path
                      << " — files may be double-indexed.\n";
        }
    }
    sqlite3_finalize(stmt);
}

void WorkspaceDB::merge_root_attached(int64_t root_id, const std::string& root_path) {
    // Assumes 'src' is already ATTACHed by the caller before the transaction.
    std::string root_prefix = root_path;
    if (!root_prefix.empty() && root_prefix.back() != '/') root_prefix += '/';

    int64_t offset = root_id * ID_SPACE;

    // 1. Files — map id → offset+id, path → absolute, with root_id
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(conn_.raw(),
        "INSERT INTO files (id, root_id, path, language, size_bytes, mtime_ns, content_hash, parse_status, parse_error) "
        "SELECT (? + id), ?, (? || path), language, size_bytes, mtime_ns, content_hash, parse_status, parse_error "
        "FROM src.files", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, offset);
    sqlite3_bind_int64(stmt, 2, root_id);
    sqlite3_bind_text(stmt, 3, root_prefix.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // 2. Nodes — map id → offset+id, file_id → offset+file_id
    stmt = nullptr;
    sqlite3_prepare_v2(conn_.raw(),
        "INSERT INTO nodes (id, node_type, file_id, kind, name, qualname, signature, "
        "start_line, start_col, end_line, end_col, is_definition, visibility, doc, stable_key) "
        "SELECT (? + id), node_type, "
        "CASE WHEN file_id IS NOT NULL THEN (? + file_id) ELSE NULL END, "
        "kind, name, qualname, signature, start_line, start_col, end_line, end_col, "
        "is_definition, visibility, doc, (? || ':' || stable_key) "
        "FROM src.nodes", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, offset);
    sqlite3_bind_int64(stmt, 2, offset);
    sqlite3_bind_text(stmt, 3, std::to_string(root_id).c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // 3. Edges — re-key src_id and dst_id
    stmt = nullptr;
    sqlite3_prepare_v2(conn_.raw(),
        "INSERT OR IGNORE INTO edges (src_id, dst_id, kind, confidence, evidence) "
        "SELECT (? + src_id), (? + dst_id), kind, confidence, evidence "
        "FROM src.edges", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, offset);
    sqlite3_bind_int64(stmt, 2, offset);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // 4. Refs — re-key file_id, resolved_node_id, containing_node_id
    stmt = nullptr;
    sqlite3_prepare_v2(conn_.raw(),
        "INSERT INTO refs (id, file_id, kind, name, start_line, start_col, end_line, end_col, "
        "resolved_node_id, evidence, containing_node_id) "
        "SELECT (? + id), (? + file_id), kind, name, start_line, start_col, end_line, end_col, "
        "CASE WHEN resolved_node_id IS NOT NULL THEN (? + resolved_node_id) ELSE NULL END, "
        "evidence, "
        "CASE WHEN containing_node_id IS NOT NULL THEN (? + containing_node_id) ELSE NULL END "
        "FROM src.refs", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, offset);
    sqlite3_bind_int64(stmt, 2, offset);
    sqlite3_bind_int64(stmt, 3, offset);
    sqlite3_bind_int64(stmt, 4, offset);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // 5. Populate FTS (nodes_fts)
    std::string fts_sql =
        "INSERT INTO nodes_fts(rowid, name, qualname, signature, doc) "
        "SELECT id, name, qualname, signature, doc FROM nodes WHERE id >= " +
        std::to_string(offset) + " AND id < " + std::to_string(offset + ID_SPACE);
    conn_.exec(fts_sql);

    // 6. Populate content_fts — read source files from disk.
    // content_fts is a contentless FTS5 table so we can't copy from src.content_fts;
    // instead iterate over the just-merged files (absolute paths) and index them directly.
    {
        sqlite3_stmt* sel = nullptr;
        sqlite3_prepare_v2(conn_.raw(),
            "SELECT id, path FROM files "
            "WHERE id >= ? AND id < ? AND parse_status != 'skipped'",
            -1, &sel, nullptr);
        sqlite3_bind_int64(sel, 1, offset);
        sqlite3_bind_int64(sel, 2, offset + ID_SPACE);

        sqlite3_stmt* ins = nullptr;
        sqlite3_prepare_v2(conn_.raw(),
            "INSERT INTO content_fts(content, file_id, line_no) VALUES(?, ?, ?)",
            -1, &ins, nullptr);
        sqlite3_stmt* trk = nullptr;
        sqlite3_prepare_v2(conn_.raw(),
            "INSERT OR IGNORE INTO content_fts_tracker(file_id) VALUES(?)",
            -1, &trk, nullptr);

        while (sqlite3_step(sel) == SQLITE_ROW) {
            int64_t file_id = sqlite3_column_int64(sel, 0);
            const auto* path_text = sqlite3_column_text(sel, 1);
            if (!path_text) continue;

            std::ifstream f(reinterpret_cast<const char*>(path_text), std::ios::binary);
            if (!f) continue;

            std::string fc((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
            if (fc.empty()) continue;
            if (fc.size() > 1024 * 1024) fc.resize(1024 * 1024);
            content_fts::insert_lines(ins, trk, file_id, fc);
        }

        sqlite3_finalize(sel);
        sqlite3_finalize(ins);
        sqlite3_finalize(trk);
    }
}

} // namespace codetopo
