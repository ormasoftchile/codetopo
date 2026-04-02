#pragma once

#include "db/connection.h"
#include "index/extractor.h"
#include "index/scanner.h"
#include "util/hash.h"
#include "util/git.h"
#include "db/schema.h"
#include <sqlite3.h>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <iostream>

namespace codetopo {

// T039: Cross-file reference resolver (simplified: name-match only for v1)
// T040: SQLite batch persister with per-file transactions and cascade
// T041: Deleted-file pruning
// T042: FTS5 synchronization (via triggers created in schema)

struct IndexProgress {
    int files_total = 0;
    int files_processed = 0;
    int files_new = 0;
    int files_changed = 0;
    int files_deleted = 0;
    int files_skipped = 0;
    int files_errors = 0;
    std::string current_file;
};

class Persister {
public:
    explicit Persister(Connection& conn) : conn_(conn) {}

    ~Persister() {
        finalize_cached_stmts();
    }

    // Non-copyable, non-movable (owns raw sqlite3_stmt pointers)
    Persister(const Persister&) = delete;
    Persister& operator=(const Persister&) = delete;

    // --- Batch transaction management for bulk loading ---
    void begin_batch() {
        if (!in_batch_) {
            conn_.exec("BEGIN TRANSACTION");
            in_batch_ = true;
            batch_count_ = 0;
        }
    }

    void commit_batch() {
        if (in_batch_) {
            conn_.exec("COMMIT");
            in_batch_ = false;
            batch_count_ = 0;
        }
    }

    // Flush if batch_size reached. Call after each persist_file.
    // Returns true if a COMMIT actually happened.
    bool flush_if_needed(int batch_size) {
        if (in_batch_ && ++batch_count_ >= batch_size) {
            conn_.exec("COMMIT");
            conn_.exec("BEGIN TRANSACTION");
            batch_count_ = 0;
            return true;
        }
        return false;
    }

    // T041: Delete files that no longer exist on disk
    int prune_deleted(const std::vector<std::string>& deleted_paths) {
        if (deleted_paths.empty()) return 0;

        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(conn_.raw(),
            "DELETE FROM files WHERE path = ?", -1, &stmt, nullptr);

        int count = 0;
        for (const auto& path : deleted_paths) {
            sqlite3_reset(stmt);
            sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_DONE) {
                ++count;
            }
        }
        sqlite3_finalize(stmt);
        return count;
    }

