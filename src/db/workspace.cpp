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
#include <vector>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace codetopo {

// ID re-keying constant: each root gets a 10^9 ID space.
static constexpr int64_t ID_SPACE = 1000000000LL;

namespace {

static constexpr int kContentFtsBatchFiles = 1000;

using WorkspaceClock = std::chrono::steady_clock;

std::string format_seconds(WorkspaceClock::time_point start) {
    double seconds = std::chrono::duration<double>(WorkspaceClock::now() - start).count();
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3) << seconds;
    return oss.str();
}

void log_workspace_phase(const std::string& phase, WorkspaceClock::time_point start,
                         const std::string& suffix = {}) {
    std::cerr << "[workspace] " << phase << " in " << format_seconds(start) << "s";
    if (!suffix.empty()) std::cerr << " " << suffix;
    std::cerr << "\n";
}

std::string sqlite_error(sqlite3* db, const std::string& context) {
    return context + ": " + sqlite3_errmsg(db);
}

void prepare_or_throw(sqlite3* db, sqlite3_stmt** stmt, const std::string& sql) {
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error(sqlite_error(db, "SQLite prepare failed"));
    }
}

void step_done_or_throw(sqlite3* db, sqlite3_stmt* stmt, const std::string& context) {
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        throw std::runtime_error(sqlite_error(db, context));
    }
}

std::string pragma_text(Connection& conn, const char* pragma) {
    sqlite3_stmt* stmt = nullptr;
    std::string sql = std::string("PRAGMA ") + pragma;
    prepare_or_throw(conn.raw(), &stmt, sql);
    std::string value;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* txt = sqlite3_column_text(stmt, 0);
        if (txt) value = reinterpret_cast<const char*>(txt);
    }
    sqlite3_finalize(stmt);
    return value;
}

void exec_noexcept(sqlite3* db, const std::string& sql) noexcept {
    char* err = nullptr;
    sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
}

class ScopedWorkspacePragmas {
public:
    explicit ScopedWorkspacePragmas(Connection& conn, bool unsafe_sync_off)
        : conn_(conn),
          synchronous_(pragma_text(conn, "synchronous")),
          wal_autocheckpoint_(pragma_text(conn, "wal_autocheckpoint")),
          temp_store_(pragma_text(conn, "temp_store")),
          cache_size_(pragma_text(conn, "cache_size")),
          mmap_size_(pragma_text(conn, "mmap_size")) {
        conn_.exec(unsafe_sync_off ? "PRAGMA synchronous=OFF" : "PRAGMA synchronous=NORMAL");
        conn_.exec("PRAGMA wal_autocheckpoint=0");
        conn_.exec("PRAGMA temp_store=MEMORY");
        conn_.exec("PRAGMA cache_size=-524288");
        conn_.exec("PRAGMA mmap_size=8589934592");
    }

    ~ScopedWorkspacePragmas() {
        restore();
    }

    ScopedWorkspacePragmas(const ScopedWorkspacePragmas&) = delete;
    ScopedWorkspacePragmas& operator=(const ScopedWorkspacePragmas&) = delete;

private:
    void restore() noexcept {
        if (restored_) return;
        restored_ = true;
        auto* db = conn_.raw();
        if (!synchronous_.empty()) exec_noexcept(db, "PRAGMA synchronous=" + synchronous_);
        if (!wal_autocheckpoint_.empty()) exec_noexcept(db, "PRAGMA wal_autocheckpoint=" + wal_autocheckpoint_);
        if (!temp_store_.empty()) exec_noexcept(db, "PRAGMA temp_store=" + temp_store_);
        if (!cache_size_.empty()) exec_noexcept(db, "PRAGMA cache_size=" + cache_size_);
        if (!mmap_size_.empty()) exec_noexcept(db, "PRAGMA mmap_size=" + mmap_size_);
    }

    Connection& conn_;
    std::string synchronous_;
    std::string wal_autocheckpoint_;
    std::string temp_store_;
    std::string cache_size_;
    std::string mmap_size_;
    bool restored_ = false;
};

std::string sql_quote(const std::string& value) {
    char* quoted = sqlite3_mprintf("%Q", value.c_str());
    if (!quoted) throw std::runtime_error("SQLite quote allocation failed");
    std::string result(quoted);
    sqlite3_free(quoted);
    return result;
}

bool exists_int64_range(Connection& conn, const std::string& sql,
                        int64_t first, int64_t second = 0, bool bind_second = false) {
    sqlite3_stmt* stmt = nullptr;
    prepare_or_throw(conn.raw(), &stmt, sql);
    sqlite3_bind_int64(stmt, 1, first);
    if (bind_second) sqlite3_bind_int64(stmt, 2, second);
    bool exists = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return exists;
}

int64_t count_edges_for_root(Connection& conn, int64_t root_id) {
    int64_t offset = root_id * ID_SPACE;
    sqlite3_stmt* stmt = nullptr;
    prepare_or_throw(conn.raw(), &stmt,
        "SELECT COUNT(*) FROM edges WHERE src_id >= ? AND src_id < ?");
    sqlite3_bind_int64(stmt, 1, offset);
    sqlite3_bind_int64(stmt, 2, offset + ID_SPACE);
    int64_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

} // namespace

static bool attached_table_has_column(sqlite3* db, const char* schema,
                                      const char* table, const char* column) {
    sqlite3_stmt* stmt = nullptr;
    std::string sql = std::string("PRAGMA ") + schema + ".table_info(" + table + ")";
    bool found = false;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* col = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (col && std::string(col) == column) { found = true; break; }
    }
    sqlite3_finalize(stmt);
    return found;
}

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
    resume_pending_content_fts();
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

    ScopedWorkspacePragmas bulk_pragmas(conn_, cfg.turbo);

    // ATTACH source DB BEFORE the transaction (SQLite disallows ATTACH inside exclusive tx)
    std::string attach_sql = "ATTACH DATABASE " + sql_quote(root_index) + " AS src";
    conn_.exec(attach_sql);

    // Insert root (or get existing)
    int64_t root_id = 0;
    conn_.exec("BEGIN EXCLUSIVE");
    try {
        conn_.exec("PRAGMA defer_foreign_keys=ON");

        sqlite3_stmt* stmt = nullptr;
        prepare_or_throw(conn_.raw(), &stmt,
            "INSERT OR IGNORE INTO roots(path, added_at) VALUES(?, datetime('now'))");
        sqlite3_bind_text(stmt, 1, abs_root.c_str(), -1, SQLITE_TRANSIENT);
        step_done_or_throw(conn_.raw(), stmt, "Insert workspace root failed");
        sqlite3_finalize(stmt);

        // Get root_id
        stmt = nullptr;
        prepare_or_throw(conn_.raw(), &stmt,
            "SELECT id FROM roots WHERE path = ?");
        sqlite3_bind_text(stmt, 1, abs_root.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            root_id = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);

        if (root_id == 0) {
            throw std::runtime_error("Failed to insert workspace root: " + abs_root);
        }

        // Clear existing data for this root (idempotent re-add)
        // Drop nodes_fts triggers transactionally so range deletes/inserts do not
        // pay per-row FTS trigger cost. Rollback restores the pre-merge trigger state.
        fts::drop_sync_triggers(conn_);

        int64_t offset = root_id * ID_SPACE;
        bool had_nodes = exists_int64_range(conn_,
            "SELECT 1 FROM nodes WHERE id >= ? AND id < ? LIMIT 1",
            offset, offset + ID_SPACE, true);
        bool had_files = exists_int64_range(conn_,
            "SELECT 1 FROM files WHERE root_id = ? LIMIT 1",
            root_id);

        if (had_nodes) {
            std::string fts_del =
                "INSERT INTO nodes_fts(nodes_fts, rowid, name, qualname, signature, doc) "
                "SELECT 'delete', id, name, qualname, signature, doc FROM nodes "
                "WHERE id >= " + std::to_string(offset) + " AND id < " + std::to_string(offset + ID_SPACE);
            conn_.exec(fts_del);

            // Delete all nodes in this root's ID range (covers file-type nodes which
            // have file_id=NULL and are NOT cascade-deleted when files are removed).
            std::string del_nodes =
                "DELETE FROM nodes WHERE id >= " + std::to_string(offset) +
                " AND id < " + std::to_string(offset + ID_SPACE);
            conn_.exec(del_nodes);
        }

        if (had_files) {
            // Clear content_fts only when this root actually had indexed content.
            // file_id is UNINDEXED in FTS5; skipping this scan on first-add avoids
            // walking the existing workspace content index just to delete zero rows.
            bool had_content = exists_int64_range(conn_,
                "SELECT 1 FROM content_fts_tracker WHERE file_id >= ? AND file_id < ? LIMIT 1",
                offset, offset + ID_SPACE, true);
            if (had_content) {
                conn_.exec("DELETE FROM content_fts WHERE file_id >= " + std::to_string(offset) +
                    " AND file_id < " + std::to_string(offset + ID_SPACE));
                conn_.exec("DELETE FROM content_fts_tracker WHERE file_id >= " + std::to_string(offset) +
                    " AND file_id < " + std::to_string(offset + ID_SPACE));
            }

            // Delete files for this root (cascades symbol nodes, edges, refs via FK).
            stmt = nullptr;
            prepare_or_throw(conn_.raw(), &stmt,
                "DELETE FROM files WHERE root_id = ?");
            sqlite3_bind_int64(stmt, 1, root_id);
            step_done_or_throw(conn_.raw(), stmt, "Delete workspace files failed");
            sqlite3_finalize(stmt);
        }

        // Maintaining secondary indexes per copied row is expensive for large roots.
        // Drop/rebuild them inside this transaction so crash rollback restores them.
        auto index_phase = WorkspaceClock::now();
        schema::drop_bulk_indexes(conn_);
        log_workspace_phase("indexes dropped", index_phase);

        // Merge from the already-attached src DB
        merge_root_attached(root_id, abs_root);

        index_phase = WorkspaceClock::now();
        schema::rebuild_indexes(conn_);
        log_workspace_phase("indexes rebuilt", index_phase);
        fts::create_sync_triggers(conn_);
        auto content_pending_key = "workspace_content_fts_pending:" + std::to_string(root_id);
        if (cfg.workspace_content_fts) {
            schema::set_kv(conn_, content_pending_key, std::to_string(root_id));
        } else {
            conn_.exec("DELETE FROM kv WHERE key = " + sql_quote(content_pending_key));
        }

        int fk_violations = conn_.foreign_key_check();
        if (fk_violations != 0) {
            throw std::runtime_error("Workspace merge failed foreign_key_check: " +
                                     std::to_string(fk_violations) + " violations");
        }

        conn_.exec("COMMIT");
    } catch (...) {
        conn_.exec("ROLLBACK");
        conn_.exec("DETACH DATABASE src");
        throw;
    }

    // DETACH AFTER commit (outside transaction)
    conn_.exec("DETACH DATABASE src");
    conn_.exec("PRAGMA wal_checkpoint(TRUNCATE)");

    // Content FTS is intentionally populated after the structural merge in
    // bounded transactions. This keeps node/file/ref/edge merge atomic while
    // preventing line-level trigram indexing from creating a multi-GB WAL.
    if (cfg.workspace_content_fts) {
        populate_content_fts_for_root(root_id);
        conn_.exec("PRAGMA wal_checkpoint(TRUNCATE)");
    } else {
        auto content_phase = WorkspaceClock::now();
        log_workspace_phase("content_fts", content_phase, "(disabled; 0 files)");
    }

    // Query stats
    AddResult result;
    result.root_id = 0;

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(conn_.raw(),
        "SELECT id FROM roots WHERE path = ?", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, abs_root.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) result.root_id = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);

    root_id = result.root_id;

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

    result.edges = count_edges_for_root(conn_, root_id);

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

    int64_t offset = root_id * ID_SPACE;

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

    result.edges = count_edges_for_root(conn_, root_id);

    conn_.exec("BEGIN IMMEDIATE");
    try {
        // Drop nodes_fts triggers transactionally; otherwise root cascades would
        // perform one FTS delete per node after the bulk delete below.
        fts::drop_sync_triggers(conn_);

        bool had_nodes = exists_int64_range(conn_,
            "SELECT 1 FROM nodes WHERE id >= ? AND id < ? LIMIT 1",
            offset, offset + ID_SPACE, true);
        if (had_nodes) {
            std::string fts_del =
                "INSERT INTO nodes_fts(nodes_fts, rowid, name, qualname, signature, doc) "
                "SELECT 'delete', id, name, qualname, signature, doc FROM nodes "
                "WHERE id >= " + std::to_string(offset) + " AND id < " + std::to_string(offset + ID_SPACE);
            conn_.exec(fts_del);

            conn_.exec("DELETE FROM nodes WHERE id >= " + std::to_string(offset) +
                " AND id < " + std::to_string(offset + ID_SPACE));
        }

        bool had_content = exists_int64_range(conn_,
            "SELECT 1 FROM content_fts_tracker WHERE file_id >= ? AND file_id < ? LIMIT 1",
            offset, offset + ID_SPACE, true);
        if (had_content) {
            conn_.exec("DELETE FROM content_fts WHERE file_id >= " + std::to_string(offset) +
                " AND file_id < " + std::to_string(offset + ID_SPACE));
            conn_.exec("DELETE FROM content_fts_tracker WHERE file_id >= " + std::to_string(offset) +
                " AND file_id < " + std::to_string(offset + ID_SPACE));
        }

        // CASCADE delete: DELETE FROM roots → files → nodes → edges/refs
        stmt = nullptr;
        prepare_or_throw(conn_.raw(), &stmt,
            "DELETE FROM roots WHERE id = ?");
        sqlite3_bind_int64(stmt, 1, root_id);
        step_done_or_throw(conn_.raw(), stmt, "Delete workspace root failed");
        sqlite3_finalize(stmt);

        conn_.exec("DELETE FROM kv WHERE key = " +
            sql_quote("workspace_content_fts_pending:" + std::to_string(root_id)));
        fts::create_sync_triggers(conn_);
        conn_.exec("COMMIT");
    } catch (...) {
        conn_.exec("ROLLBACK");
        throw;
    }
    conn_.exec("PRAGMA wal_checkpoint(TRUNCATE)");

    return result;
}