    // DEC-026 R4: Detect cold index — skip DELETE when DB has no existing files.
    // Call once before the drain loop starts.
    void enable_cold_index_if_empty() {
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(conn_.raw(), "SELECT count(*) FROM files", -1, &stmt, nullptr);
        if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int64(stmt, 0) == 0) {
            cold_index_ = true;
        }
        sqlite3_finalize(stmt);
    }
    bool is_cold_index() const { return cold_index_; }

    // T040: Persist a single file's extraction results.
    // When used with begin_batch/commit_batch, caller manages the transaction.
    // When used standalone, wraps in its own transaction.
    bool persist_file(const ScannedFile& file,
                      const ExtractionResult& extraction,
                      const std::string& content_hash,
                      const std::string& parse_status,
                      const std::string& parse_error = "") {
        ensure_stmts_cached();

        bool own_txn = !in_batch_;
        if (own_txn) conn_.exec("BEGIN TRANSACTION");

        try {
            // Delete existing file record (cascades to nodes → edges, refs)
            // R4: Skip on cold index — DELETE is a guaranteed no-op on empty tables
            if (!cold_index_) {
                sqlite3_reset(stmt_delete_file_);
                sqlite3_clear_bindings(stmt_delete_file_);
                sqlite3_bind_text(stmt_delete_file_, 1, file.relative_path.c_str(), -1, SQLITE_STATIC);
                sqlite3_step(stmt_delete_file_);
            }

            // Insert file record
            int64_t file_id;
            {
                sqlite3_reset(stmt_insert_file_);
                sqlite3_clear_bindings(stmt_insert_file_);
                sqlite3_bind_text(stmt_insert_file_, 1, file.relative_path.c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt_insert_file_, 2, file.language.c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_int64(stmt_insert_file_, 3, file.size_bytes);
                sqlite3_bind_int64(stmt_insert_file_, 4, file.mtime_ns);
                sqlite3_bind_text(stmt_insert_file_, 5, content_hash.c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt_insert_file_, 6, parse_status.c_str(), -1, SQLITE_STATIC);
                if (parse_error.empty()) {
                    sqlite3_bind_null(stmt_insert_file_, 7);
                } else {
                    sqlite3_bind_text(stmt_insert_file_, 7, parse_error.c_str(), -1, SQLITE_STATIC);
                }
                sqlite3_step(stmt_insert_file_);
                file_id = sqlite3_last_insert_rowid(conn_.raw());
            }

            // Insert file node
            int64_t file_node_id;
            {
                auto file_key = make_file_stable_key(file.relative_path);
                sqlite3_reset(stmt_insert_file_node_);
                sqlite3_clear_bindings(stmt_insert_file_node_);
                sqlite3_bind_text(stmt_insert_file_node_, 1, file.relative_path.c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt_insert_file_node_, 2, file_key.c_str(), -1, SQLITE_STATIC);
                sqlite3_step(stmt_insert_file_node_);
                file_node_id = sqlite3_last_insert_rowid(conn_.raw());
            }

            // Insert symbol nodes
            std::vector<int64_t> symbol_ids;
            {
                for (const auto& sym : extraction.symbols) {
                    sqlite3_reset(stmt_insert_symbol_);
                    sqlite3_clear_bindings(stmt_insert_symbol_);
                    sqlite3_bind_int64(stmt_insert_symbol_, 1, file_id);
                    sqlite3_bind_text(stmt_insert_symbol_, 2, sym.kind.c_str(), -1, SQLITE_STATIC);
                    sqlite3_bind_text(stmt_insert_symbol_, 3, sym.name.c_str(), -1, SQLITE_STATIC);
                    sqlite3_bind_text(stmt_insert_symbol_, 4, sym.qualname.c_str(), -1, SQLITE_STATIC);
                    if (sym.signature.empty()) sqlite3_bind_null(stmt_insert_symbol_, 5);
                    else sqlite3_bind_text(stmt_insert_symbol_, 5, sym.signature.c_str(), -1, SQLITE_STATIC);
                    sqlite3_bind_int(stmt_insert_symbol_, 6, sym.start_line);
                    sqlite3_bind_int(stmt_insert_symbol_, 7, sym.start_col);
                    sqlite3_bind_int(stmt_insert_symbol_, 8, sym.end_line);
                    sqlite3_bind_int(stmt_insert_symbol_, 9, sym.end_col);
                    sqlite3_bind_int(stmt_insert_symbol_, 10, sym.is_definition ? 1 : 0);
                    if (sym.visibility.empty()) sqlite3_bind_null(stmt_insert_symbol_, 11);
                    else sqlite3_bind_text(stmt_insert_symbol_, 11, sym.visibility.c_str(), -1, SQLITE_STATIC);
                    if (sym.doc.empty()) sqlite3_bind_null(stmt_insert_symbol_, 12);
                    else sqlite3_bind_text(stmt_insert_symbol_, 12, sym.doc.c_str(), -1, SQLITE_STATIC);
                    sqlite3_bind_text(stmt_insert_symbol_, 13, sym.stable_key.c_str(), -1, SQLITE_STATIC);

                    sqlite3_step(stmt_insert_symbol_);
                    symbol_ids.push_back(sqlite3_last_insert_rowid(conn_.raw()));
                }
            }

            // Insert refs
            {
                for (const auto& ref : extraction.refs) {
                    sqlite3_reset(stmt_insert_ref_);
                    sqlite3_clear_bindings(stmt_insert_ref_);
                    sqlite3_bind_int64(stmt_insert_ref_, 1, file_id);
                    sqlite3_bind_text(stmt_insert_ref_, 2, ref.kind.c_str(), -1, SQLITE_STATIC);
                    sqlite3_bind_text(stmt_insert_ref_, 3, ref.name.c_str(), -1, SQLITE_STATIC);
                    sqlite3_bind_int(stmt_insert_ref_, 4, ref.start_line);
                    sqlite3_bind_int(stmt_insert_ref_, 5, ref.start_col);
                    sqlite3_bind_int(stmt_insert_ref_, 6, ref.end_line);
                    sqlite3_bind_int(stmt_insert_ref_, 7, ref.end_col);
                    if (ref.evidence.empty()) sqlite3_bind_null(stmt_insert_ref_, 8);
                    else sqlite3_bind_text(stmt_insert_ref_, 8, ref.evidence.c_str(), -1, SQLITE_STATIC);
                    if (ref.containing_symbol_index >= 0 &&
                        ref.containing_symbol_index < static_cast<int>(symbol_ids.size()))
                        sqlite3_bind_int64(stmt_insert_ref_, 9, symbol_ids[ref.containing_symbol_index]);
                    else
                        sqlite3_bind_null(stmt_insert_ref_, 9);
                    sqlite3_step(stmt_insert_ref_);
                }
            }

            // Insert edges (containment: file_node → symbol)
            {
                for (const auto& edge : extraction.edges) {
                    int64_t src_id = (edge.src_index < 0) ? file_node_id : symbol_ids[edge.src_index];
                    int64_t dst_id;

                    if (edge.dst_index >= 0 && edge.dst_index < static_cast<int>(symbol_ids.size())) {
                        dst_id = symbol_ids[edge.dst_index];
                    } else {
                        continue;  // Unresolved cross-file edge — skip for now
                    }

                    if (edge.confidence < 0.3) continue;  // FR-044

                    sqlite3_reset(stmt_insert_edge_);
                    sqlite3_clear_bindings(stmt_insert_edge_);
                    sqlite3_bind_int64(stmt_insert_edge_, 1, src_id);
                    sqlite3_bind_int64(stmt_insert_edge_, 2, dst_id);
                    sqlite3_bind_text(stmt_insert_edge_, 3, edge.kind.c_str(), -1, SQLITE_STATIC);
                    sqlite3_bind_double(stmt_insert_edge_, 4, edge.confidence);
                    if (edge.evidence.empty()) sqlite3_bind_null(stmt_insert_edge_, 5);
                    else sqlite3_bind_text(stmt_insert_edge_, 5, edge.evidence.c_str(), -1, SQLITE_STATIC);
                    sqlite3_step(stmt_insert_edge_);
                }
            }

            if (own_txn) conn_.exec("COMMIT");
            return true;

        } catch (...) {
            if (own_txn || in_batch_) {
                conn_.exec("ROLLBACK");
                in_batch_ = false;
                batch_count_ = 0;
            }
            return false;
        }
    }

    // T049: Write kv metadata
    void write_metadata(const std::string& repo_root) {
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf{};
#ifdef _WIN32
        gmtime_s(&tm_buf, &t);
#else
        gmtime_r(&t, &tm_buf);
#endif
        std::ostringstream time_ss;
        time_ss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%SZ");

        schema::set_kv(conn_, "schema_version", std::to_string(CURRENT_SCHEMA_VERSION));
        schema::set_kv(conn_, "indexer_version", INDEXER_VERSION);
        schema::set_kv(conn_, "repo_root", repo_root);
        schema::set_kv(conn_, "last_index_time", time_ss.str());
        schema::set_kv(conn_, "language_coverage", "c,cpp,csharp,typescript,go,yaml");

        auto head = get_git_head(repo_root);
        auto branch = get_git_branch(repo_root);
        if (!head.empty()) schema::set_kv(conn_, "git_head", head);
        if (!branch.empty()) schema::set_kv(conn_, "git_branch", branch);
    }

    // T039: Post-index cross-file reference resolution.
    // Uses in-memory hash map for O(N) resolution instead of O(N*M) correlated subqueries.
    // Returns {refs_resolved, edges_created}.
    std::pair<int,int> resolve_references() {
        int total_resolved = 0;
        int edges_created = 0;

        // Disable FK checks — all IDs originate from the DB itself.
        conn_.exec("PRAGMA foreign_keys = OFF");

        // --- Step 1: Build in-memory lookup: name → (node_id, file_id, is_definition) ---
        // Also builds class_map in the same scan (merges former Step 4).
        // For each symbol name, keep the best candidate (prefer definitions, then lowest id).
        struct SymbolEntry {
            int64_t id;
            int64_t file_id;
            bool is_definition;
        };
        std::unordered_map<std::string, SymbolEntry> symbol_map;
        symbol_map.reserve(2500000);

        struct ClassEntry { int64_t id; bool is_def; };
        std::unordered_map<std::string, ClassEntry> class_map;

        {
            sqlite3_stmt* stmt = nullptr;
            sqlite3_prepare_v2(conn_.raw(),
                "SELECT id, name, file_id, is_definition, kind FROM nodes WHERE node_type = 'symbol'",
                -1, &stmt, nullptr);

            int loaded = 0;
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                int64_t id = sqlite3_column_int64(stmt, 0);
                const char* name_raw = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                int64_t file_id = sqlite3_column_int64(stmt, 2);
                bool is_def = sqlite3_column_int(stmt, 3) != 0;
                const char* kind_raw = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));

                if (!name_raw) continue;
                std::string name(name_raw);

                auto it = symbol_map.find(name);
                if (it == symbol_map.end()) {
                    symbol_map[name] = {id, file_id, is_def};
                } else {
                    auto& existing = it->second;
                    if ((!existing.is_definition && is_def) ||
                        (existing.is_definition == is_def && id < existing.id)) {
                        existing = {id, file_id, is_def};
                    }
                }

                // Build class_map inline (replaces former Step 4 scan)
                if (kind_raw) {
                    std::string_view kind_sv(kind_raw);
                    if (kind_sv == "class" || kind_sv == "struct" || kind_sv == "interface") {
                        auto cit = class_map.find(name);
                        if (cit == class_map.end()) {
                            class_map[name] = {id, is_def};
                        } else if (!cit->second.is_def && is_def) {
                            cit->second = {id, is_def};
                        }
                    }
                }

                ++loaded;
            }
            sqlite3_finalize(stmt);
            std::cerr << "  Loaded " << loaded << " symbols into lookup map ("
                      << symbol_map.size() << " unique names)\n";
        }

        // --- Step 2: Build file-node lookup: file path → file node id ---
        std::unordered_map<std::string, int64_t> file_node_map;
        {
            sqlite3_stmt* stmt = nullptr;
            sqlite3_prepare_v2(conn_.raw(),
                "SELECT id, name FROM nodes WHERE node_type = 'file'",
                -1, &stmt, nullptr);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                int64_t id = sqlite3_column_int64(stmt, 0);
                const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                if (name) file_node_map[name] = id;
            }
            sqlite3_finalize(stmt);
        }

        // --- Step 3: Build include lookup: filename → file node id ---
        // For includes like "foo.h", we match the file node whose path ends with "/foo.h"
        std::unordered_map<std::string, int64_t> include_map;
        for (const auto& [path, node_id] : file_node_map) {
            auto slash_pos = path.rfind('/');
            std::string basename = (slash_pos != std::string::npos) ? path.substr(slash_pos + 1) : path;
            // First match wins — for ambiguous includes, any match is fine (confidence=0.7)
            if (include_map.find(basename) == include_map.end()) {
                include_map[basename] = node_id;
            }
        }

        // --- Step 4: (merged into Step 1 above) ---

        // --- Step 5: Read all unresolved refs, resolve in memory, batch-update ---
        std::cerr << "  Resolving refs in memory...\n";

        // Prepare the UPDATE statement (by rowid — fastest possible)
        sqlite3_stmt* update_stmt = nullptr;
        sqlite3_prepare_v2(conn_.raw(),
            "UPDATE refs SET resolved_node_id = ? WHERE id = ?",
            -1, &update_stmt, nullptr);

        // Also need file_id → file path for the file_node_map lookup
        std::unordered_map<int64_t, std::string> fileid_to_path;
        {
            sqlite3_stmt* stmt = nullptr;
            sqlite3_prepare_v2(conn_.raw(),
                "SELECT id, path FROM files", -1, &stmt, nullptr);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                fileid_to_path[sqlite3_column_int64(stmt, 0)] =
                    reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            }
            sqlite3_finalize(stmt);
        }

        // Scan all unresolved refs and resolve, collecting edge tuples in memory
        sqlite3_stmt* ref_stmt = nullptr;
        sqlite3_prepare_v2(conn_.raw(),
            "SELECT id, file_id, kind, name FROM refs WHERE resolved_node_id IS NULL",
            -1, &ref_stmt, nullptr);

        // Edge tuples collected during resolution — avoids expensive SQL join in Step 6
        struct EdgeTuple {
            int64_t src_id;   // file node id
            int64_t dst_id;   // resolved target node id
            const char* kind; // edge kind (static string literal)
        };
        std::vector<EdgeTuple> edge_tuples;
        edge_tuples.reserve(1000000);

        conn_.exec("BEGIN TRANSACTION");
        int batch = 0;
        int call_resolved = 0, include_resolved = 0, inherit_resolved = 0;
        std::string name;
        name.reserve(256);  // reuse buffer across iterations

        while (sqlite3_step(ref_stmt) == SQLITE_ROW) {
            int64_t ref_id = sqlite3_column_int64(ref_stmt, 0);
            int64_t ref_file_id = sqlite3_column_int64(ref_stmt, 1);
            const char* kind_raw = reinterpret_cast<const char*>(sqlite3_column_text(ref_stmt, 2));
            const char* name_raw = reinterpret_cast<const char*>(sqlite3_column_text(ref_stmt, 3));
            if (!kind_raw || !name_raw) continue;

            std::string_view kind(kind_raw);
            name.assign(name_raw);
            int64_t resolved_id = 0;
            bool resolved = false;
            const char* edge_kind = nullptr;

            if (kind == "call") {
                auto it = symbol_map.find(name);
                if (it != symbol_map.end() && it->second.file_id != ref_file_id) {
                    resolved_id = it->second.id;
                    resolved = true;
                    edge_kind = "calls";
                    ++call_resolved;
                }
            } else if (kind == "include") {
                auto it = include_map.find(name);
                if (it != include_map.end()) {
                    resolved_id = it->second;
                    resolved = true;
                    edge_kind = "includes";
                    ++include_resolved;
                }
            } else if (kind == "inherit") {
                auto it = class_map.find(name);
                if (it != class_map.end()) {
                    resolved_id = it->second.id;
                    resolved = true;
                    edge_kind = "inherits";
                    ++inherit_resolved;
                }
            }

            if (resolved) {
                sqlite3_reset(update_stmt);
                sqlite3_bind_int64(update_stmt, 1, resolved_id);
                sqlite3_bind_int64(update_stmt, 2, ref_id);
                sqlite3_step(update_stmt);
                ++total_resolved;

                // Edge tuple: file_node → resolved target (kind already set above)
                auto path_it = fileid_to_path.find(ref_file_id);
                if (path_it != fileid_to_path.end()) {
                    auto fn_it = file_node_map.find(path_it->second);
                    if (fn_it != file_node_map.end()) {
                        edge_tuples.push_back({fn_it->second, resolved_id, edge_kind});
                    }
                }

                if (++batch >= 100000) {
                    conn_.exec("COMMIT");
                    conn_.exec("BEGIN TRANSACTION");
                    batch = 0;
                }
            }
        }
        conn_.exec("COMMIT");

        sqlite3_finalize(ref_stmt);
        sqlite3_finalize(update_stmt);

        std::cerr << "  Resolved " << call_resolved << " call, "
                  << include_resolved << " include, "
                  << inherit_resolved << " inherit refs\n";

        // --- Step 6: Delete stale cross-ref edges, then batch-insert from in-memory tuples ---
        // Without a unique constraint, re-runs would accumulate duplicate edges.
        // Delete only resolver-created edges (confidence=0.7, name-match evidence).
        std::cerr << "  Clearing old cross-ref edges...\n";
        conn_.exec("DELETE FROM edges WHERE kind IN ('calls','includes','inherits') AND evidence = 'name-match'");
        std::cerr << "  Inserting " << edge_tuples.size() << " edges...\n";
        conn_.exec("BEGIN TRANSACTION");

        sqlite3_stmt* edge_stmt = nullptr;
        sqlite3_prepare_v2(conn_.raw(),
            "INSERT OR IGNORE INTO edges(src_id, dst_id, kind, confidence, evidence) "
            "VALUES(?, ?, ?, 0.7, 'name-match')",
            -1, &edge_stmt, nullptr);

        int edge_batch = 0;
        for (const auto& e : edge_tuples) {
            sqlite3_reset(edge_stmt);
            sqlite3_bind_int64(edge_stmt, 1, e.src_id);
            sqlite3_bind_int64(edge_stmt, 2, e.dst_id);
            sqlite3_bind_text(edge_stmt, 3, e.kind, -1, SQLITE_STATIC);
            sqlite3_step(edge_stmt);
            ++edges_created;

            if (++edge_batch >= 100000) {
                conn_.exec("COMMIT");
                conn_.exec("BEGIN TRANSACTION");
                edge_batch = 0;
            }
        }
        conn_.exec("COMMIT");
        sqlite3_finalize(edge_stmt);
        std::cerr << "  Created " << edges_created << " edges\n";

        conn_.exec("PRAGMA foreign_keys = ON");
        return {total_resolved, edges_created};
    }

private:
    Connection& conn_;
    bool in_batch_ = false;
    int batch_count_ = 0;

    // Cached prepared statements for persist_file() — prepared once, reused via reset/clear_bindings
    sqlite3_stmt* stmt_delete_file_ = nullptr;
    sqlite3_stmt* stmt_insert_file_ = nullptr;
    sqlite3_stmt* stmt_insert_file_node_ = nullptr;
    sqlite3_stmt* stmt_insert_symbol_ = nullptr;
    sqlite3_stmt* stmt_insert_ref_ = nullptr;
    sqlite3_stmt* stmt_insert_edge_ = nullptr;
    bool stmts_cached_ = false;
    bool cold_index_ = false;   // R4: skip DELETE on cold index (empty files table)

    void ensure_stmts_cached() {
        if (stmts_cached_) return;

        sqlite3_prepare_v2(conn_.raw(),
            "DELETE FROM files WHERE path = ?", -1, &stmt_delete_file_, nullptr);

        sqlite3_prepare_v2(conn_.raw(),
            "INSERT INTO files(path, language, size_bytes, mtime_ns, content_hash, parse_status, parse_error) "
            "VALUES(?, ?, ?, ?, ?, ?, ?)", -1, &stmt_insert_file_, nullptr);

        sqlite3_prepare_v2(conn_.raw(),
            "INSERT INTO nodes(node_type, file_id, kind, name, stable_key) "
            "VALUES('file', NULL, 'file', ?, ?)", -1, &stmt_insert_file_node_, nullptr);

        sqlite3_prepare_v2(conn_.raw(),
            "INSERT INTO nodes(node_type, file_id, kind, name, qualname, signature, "
            "start_line, start_col, end_line, end_col, is_definition, visibility, doc, stable_key) "
            "VALUES('symbol', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", -1, &stmt_insert_symbol_, nullptr);

        sqlite3_prepare_v2(conn_.raw(),
            "INSERT INTO refs(file_id, kind, name, start_line, start_col, end_line, end_col, evidence, containing_node_id) "
            "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)", -1, &stmt_insert_ref_, nullptr);

        sqlite3_prepare_v2(conn_.raw(),
            "INSERT INTO edges(src_id, dst_id, kind, confidence, evidence) "
            "VALUES(?, ?, ?, ?, ?)", -1, &stmt_insert_edge_, nullptr);

        stmts_cached_ = true;
    }

    void finalize_cached_stmts() {
        if (stmt_delete_file_)     { sqlite3_finalize(stmt_delete_file_);     stmt_delete_file_ = nullptr; }
        if (stmt_insert_file_)     { sqlite3_finalize(stmt_insert_file_);     stmt_insert_file_ = nullptr; }
        if (stmt_insert_file_node_){ sqlite3_finalize(stmt_insert_file_node_);stmt_insert_file_node_ = nullptr; }
        if (stmt_insert_symbol_)   { sqlite3_finalize(stmt_insert_symbol_);   stmt_insert_symbol_ = nullptr; }
        if (stmt_insert_ref_)      { sqlite3_finalize(stmt_insert_ref_);      stmt_insert_ref_ = nullptr; }
        if (stmt_insert_edge_)     { sqlite3_finalize(stmt_insert_edge_);     stmt_insert_edge_ = nullptr; }
        stmts_cached_ = false;
    }
};

} // namespace codetopo