std::vector<WorkspaceDB::RootInfo> WorkspaceDB::list_roots() {
    std::vector<RootInfo> roots;

    sqlite3_stmt* stmt = nullptr;
    std::string sql =
        "SELECT r.id, r.path, "
        "(SELECT COUNT(*) FROM files WHERE root_id = r.id), "
        "(SELECT COUNT(*) FROM nodes n JOIN files f ON n.file_id = f.id WHERE f.root_id = r.id), "
        "(SELECT COUNT(*) FROM edges e WHERE e.src_id >= r.id * " + std::to_string(ID_SPACE) +
        " AND e.src_id < (r.id + 1) * " + std::to_string(ID_SPACE) + ") "
        "FROM roots r ORDER BY r.id";
    sqlite3_prepare_v2(conn_.raw(), sql.c_str(), -1, &stmt, nullptr);

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
    auto phase = WorkspaceClock::now();
    sqlite3_stmt* stmt = nullptr;
    prepare_or_throw(conn_.raw(), &stmt,
        "INSERT INTO files (id, root_id, path, language, size_bytes, mtime_ns, content_hash, parse_status, parse_error) "
        "SELECT (? + id), ?, (? || path), language, size_bytes, mtime_ns, content_hash, parse_status, parse_error "
        "FROM src.files");
    sqlite3_bind_int64(stmt, 1, offset);
    sqlite3_bind_int64(stmt, 2, root_id);
    sqlite3_bind_text(stmt, 3, root_prefix.c_str(), -1, SQLITE_TRANSIENT);
    step_done_or_throw(conn_.raw(), stmt, "Copy workspace files failed");
    sqlite3_int64 copied = sqlite3_changes64(conn_.raw());
    sqlite3_finalize(stmt);
    log_workspace_phase("files copied", phase, "(" + std::to_string(copied) + " rows)");

    // 2. Nodes — map id → offset+id, file_id → offset+file_id
    phase = WorkspaceClock::now();
    stmt = nullptr;
    prepare_or_throw(conn_.raw(), &stmt,
        "INSERT INTO nodes (id, node_type, file_id, kind, name, qualname, signature, "
        "start_line, start_col, end_line, end_col, is_definition, visibility, doc, stable_key) "
        "SELECT (? + id), node_type, "
        "CASE WHEN file_id IS NOT NULL THEN (? + file_id) ELSE NULL END, "
        "kind, name, qualname, signature, start_line, start_col, end_line, end_col, "
        "is_definition, visibility, doc, (? || ':' || stable_key) "
        "FROM src.nodes");
    sqlite3_bind_int64(stmt, 1, offset);
    sqlite3_bind_int64(stmt, 2, offset);
    std::string root_id_prefix = std::to_string(root_id);
    sqlite3_bind_text(stmt, 3, root_id_prefix.c_str(), -1, SQLITE_TRANSIENT);
    step_done_or_throw(conn_.raw(), stmt, "Copy workspace nodes failed");
    copied = sqlite3_changes64(conn_.raw());
    sqlite3_finalize(stmt);
    log_workspace_phase("nodes copied", phase, "(" + std::to_string(copied) + " rows)");

    // 3. Edges — re-key src_id and dst_id
    phase = WorkspaceClock::now();
    stmt = nullptr;
    prepare_or_throw(conn_.raw(), &stmt,
        "INSERT OR IGNORE INTO edges (src_id, dst_id, kind, confidence, evidence) "
        "SELECT (? + src_id), (? + dst_id), kind, confidence, evidence "
        "FROM src.edges");
    sqlite3_bind_int64(stmt, 1, offset);
    sqlite3_bind_int64(stmt, 2, offset);
    step_done_or_throw(conn_.raw(), stmt, "Copy workspace edges failed");
    copied = sqlite3_changes64(conn_.raw());
    sqlite3_finalize(stmt);
    log_workspace_phase("edges copied", phase, "(" + std::to_string(copied) + " rows)");

    // 4. Refs — re-key file_id, resolved_node_id, containing_node_id
    phase = WorkspaceClock::now();
    bool src_has_arg_count = attached_table_has_column(conn_.raw(), "src", "refs", "arg_count");
    bool src_has_arg_pattern = attached_table_has_column(conn_.raw(), "src", "refs", "arg_pattern");
    bool src_has_receiver_type = attached_table_has_column(conn_.raw(), "src", "refs", "receiver_type_hint");
    std::string ref_sql =
        "INSERT INTO refs (id, file_id, kind, name, start_line, start_col, end_line, end_col, "
        "resolved_node_id, evidence, containing_node_id, arg_count, arg_pattern, receiver_type_hint) "
        "SELECT (? + id), (? + file_id), kind, name, start_line, start_col, end_line, end_col, "
        "CASE WHEN resolved_node_id IS NOT NULL THEN (? + resolved_node_id) ELSE NULL END, "
        "evidence, "
        "CASE WHEN containing_node_id IS NOT NULL THEN (? + containing_node_id) ELSE NULL END, " +
        std::string(src_has_arg_count ? "arg_count" : "NULL") + ", " +
        std::string(src_has_arg_pattern ? "arg_pattern" : "NULL") + ", " +
        std::string(src_has_receiver_type ? "receiver_type_hint" : "NULL") +
        " FROM src.refs";
    stmt = nullptr;
    prepare_or_throw(conn_.raw(), &stmt, ref_sql);
    sqlite3_bind_int64(stmt, 1, offset);
    sqlite3_bind_int64(stmt, 2, offset);
    sqlite3_bind_int64(stmt, 3, offset);
    sqlite3_bind_int64(stmt, 4, offset);
    step_done_or_throw(conn_.raw(), stmt, "Copy workspace refs failed");
    copied = sqlite3_changes64(conn_.raw());
    sqlite3_finalize(stmt);
    log_workspace_phase("refs copied", phase, "(" + std::to_string(copied) + " rows)");

    // 5. Populate symbol FTS in one bulk pass. Triggers are disabled by caller.
    phase = WorkspaceClock::now();
    std::string fts_sql =
        "INSERT INTO nodes_fts(rowid, name, qualname, signature, doc) "
        "SELECT id, name, qualname, signature, doc FROM nodes WHERE id >= " +
        std::to_string(offset) + " AND id < " + std::to_string(offset + ID_SPACE);
    conn_.exec(fts_sql);
    log_workspace_phase("nodes_fts", phase);
}

void WorkspaceDB::populate_content_fts_for_root(int64_t root_id) {
    auto phase = WorkspaceClock::now();
    int64_t indexed_files = 0;
    int64_t offset = root_id * ID_SPACE;
    int64_t end = offset + ID_SPACE;
    int64_t last_id = offset - 1;

    while (true) {
        std::vector<std::pair<int64_t, std::string>> files;
        conn_.exec("BEGIN IMMEDIATE");
        try {
            sqlite3_stmt* sel = nullptr;
            prepare_or_throw(conn_.raw(), &sel,
                "SELECT id, path FROM files "
                "WHERE id > ? AND id < ? AND parse_status != 'skipped' "
                "AND id NOT IN (SELECT file_id FROM content_fts_tracker) "
                "ORDER BY id LIMIT ?");
            sqlite3_bind_int64(sel, 1, last_id);
            sqlite3_bind_int64(sel, 2, end);
            sqlite3_bind_int(sel, 3, kContentFtsBatchFiles);

            while (sqlite3_step(sel) == SQLITE_ROW) {
                int64_t file_id = sqlite3_column_int64(sel, 0);
                const auto* path_text = sqlite3_column_text(sel, 1);
                if (!path_text) continue;
                files.emplace_back(file_id, reinterpret_cast<const char*>(path_text));
                last_id = file_id;
            }
            sqlite3_finalize(sel);

            if (files.empty()) {
                conn_.exec("COMMIT");
                break;
            }
            indexed_files += static_cast<int64_t>(files.size());

            sqlite3_stmt* ins = nullptr;
            prepare_or_throw(conn_.raw(), &ins,
                "INSERT INTO content_fts(content, file_id, line_no) VALUES(?, ?, ?)");
            sqlite3_stmt* trk = nullptr;
            prepare_or_throw(conn_.raw(), &trk,
                "INSERT OR IGNORE INTO content_fts_tracker(file_id) VALUES(?)");

            for (const auto& [file_id, path] : files) {
                std::ifstream f(path, std::ios::binary);
                if (!f) continue;

                std::string fc((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
                if (fc.empty()) continue;
                if (fc.size() > 1024 * 1024) fc.resize(1024 * 1024);
                content_fts::insert_lines(ins, trk, file_id, fc);
            }

            sqlite3_finalize(ins);
            sqlite3_finalize(trk);
            conn_.exec("COMMIT");
        } catch (...) {
            conn_.exec("ROLLBACK");
            throw;
        }

        conn_.exec("PRAGMA wal_checkpoint(TRUNCATE)");
    }

    conn_.exec("DELETE FROM kv WHERE key = " +
        sql_quote("workspace_content_fts_pending:" + std::to_string(root_id)));
    log_workspace_phase("content_fts", phase, "(" + std::to_string(indexed_files) + " files)");
}

void WorkspaceDB::resume_pending_content_fts() {
    if (!schema::table_exists(conn_, "kv")) return;

    sqlite3_stmt* stmt = nullptr;
    prepare_or_throw(conn_.raw(), &stmt,
        "SELECT key, value FROM kv WHERE key LIKE 'workspace_content_fts_pending:%'");

    std::vector<int64_t> pending_roots;
    std::vector<std::string> stale_keys;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* value = sqlite3_column_text(stmt, 1);
        if (!value) continue;
        try {
            pending_roots.push_back(std::stoll(reinterpret_cast<const char*>(value)));
        } catch (...) {
            const auto* key = sqlite3_column_text(stmt, 0);
            if (key) {
                stale_keys.emplace_back(reinterpret_cast<const char*>(key));
            }
        }
    }
    sqlite3_finalize(stmt);

    for (const auto& key : stale_keys) {
        conn_.exec("DELETE FROM kv WHERE key = " + sql_quote(key));
    }

    for (int64_t root_id : pending_roots) {
        bool root_exists = exists_int64_range(conn_,
            "SELECT 1 FROM roots WHERE id = ? LIMIT 1", root_id);
        if (root_exists) {
            populate_content_fts_for_root(root_id);
        } else {
            conn_.exec("DELETE FROM kv WHERE key = " +
                sql_quote("workspace_content_fts_pending:" + std::to_string(root_id)));
        }
    }
}

} // namespace codetopo
