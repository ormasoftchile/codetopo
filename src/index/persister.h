#pragma once

#include "db/connection.h"
#include "index/extractor.h"
#include "index/call_binding_db.h"
#include "index/scanner.h"
#include "util/hash.h"
#include "util/git.h"
#include "util/log.h"
#include "db/schema.h"
#include "db/owned_nodes.h"
#include "db/fts.h"
#include "db/bind.h"
#include <sqlite3.h>
#include <algorithm>
#include <atomic>
#include <thread>
#include <cctype>
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
    Persister(const Persister&) = delete("Persister owns raw sqlite3_stmt pointers bound to a connection and cannot be copied");
    Persister& operator=(const Persister&) = delete("Persister owns raw sqlite3_stmt pointers bound to a connection and cannot be copied");

    // --- Batch transaction management for bulk loading ---
    void begin_batch() {
        if (!in_batch_) {
            conn_.exec("BEGIN IMMEDIATE");
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
            conn_.exec("BEGIN IMMEDIATE");
            batch_count_ = 0;
            return true;
        }
        return false;
    }

    // T041: Delete files that no longer exist on disk.
    // Guards with root_id IS NULL so workspace root files are never pruned here.
    int prune_deleted(const std::vector<std::string>& deleted_paths) {
        if (deleted_paths.empty()) return 0;

        sqlite3_stmt* find_stmt = nullptr;
        sqlite3_prepare_v2(conn_.raw(),
            "SELECT id FROM files WHERE path = ? AND root_id IS NULL", -1, &find_stmt, nullptr);
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(conn_.raw(),
            "DELETE FROM files WHERE path = ? AND root_id IS NULL", -1, &stmt, nullptr);

        int count = 0;
        for (const auto& path : deleted_paths) {
            sqlite3_reset(find_stmt);
            sqlite3_bind_text(find_stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
            while (sqlite3_step(find_stmt) == SQLITE_ROW) {
                content_fts::delete_file(conn_, sqlite3_column_int64(find_stmt, 0));
            }

            sqlite3_reset(stmt);
            sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_DONE) {
                ++count;
            }
        }
        sqlite3_finalize(find_stmt);
        sqlite3_finalize(stmt);
        return count;
    }

    // DEC-026 R4: Detect cold index — skip DELETE when DB has no existing files.
    // Only count main-project files (root_id IS NULL) — extra workspace roots
    // (root_id NOT NULL) should not affect cold-index detection for the main project.
    void enable_cold_index_if_empty() {
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(conn_.raw(),
            "SELECT count(*) FROM files WHERE root_id IS NULL", -1, &stmt, nullptr);
        if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int64(stmt, 0) == 0) {
            cold_index_ = true;
        }
        sqlite3_finalize(stmt);
    }
    bool is_cold_index() const { return cold_index_; }

    // Returns the file_id assigned by the last persist_file() call.
    int64_t last_file_id() const { return last_file_id_; }

    // Clear the main-project index in bulk for --force reindex.
    // Keeps schema, kv, quarantine, and workspace-root rows intact.  If the
    // database only contains main-project files, this takes the fast truncate-
    // style path; otherwise it deletes only rows reachable from root_id IS NULL.
    int64_t clear_main_project_index_for_force() {
        finalize_cached_stmts();

        int64_t main_files = scalar_int64(
            "SELECT COUNT(*) FROM files WHERE root_id IS NULL");
        if (main_files == 0) {
            cold_index_ = true;
            return 0;
        }

        bool has_workspace_files = scalar_int64(
            "SELECT COUNT(*) FROM files WHERE root_id IS NOT NULL") > 0;

        conn_.exec("PRAGMA foreign_keys = OFF");
        try {
            conn_.exec("BEGIN TRANSACTION");

            if (!has_workspace_files) {
                conn_.exec("INSERT INTO content_fts(content_fts) VALUES('delete-all')");
                conn_.exec("DELETE FROM content_fts_tracker");
                conn_.exec("DELETE FROM edges");
                conn_.exec("DELETE FROM refs");
                conn_.exec("DELETE FROM nodes");
                conn_.exec("DELETE FROM files");
            } else {
                conn_.exec("DROP TABLE IF EXISTS __codetopo_force_file_ids");
                conn_.exec("DROP TABLE IF EXISTS __codetopo_force_node_ids");
                conn_.exec("CREATE TABLE __codetopo_force_file_ids(id INTEGER PRIMARY KEY)");
                conn_.exec("CREATE TABLE __codetopo_force_node_ids(id INTEGER PRIMARY KEY)");
                conn_.exec(
                    "INSERT INTO __codetopo_force_file_ids(id) "
                    "SELECT id FROM files WHERE root_id IS NULL");
                conn_.exec(
                    "INSERT OR IGNORE INTO __codetopo_force_node_ids(id) "
                    "SELECT id FROM nodes "
                    "WHERE file_id IN (SELECT id FROM __codetopo_force_file_ids)");
                conn_.exec(
                    "INSERT OR IGNORE INTO __codetopo_force_node_ids(id) "
                    "SELECT n.id FROM __codetopo_force_file_ids t "
                    "CROSS JOIN files f ON f.id=t.id "
                    "CROSS JOIN nodes n INDEXED BY idx_nodes_stable_key ON n.stable_key=f.path||'::file' "
                    "WHERE n.node_type='file'");

                conn_.exec(
                    "DELETE FROM content_fts "
                    "WHERE file_id IN (SELECT id FROM __codetopo_force_file_ids)");
                conn_.exec(
                    "DELETE FROM content_fts_tracker "
                    "WHERE file_id IN (SELECT id FROM __codetopo_force_file_ids)");
                conn_.exec(
                    "DELETE FROM edges "
                    "WHERE src_id IN (SELECT id FROM __codetopo_force_node_ids) "
                    "   OR dst_id IN (SELECT id FROM __codetopo_force_node_ids)");
                conn_.exec(
                    "DELETE FROM refs "
                    "WHERE file_id IN (SELECT id FROM __codetopo_force_file_ids)");
                conn_.exec(
                    "UPDATE refs SET resolved_node_id = NULL "
                    "WHERE resolved_node_id IN (SELECT id FROM __codetopo_force_node_ids)");
                conn_.exec(
                    "UPDATE refs SET containing_node_id = NULL "
                    "WHERE containing_node_id IN (SELECT id FROM __codetopo_force_node_ids)");
                conn_.exec(
                    "DELETE FROM nodes "
                    "WHERE id IN (SELECT id FROM __codetopo_force_node_ids)");
                conn_.exec(
                    "DELETE FROM files "
                    "WHERE id IN (SELECT id FROM __codetopo_force_file_ids)");
                conn_.exec("DROP TABLE __codetopo_force_node_ids");
                conn_.exec("DROP TABLE __codetopo_force_file_ids");
            }

            conn_.exec("COMMIT");
        } catch (...) {
            try { conn_.exec("ROLLBACK"); } catch (...) {}
            conn_.exec("PRAGMA foreign_keys = ON");
            throw;
        }
        conn_.exec("PRAGMA foreign_keys = ON");

        cold_index_ = true;
        return main_files;
    }

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
        if (own_txn) conn_.exec("BEGIN IMMEDIATE");

        try {
            int64_t file_id = 0;
            int64_t file_node_id = 0;
            std::vector<int64_t> symbol_ids;
            symbol_ids.reserve(extraction.symbols.size());

            int64_t existing_file_id = 0;
            if (!cold_index_) {
                sqlite3_reset(stmt_find_file_);
                sqlite3_bind_text(stmt_find_file_, 1, file.relative_path.c_str(), -1, SQLITE_STATIC);
                const int find_rc = sqlite3_step(stmt_find_file_);
                if (find_rc == SQLITE_ROW) {
                    existing_file_id = sqlite3_column_int64(stmt_find_file_, 0);
                } else if (find_rc != SQLITE_DONE) {
                    sqlite3_reset(stmt_find_file_);
                    throw SqliteError(find_rc, "Find existing file failed: " + std::string(sqlite3_errmsg(conn_.raw())));
                }
                sqlite3_reset(stmt_find_file_);
            }

            if (existing_file_id > 0) {
                // In-place UPSERT path: preserves durable node identity, centrality rank,
                // external incoming edges from other files, and runtime trace evidence!
                file_id = existing_file_id;
                last_file_id_ = file_id;
                content_fts::delete_file(conn_, existing_file_id);

                // Update files row in place
                db::bind(stmt_update_file_,
                         file.language,
                         file.size_bytes,
                         file.mtime_ns,
                         content_hash,
                         parse_status,
                         parse_error.empty() ? nullptr : parse_error.c_str(),
                         file_id);
                step_write(stmt_update_file_);

                file_node_id = ensure_file_node(file.relative_path);

                // Load existing symbol nodes for this file into map (stable_key -> node_id)
                std::unordered_map<std::string, int64_t> existing_symbols;
                sqlite3_stmt* sym_stmt = nullptr;
                sqlite3_prepare_v2(conn_.raw(),
                    "SELECT id, stable_key FROM nodes WHERE file_id = ? AND node_type = 'symbol'",
                    -1, &sym_stmt, nullptr);
                sqlite3_bind_int64(sym_stmt, 1, file_id);
                while (sqlite3_step(sym_stmt) == SQLITE_ROW) {
                    int64_t nid = sqlite3_column_int64(sym_stmt, 0);
                    const unsigned char* sk = sqlite3_column_text(sym_stmt, 1);
                    if (sk) existing_symbols[reinterpret_cast<const char*>(sk)] = nid;
                }
                sqlite3_finalize(sym_stmt);

                // Match and upsert symbols
                for (const auto& sym : extraction.symbols) {
                    auto it = existing_symbols.find(sym.stable_key);
                    if (it != existing_symbols.end()) {
                        int64_t existing_id = it->second;
                        db::bind(stmt_update_symbol_,
                                 sym.kind,
                                 sym.name,
                                 sym.qualname,
                                 sym.signature.empty() ? nullptr : sym.signature.c_str(),
                                 sym.fingerprint.empty() ? nullptr : sym.fingerprint.c_str(),
                                 sym.start_line,
                                 sym.start_col,
                                 sym.end_line,
                                 sym.end_col,
                                 sym.is_definition,
                                 sym.visibility.empty() ? nullptr : sym.visibility.c_str(),
                                 sym.doc.empty() ? nullptr : sym.doc.c_str(),
                                 existing_id);
                        step_write(stmt_update_symbol_);
                        symbol_ids.push_back(existing_id);
                        existing_symbols.erase(it);
                    } else {
                        db::bind(stmt_insert_symbol_,
                                 file_id,
                                 sym.kind,
                                 sym.name,
                                 sym.qualname,
                                 sym.signature.empty() ? nullptr : sym.signature.c_str(),
                                 sym.fingerprint.empty() ? nullptr : sym.fingerprint.c_str(),
                                 sym.start_line,
                                 sym.start_col,
                                 sym.end_line,
                                 sym.end_col,
                                 sym.is_definition,
                                 sym.visibility.empty() ? nullptr : sym.visibility.c_str(),
                                 sym.doc.empty() ? nullptr : sym.doc.c_str(),
                                 sym.stable_key);
                        step_write(stmt_insert_symbol_);
                        symbol_ids.push_back(sqlite3_last_insert_rowid(conn_.raw()));
                    }
                }

                // Delete symbols that were removed from the file
                for (const auto& [_, old_id] : existing_symbols) {
                    db::bind(stmt_delete_symbol_, old_id);
                    step_write(stmt_delete_symbol_);
                }

                // Delete old refs for this file
                db::bind(stmt_delete_refs_, file_id);
                step_write(stmt_delete_refs_);

                // Delete old outgoing static edges from this file
                db::bind(stmt_delete_file_edges_, file_node_id, file_id);
                step_write(stmt_delete_file_edges_);
            } else {
                // Cold insert branch
                db::bind(stmt_insert_file_,
                         file.relative_path,
                         file.language,
                         file.size_bytes,
                         file.mtime_ns,
                         content_hash,
                         parse_status,
                         parse_error.empty() ? nullptr : parse_error.c_str());
                step_write(stmt_insert_file_);
                file_id = sqlite3_last_insert_rowid(conn_.raw());
                last_file_id_ = file_id;

                // File nodes have NULL file_id and can survive an earlier file prune.
                // Reuse their stable identity so incoming include/workspace links survive.
                file_node_id = ensure_file_node(file.relative_path);
                db::bind(stmt_delete_file_edges_, file_node_id, file_id);
                step_write(stmt_delete_file_edges_);

                // Insert symbol nodes (DEC-039 OPT-1: batched 100-row INSERT)
                const int SYMBOL_BATCH_SIZE = 100;
                int num_syms = static_cast<int>(extraction.symbols.size());
                int full_chunks = num_syms / SYMBOL_BATCH_SIZE;
                int remainder = num_syms % SYMBOL_BATCH_SIZE;

                // Full chunks: use batch INSERT
                if (full_chunks > 0) {
                    ensure_batch_symbol_stmt();
                    for (int c = 0; c < full_chunks; ++c) {
                        sqlite3_reset(stmt_batch_insert_symbol_);

                        for (int r = 0; r < SYMBOL_BATCH_SIZE; ++r) {
                            int idx = c * SYMBOL_BATCH_SIZE + r;
                            const auto& sym = extraction.symbols[idx];
                            int base_param = r * 14 + 1;  // 14 params per symbol

                            sqlite3_bind_int64(stmt_batch_insert_symbol_, base_param + 0, file_id);
                            sqlite3_bind_text(stmt_batch_insert_symbol_, base_param + 1, sym.kind.c_str(), -1, SQLITE_STATIC);
                            sqlite3_bind_text(stmt_batch_insert_symbol_, base_param + 2, sym.name.c_str(), -1, SQLITE_STATIC);
                            sqlite3_bind_text(stmt_batch_insert_symbol_, base_param + 3, sym.qualname.c_str(), -1, SQLITE_STATIC);
                            if (sym.signature.empty()) sqlite3_bind_null(stmt_batch_insert_symbol_, base_param + 4);
                            else sqlite3_bind_text(stmt_batch_insert_symbol_, base_param + 4, sym.signature.c_str(), -1, SQLITE_STATIC);
                            if (sym.fingerprint.empty()) sqlite3_bind_null(stmt_batch_insert_symbol_, base_param + 5);
                            else sqlite3_bind_text(stmt_batch_insert_symbol_, base_param + 5, sym.fingerprint.c_str(), -1, SQLITE_STATIC);
                            sqlite3_bind_int(stmt_batch_insert_symbol_, base_param + 6, sym.start_line);
                            sqlite3_bind_int(stmt_batch_insert_symbol_, base_param + 7, sym.start_col);
                            sqlite3_bind_int(stmt_batch_insert_symbol_, base_param + 8, sym.end_line);
                            sqlite3_bind_int(stmt_batch_insert_symbol_, base_param + 9, sym.end_col);
                            sqlite3_bind_int(stmt_batch_insert_symbol_, base_param + 10, sym.is_definition ? 1 : 0);
                            if (sym.visibility.empty()) sqlite3_bind_null(stmt_batch_insert_symbol_, base_param + 11);
                            else sqlite3_bind_text(stmt_batch_insert_symbol_, base_param + 11, sym.visibility.c_str(), -1, SQLITE_STATIC);
                            if (sym.doc.empty()) sqlite3_bind_null(stmt_batch_insert_symbol_, base_param + 12);
                            else sqlite3_bind_text(stmt_batch_insert_symbol_, base_param + 12, sym.doc.c_str(), -1, SQLITE_STATIC);
                            sqlite3_bind_text(stmt_batch_insert_symbol_, base_param + 13, sym.stable_key.c_str(), -1, SQLITE_STATIC);
                        }
                        step_write(stmt_batch_insert_symbol_);
                        // Compute IDs arithmetically: last_insert_rowid is the LAST row's ID
                        int64_t last_id = sqlite3_last_insert_rowid(conn_.raw());
                        int64_t first_id = last_id - (SYMBOL_BATCH_SIZE - 1);
                        for (int r = 0; r < SYMBOL_BATCH_SIZE; ++r) {
                            symbol_ids.push_back(first_id + r);
                        }
                    }
                }

                // Remainder: use single-row INSERT
                for (int r = 0; r < remainder; ++r) {
                    int idx = full_chunks * SYMBOL_BATCH_SIZE + r;
                    const auto& sym = extraction.symbols[idx];

                    db::bind(stmt_insert_symbol_,
                             file_id,
                             sym.kind,
                             sym.name,
                             sym.qualname,
                             sym.signature.empty() ? nullptr : sym.signature.c_str(),
                             sym.fingerprint.empty() ? nullptr : sym.fingerprint.c_str(),
                             sym.start_line,
                             sym.start_col,
                             sym.end_line,
                             sym.end_col,
                             sym.is_definition,
                             sym.visibility.empty() ? nullptr : sym.visibility.c_str(),
                             sym.doc.empty() ? nullptr : sym.doc.c_str(),
                             sym.stable_key);

                    step_write(stmt_insert_symbol_);
                    symbol_ids.push_back(sqlite3_last_insert_rowid(conn_.raw()));
                }
            }

            // Insert refs (DEC-038 OPT-2: batched 80-row INSERT)
            {
                const int REF_BATCH_SIZE = 80;
                int num_refs = static_cast<int>(extraction.refs.size());
                int full_chunks = num_refs / REF_BATCH_SIZE;
                int remainder = num_refs % REF_BATCH_SIZE;
                
                // Full chunks: use batch INSERT
                if (full_chunks > 0) {
                    ensure_batch_ref_stmt();
                    for (int c = 0; c < full_chunks; ++c) {
                        sqlite3_reset(stmt_batch_insert_ref_);
                        
                        for (int r = 0; r < REF_BATCH_SIZE; ++r) {
                            int idx = c * REF_BATCH_SIZE + r;
                            const auto& ref = extraction.refs[idx];
                            int base_param = r * 12 + 1;  // 12 params per ref
                            
                            sqlite3_bind_int64(stmt_batch_insert_ref_, base_param + 0, file_id);
                            sqlite3_bind_text(stmt_batch_insert_ref_, base_param + 1, ref.kind.c_str(), -1, SQLITE_STATIC);
                            sqlite3_bind_text(stmt_batch_insert_ref_, base_param + 2, ref.name.c_str(), -1, SQLITE_STATIC);
                            sqlite3_bind_int(stmt_batch_insert_ref_, base_param + 3, ref.start_line);
                            sqlite3_bind_int(stmt_batch_insert_ref_, base_param + 4, ref.start_col);
                            sqlite3_bind_int(stmt_batch_insert_ref_, base_param + 5, ref.end_line);
                            sqlite3_bind_int(stmt_batch_insert_ref_, base_param + 6, ref.end_col);
                            if (ref.evidence.empty()) 
                                sqlite3_bind_null(stmt_batch_insert_ref_, base_param + 7);
                            else 
                                sqlite3_bind_text(stmt_batch_insert_ref_, base_param + 7, ref.evidence.c_str(), -1, SQLITE_STATIC);
                            if (ref.containing_symbol_index >= 0 && 
                                ref.containing_symbol_index < static_cast<int>(symbol_ids.size()))
                                sqlite3_bind_int64(stmt_batch_insert_ref_, base_param + 8, symbol_ids[ref.containing_symbol_index]);
                            else
                                sqlite3_bind_null(stmt_batch_insert_ref_, base_param + 8);
                            if (ref.arg_count >= 0)
                                sqlite3_bind_int(stmt_batch_insert_ref_, base_param + 9, ref.arg_count);
                            else
                                sqlite3_bind_null(stmt_batch_insert_ref_, base_param + 9);
                            if (ref.arg_pattern.empty())
                                sqlite3_bind_null(stmt_batch_insert_ref_, base_param + 10);
                            else
                                sqlite3_bind_text(stmt_batch_insert_ref_, base_param + 10, ref.arg_pattern.c_str(), -1, SQLITE_STATIC);
                            if (ref.receiver_type_hint.empty())
                                sqlite3_bind_null(stmt_batch_insert_ref_, base_param + 11);
                            else
                                sqlite3_bind_text(stmt_batch_insert_ref_, base_param + 11, ref.receiver_type_hint.c_str(), -1, SQLITE_STATIC);
                        }
                        step_write(stmt_batch_insert_ref_);
                    }
                }
                
                // Remainder: use single-row INSERT
                for (int r = 0; r < remainder; ++r) {
                    int idx = full_chunks * REF_BATCH_SIZE + r;
                    const auto& ref = extraction.refs[idx];
                    
                    db::bind(stmt_insert_ref_,
                             file_id,
                             ref.kind,
                             ref.name,
                             ref.start_line,
                             ref.start_col,
                             ref.end_line,
                             ref.end_col,
                             ref.evidence.empty() ? nullptr : ref.evidence.c_str(),
                             (ref.containing_symbol_index >= 0 &&
                              ref.containing_symbol_index < static_cast<int>(symbol_ids.size()))
                                 ? std::optional<int64_t>(symbol_ids[ref.containing_symbol_index])
                                 : std::nullopt,
                             ref.arg_count >= 0 ? std::optional<int>(ref.arg_count) : std::nullopt,
                             ref.arg_pattern.empty() ? nullptr : ref.arg_pattern.c_str(),
                             ref.receiver_type_hint.empty() ? nullptr : ref.receiver_type_hint.c_str());
                    step_write(stmt_insert_ref_);
                }
            }

            // Insert edges (DEC-038 OPT-2: batched 150-row INSERT)
            {
                const int EDGE_BATCH_SIZE = 150;
                
                // Pre-filter edges (need to know final count)
                std::vector<std::tuple<int64_t, int64_t, const ExtractedEdge*>> valid_edges;
                for (const auto& edge : extraction.edges) {
                    int64_t src_id = (edge.src_index < 0) ? file_node_id : symbol_ids[edge.src_index];
                    
                    if (edge.dst_index < 0 || edge.dst_index >= static_cast<int>(symbol_ids.size()))
                        continue;  // Unresolved cross-file edge
                    if (edge.confidence < 0.3) continue;  // FR-044
                    
                    int64_t dst_id = symbol_ids[edge.dst_index];
                    valid_edges.push_back({src_id, dst_id, &edge});
                }
                
                int num_edges = static_cast<int>(valid_edges.size());
                int full_chunks = num_edges / EDGE_BATCH_SIZE;
                int remainder = num_edges % EDGE_BATCH_SIZE;
                
                // Full chunks: use batch INSERT
                if (full_chunks > 0) {
                    ensure_batch_edge_stmt();
                    for (int c = 0; c < full_chunks; ++c) {
                        sqlite3_reset(stmt_batch_insert_edge_);
                        
                        for (int e = 0; e < EDGE_BATCH_SIZE; ++e) {
                            int idx = c * EDGE_BATCH_SIZE + e;
                            auto [src_id, dst_id, edge_ptr] = valid_edges[idx];
                            int base_param = e * 5 + 1;  // 5 params per edge
                            
                            sqlite3_bind_int64(stmt_batch_insert_edge_, base_param + 0, src_id);
                            sqlite3_bind_int64(stmt_batch_insert_edge_, base_param + 1, dst_id);
                            sqlite3_bind_text(stmt_batch_insert_edge_, base_param + 2, edge_ptr->kind.c_str(), -1, SQLITE_STATIC);
                            sqlite3_bind_double(stmt_batch_insert_edge_, base_param + 3, edge_ptr->confidence);
                            if (edge_ptr->evidence.empty()) 
                                sqlite3_bind_null(stmt_batch_insert_edge_, base_param + 4);
                            else 
                                sqlite3_bind_text(stmt_batch_insert_edge_, base_param + 4, edge_ptr->evidence.c_str(), -1, SQLITE_STATIC);
                        }
                        step_write(stmt_batch_insert_edge_);
                    }
                }
                
                // Remainder: use single-row INSERT
                for (int e = 0; e < remainder; ++e) {
                    int idx = full_chunks * EDGE_BATCH_SIZE + e;
                    auto [src_id, dst_id, edge_ptr] = valid_edges[idx];
                    
                    db::bind(stmt_insert_edge_,
                             src_id,
                             dst_id,
                             edge_ptr->kind,
                             edge_ptr->confidence,
                             edge_ptr->evidence.empty() ? nullptr : edge_ptr->evidence.c_str());
                    step_write(stmt_insert_edge_);
                }
            }

            if (own_txn) conn_.exec("COMMIT");
            return true;

        } catch (...) {
            if (own_txn || in_batch_) {
                if (!sqlite3_get_autocommit(conn_.raw())) conn_.exec("ROLLBACK");
                in_batch_ = false;
                batch_count_ = 0;
            }
            last_file_id_ = -1;
            throw;
        }
    }

    // T049: Write kv metadata
    void write_metadata(const std::string& repo_root, bool complete = true) {
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
        schema::set_kv(conn_, "index_generation",
            std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
                now.time_since_epoch()).count()));
        schema::set_kv(conn_, "index_state",
            complete ? "current" : "needs_reconciliation");
        if (!complete) {
            schema::delete_kv(conn_, "last_index_time");
            schema::delete_kv(conn_, "git_head");
            schema::delete_kv(conn_, "git_branch");
            return;
        }
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
    //
    // incremental=true (targeted/watch reindex): the DB always carries a large permanent
    // backlog of unresolved refs (every external/stdlib symbol that never had a definition
    // stays resolved_node_id IS NULL forever — millions of rows). Re-resolving that whole
    // backlog on a 1-file save is what made targeted reindex take minutes. So in incremental
    // mode we compute the EXACT set of ref ids to re-resolve, entirely index-driven, and
    // never scan the NULL backlog:
    //   affected_refs = { the changed files' OWN unresolved refs (via idx_refs_file_id) }
    //               ∪ { repo-wide unresolved refs whose (kind,name) is attracted by a name
    //                   the changed files (re)DEFINE (via idx_refs_kind_name) — this
    //                   re-attaches incoming refs whose target node was just deleted → SET
    //                   NULL, and picks up previously-unresolvable same-name refs too }
    // Only those refs are re-resolved; symbols are loaded only for the affected refs' names
    // plus their bare suffixes. Existing edges are retained in both modes: persistence
    // removes obsolete outgoing edges of changed files, while resolved references in
    // unchanged files must keep their graph relationships. Full mode also repairs missing
    // primary edges from persisted resolutions, without rewriting extra-root graphs.
    std::pair<int,int> resolve_references(bool incremental = false,
                                          const std::vector<std::string>& changed_paths = {},
                                          const std::string& progress_path = "") {
        int total_resolved = 0;
        int edges_created = 0;

        // Disable FK checks — all IDs originate from the DB itself.
        conn_.exec("PRAGMA foreign_keys = OFF");

        auto is_test_or_mock_path = [](const std::string& path) {
            std::string lower = path;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return lower.find("/test") != std::string::npos ||
                   lower.find("/mock") != std::string::npos ||
                   lower.find("/fake") != std::string::npos ||
                   lower.find("/stub") != std::string::npos ||
                   lower.find("_test.") != std::string::npos ||
                   lower.find("_mock.") != std::string::npos ||
                   lower.find("_fake.") != std::string::npos ||
                   lower.find("test_") != std::string::npos;
        };

        // Build file_id → file path early so symbol/include/class lookup can prefer
        // real implementation files over test/mock/fake/stub variants.
        std::unordered_map<int64_t, std::string> fileid_to_path;
        std::unordered_map<int64_t, std::string> fileid_to_language;
        {
            sqlite3_stmt* stmt = nullptr;
            sqlite3_prepare_v2(conn_.raw(),
                "SELECT id, path, language FROM files", -1, &stmt, nullptr);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const unsigned char* path_raw = sqlite3_column_text(stmt, 1);
                if (!path_raw) continue;
                fileid_to_path[sqlite3_column_int64(stmt, 0)] =
                    reinterpret_cast<const char*>(path_raw);
                const auto* language = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                fileid_to_language[sqlite3_column_int64(stmt, 0)] = language ? language : "";
            }
            sqlite3_finalize(stmt);
        }

        // --- Step 1: Build in-memory lookup: name → candidate symbols ---
        // Also builds class_map in the same scan (merges former Step 4).
        struct SymbolEntry {
            int64_t id;
            int64_t file_id;
            bool is_definition;
            bool is_test_or_mock;
            call_binding::Target target;
        };
        std::unordered_map<std::string, std::vector<SymbolEntry>> symbol_map;
        symbol_map.reserve(2500000);

        struct ClassEntry {
            int64_t id;
            bool is_def;
            bool is_test_or_mock;
        };
        std::unordered_map<std::string, ClassEntry> class_map;
        // All type-like kinds (class/struct/interface/enum/union/typedef/type_alias/type),
        // used to resolve 'type_ref' refs so type usages become queryable references.
        std::unordered_map<std::string, ClassEntry> type_map;
        std::unordered_set<std::string> class_names;
        std::unordered_set<std::string> namespace_names;

        // Incremental (targeted) mode: the unresolved-ref set spans the WHOLE repo (every
        // external/stdlib symbol that never had a definition stays resolved_node_id IS NULL
        // forever — millions of rows). Re-resolving that whole backlog on a 1-file save is
        // what made targeted reindex take minutes. So we compute the EXACT set of ref ids to
        // re-resolve, index-driven, and never touch the NULL backlog:
        //   __ct_changed  : the changed files' repo-relative paths.
        //   __ct_defnames : names the changed files define (definitions).
        //   __ct_kinds    : the ref kinds that produce edges (call/include/inherit/type_ref).
        //   __ct_affected : the ref ids to re-resolve =
        //       (a) the changed files' own unresolved refs (via idx_refs_file_id), ∪
        //       (c) repo-wide unresolved refs whose (kind,name) is attracted by a name the
        //           changed files (re)define (via idx_refs_kind_name) — this re-attaches
        //           incoming refs whose target was just deleted & SET NULL, and picks up
        //           previously-unresolvable same-name refs.
        //   __ct_want     : names of the affected refs ∪ bare suffixes — scopes the
        //                   symbol_map load (find_cross_file_symbols does a bare-name lookup).
        // Every population query is pinned with CROSS JOIN so the tiny temp table drives and
        // refs is reached by idx_refs_file_id / idx_refs_kind_name / rowid — NEVER via
        // idx_refs_resolved (which would drive from the whole NULL backlog). Verified with
        // EXPLAIN QUERY PLAN against the DsMainDev index (no ANALYZE stats exist).
        if (incremental) {
            conn_.exec("DROP TABLE IF EXISTS temp.__ct_changed");
            conn_.exec("DROP TABLE IF EXISTS temp.__ct_defnames");
            conn_.exec("DROP TABLE IF EXISTS temp.__ct_kinds");
            conn_.exec("DROP TABLE IF EXISTS temp.__ct_affected");
            conn_.exec("DROP TABLE IF EXISTS temp.__ct_want");
            conn_.exec("CREATE TEMP TABLE __ct_changed(path TEXT PRIMARY KEY)");
            conn_.exec("CREATE TEMP TABLE __ct_defnames(name TEXT PRIMARY KEY)");
            conn_.exec("CREATE TEMP TABLE __ct_kinds(kind TEXT PRIMARY KEY)");
            conn_.exec("CREATE TEMP TABLE __ct_affected(id INTEGER PRIMARY KEY)");
            conn_.exec("CREATE TEMP TABLE __ct_want(name TEXT PRIMARY KEY)");

            conn_.exec("BEGIN TRANSACTION");
            // 1. Changed file paths (tiny — one row per re-persisted file).
            {
                sqlite3_stmt* ins = nullptr;
                sqlite3_prepare_v2(conn_.raw(),
                    "INSERT OR IGNORE INTO temp.__ct_changed(path) VALUES(?)", -1, &ins, nullptr);
                for (const auto& p : changed_paths) {
                    sqlite3_reset(ins);
                    sqlite3_bind_text(ins, 1, p.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_step(ins);
                }
                sqlite3_finalize(ins);
            }
            // The ref kinds that produce resolvable edges (used by the (c) attraction probe).
            conn_.exec(
                "INSERT OR IGNORE INTO temp.__ct_kinds(kind) "
                "VALUES('call'),('include'),('inherit'),('type_ref')");
            // 2. Names the changed files define. CROSS JOIN pins nodes last so it is probed
            //    via idx_nodes_file_id from the tiny file set — never a full nodes scan.
            conn_.exec(
                "INSERT OR IGNORE INTO temp.__ct_defnames(name) "
                "SELECT DISTINCT n.name FROM temp.__ct_changed c "
                "JOIN files f ON f.path = c.path "
                "CROSS JOIN nodes n "
                "WHERE n.file_id = f.id AND n.node_type = 'symbol' "
                "AND n.is_definition = 1 AND n.name IS NOT NULL");
            // 3a. (a) The changed files' OWN unresolved refs. CROSS JOIN pins __ct_changed
            //     first so refs is reached via idx_refs_file_id (NOT idx_refs_resolved).
            conn_.exec(
                "INSERT OR IGNORE INTO temp.__ct_affected(id) "
                "SELECT r.id FROM temp.__ct_changed c "
                "CROSS JOIN files f ON f.path = c.path "
                "CROSS JOIN refs r ON r.file_id = f.id "
                "WHERE r.resolved_node_id IS NULL");
            // 3b. (c) Repo-wide unresolved refs attracted by a name the changed files
            //     (re)define. idx_refs_kind_name makes each (kind,name) probe index-driven,
            //     so this is bounded by (def-names × 4 kinds), never the NULL backlog.
            conn_.exec(
                "INSERT OR IGNORE INTO temp.__ct_affected(id) "
                "SELECT r.id FROM temp.__ct_defnames d "
                "CROSS JOIN temp.__ct_kinds k "
                "CROSS JOIN refs r ON r.kind = k.kind AND r.name = d.name "
                "WHERE r.resolved_node_id IS NULL");
            // 4. want = names of the affected refs ∪ their bare suffixes. Collect names first
            //    (CROSS JOIN pins __ct_affected → refs by rowid), then add suffixes.
            std::vector<std::string> affected_names;
            {
                sqlite3_stmt* qn = nullptr;
                sqlite3_prepare_v2(conn_.raw(),
                    "SELECT DISTINCT r.name FROM temp.__ct_affected a "
                    "CROSS JOIN refs r ON r.id = a.id WHERE r.name IS NOT NULL",
                    -1, &qn, nullptr);
                while (qn && sqlite3_step(qn) == SQLITE_ROW) {
                    const char* nm = reinterpret_cast<const char*>(sqlite3_column_text(qn, 0));
                    if (nm) affected_names.emplace_back(nm);
                }
                sqlite3_finalize(qn);
            }
            {
                sqlite3_stmt* insw = nullptr;
                sqlite3_prepare_v2(conn_.raw(),
                    "INSERT OR IGNORE INTO temp.__ct_want(name) VALUES(?)", -1, &insw, nullptr);
                auto want_add = [&](const std::string& s) {
                    sqlite3_reset(insw);
                    sqlite3_bind_text(insw, 1, s.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_step(insw);
                };
                for (const auto& full : affected_names) {
                    want_add(full);
                    want_add(call_binding::bare_name(full));
                }
                sqlite3_finalize(insw);
            }
            conn_.exec("COMMIT");
        }

        {
            sqlite3_stmt* stmt = nullptr;
            sqlite3_prepare_v2(conn_.raw(),
                incremental
                    // Scoped to names referenced/defined by the changed files. CROSS JOIN
                    // pins the tiny temp table as the outer driver (no ANALYZE stats exist,
                    // so the planner would otherwise scan every symbol node).
                    ? "SELECT n.id, n.name, n.file_id, n.is_definition, n.kind, "
                      "COALESCE(NULLIF(n.qualname,''),n.name), COALESCE(n.signature,'') "
                      "FROM temp.__ct_want w CROSS JOIN nodes n INDEXED BY idx_nodes_name_type "
                      "ON n.name = w.name AND n.node_type = 'symbol'"
                    : "SELECT n.id, n.name, n.file_id, n.is_definition, n.kind, "
                      "COALESCE(NULLIF(n.qualname,''),n.name), COALESCE(n.signature,'') "
                      "FROM files f CROSS JOIN nodes n INDEXED BY idx_nodes_file_id "
                      "ON n.file_id = f.id WHERE n.node_type = 'symbol'",
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
                auto path_it = fileid_to_path.find(file_id);
                const bool is_test_or_mock = path_it != fileid_to_path.end() &&
                    is_test_or_mock_path(path_it->second);

                const auto* qualname = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
                const auto* signature = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
                if (kind_raw && (std::string_view(kind_raw) == "function" ||
                    std::string_view(kind_raw) == "method" || std::string_view(kind_raw) == "constructor_fn")) {
                    symbol_map[name].push_back({id, file_id, is_def, is_test_or_mock,
                        call_binding::target(kind_raw, name, qualname ? qualname : name,
                            signature ? signature : "", fileid_to_language[file_id])});
                }

                // Build class_map inline (replaces former Step 4 scan)
                if (kind_raw) {
                    std::string_view kind_sv(kind_raw);
                    auto prefer = [&](std::unordered_map<std::string, ClassEntry>& m) {
                        auto cit = m.find(name);
                        if (cit == m.end()) {
                            m[name] = {id, is_def, is_test_or_mock};
                        } else if ((cit->second.is_test_or_mock && !is_test_or_mock) ||
                                  (cit->second.is_test_or_mock == is_test_or_mock &&
                                   ((!cit->second.is_def && is_def) ||
                                    (cit->second.is_def == is_def && id < cit->second.id)))) {
                            cit->second = {id, is_def, is_test_or_mock};
                        }
                    };
                    if (kind_sv == "class" || kind_sv == "struct" || kind_sv == "interface") {
                        prefer(class_map);
                        class_names.insert(call_binding::without_templates(qualname ? qualname : name));
                    }
                    if (kind_sv == "namespace")
                        namespace_names.insert(call_binding::without_templates(qualname ? qualname : name));
                    if (kind_sv == "class" || kind_sv == "struct" || kind_sv == "interface" ||
                        kind_sv == "enum" || kind_sv == "union" || kind_sv == "typedef" ||
                        kind_sv == "type_alias" || kind_sv == "type") {
                        prefer(type_map);
                    }
                }

                ++loaded;
                if (!progress_path.empty() && loaded % 200000 == 0) {
                    std::ofstream pf(progress_path, std::ios::trunc);
                    pf << "loading_symbols: " << loaded << '\n';
                    pf.flush();
                }
            }
            sqlite3_finalize(stmt);

            for (auto& [_, entries] : symbol_map) {
                std::sort(entries.begin(), entries.end(),
                    [](const SymbolEntry& a, const SymbolEntry& b) {
                        if (a.is_test_or_mock != b.is_test_or_mock) {
                            return !a.is_test_or_mock;
                        }
                        if (a.is_definition != b.is_definition) {
                            return a.is_definition;
                        }
                        return a.id < b.id;
                    });
            }
            if (incremental) {
                std::unordered_set<std::string> owners;
                for (const auto& [_, entries] : symbol_map) {
                    for (const auto& entry : entries) {
                        auto owner = call_binding::owner_scope(entry.target.qualname, entry.target.name);
                        if (!owner.empty()) owners.insert(owner);
                    }
                }
                auto scopes = call_binding::load_scopes(conn_, owners);
                class_names.insert(scopes.classes.begin(), scopes.classes.end());
                namespace_names.insert(scopes.namespaces.begin(), scopes.namespaces.end());
            }
            const bool color_output = stderr_is_tty();
            std::cerr << "  Loaded " << stderr_cyan(format_with_commas(loaded), color_output)
                      << " symbols into lookup map ("
                      << stderr_dim(format_with_commas(static_cast<int64_t>(symbol_map.size())) + " unique names",
                                     color_output)
                      << ")\n";
        }

        // --- Step 2: File nodes have NULL file_id. Reach their stable keys from files,
        // including root-prefixed keys and the older file:<path> key format.
        std::unordered_map<std::string, int64_t> file_node_map;
        {
            sqlite3_stmt* stmt = nullptr;
            sqlite3_prepare_v2(conn_.raw(),
                db::owned_file_nodes_sql,
                -1, &stmt, nullptr);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                int64_t id = sqlite3_column_int64(stmt, 0);
                const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                if (name) file_node_map[name] = id;
            }
            sqlite3_finalize(stmt);
        }

        if (!incremental) {
            sqlite3_stmt* resolved_refs = nullptr;
            prepare_cached(
                "SELECT COALESCE(r.containing_node_id, 0), r.resolved_node_id, r.kind, f.path "
                "FROM files f INDEXED BY idx_files_root "
                "CROSS JOIN refs r INDEXED BY idx_refs_file_id ON r.file_id = f.id "
                "CROSS JOIN nodes dst ON dst.id = r.resolved_node_id "
                "WHERE f.root_id IS NULL AND r.resolved_node_id IS NOT NULL "
                "AND r.kind IN ('call', 'include', 'inherit', 'type_ref')",
                &resolved_refs);
            sqlite3_stmt* repair_edge = nullptr;
            prepare_cached(
                "INSERT INTO edges(src_id, dst_id, kind, confidence, evidence) "
                "SELECT ?, ?, ?, 0.7, 'name-match' "
                "WHERE EXISTS (SELECT 1 FROM nodes WHERE id = ?)"
                "AND NOT EXISTS (SELECT 1 FROM edges e INDEXED BY idx_edges_src "
                "WHERE e.src_id = ? AND e.kind = ? AND e.dst_id = ?)",
                &repair_edge);
            conn_.exec("BEGIN IMMEDIATE");
            int repaired = 0;
            int inspected = 0;
            while (sqlite3_step(resolved_refs) == SQLITE_ROW) {
                int64_t source_id = sqlite3_column_int64(resolved_refs, 0);
                int64_t target_id = sqlite3_column_int64(resolved_refs, 1);
                const char* ref_kind =
                    reinterpret_cast<const char*>(sqlite3_column_text(resolved_refs, 2));
                const char* path =
                    reinterpret_cast<const char*>(sqlite3_column_text(resolved_refs, 3));
                if (source_id == 0 && path) {
                    auto file_node = file_node_map.find(path);
                    if (file_node != file_node_map.end()) source_id = file_node->second;
                }
                if (source_id == 0 || !ref_kind) continue;
                const char* edge_kind =
                    std::string_view(ref_kind) == "call" ? "calls" :
                    std::string_view(ref_kind) == "include" ? "includes" :
                    std::string_view(ref_kind) == "inherit" ? "inherits" : "references";
                db::bind(repair_edge, source_id, target_id, edge_kind,
                         source_id, source_id, edge_kind, target_id);
                step_write(repair_edge);
                repaired += sqlite3_changes(conn_.raw());
                if (++inspected % 100000 == 0) {
                    conn_.exec("COMMIT");
                    conn_.exec("BEGIN IMMEDIATE");
                    if (!progress_path.empty()) {
                        std::ofstream pf(progress_path, std::ios::trunc);
                        pf << "repairing_resolved_edges: " << inspected
                           << " inspected, " << repaired << " restored\n";
                    }
                }
            }
            conn_.exec("COMMIT");
            sqlite3_finalize(resolved_refs);
            sqlite3_finalize(repair_edge);
            edges_created += repaired;
            std::cerr << "  Restored " << repaired
                      << " missing primary edges from persisted resolutions\n";
        }

        // --- Step 3: Build include lookup: filename → file node id ---
        // For includes like "foo.h", we match the file node whose path ends with "/foo.h"
        struct IncludeEntry {
            int64_t node_id;
            bool is_test_or_mock;
        };
        std::unordered_map<std::string, IncludeEntry> include_map;
        for (const auto& [path, node_id] : file_node_map) {
            auto slash_pos = path.rfind('/');
            std::string basename = (slash_pos != std::string::npos) ? path.substr(slash_pos + 1) : path;
            const bool is_test_or_mock = is_test_or_mock_path(path);
            auto it = include_map.find(basename);
            if (it == include_map.end()) {
                include_map[basename] = {node_id, is_test_or_mock};
            } else if (it->second.is_test_or_mock && !is_test_or_mock) {
                it->second = {node_id, is_test_or_mock};
            }
        }

        // --- Step 4: (merged into Step 1 above) ---

        // --- Step 5: Read all unresolved refs, resolve in memory, batch-update ---
        const bool color_output = stderr_is_tty();
        std::cerr << "  " << stderr_bold("Resolving refs in memory...", color_output) << "\n";

        // Prepare the UPDATE statement (by rowid — fastest possible)
        sqlite3_stmt* update_stmt = nullptr;
        sqlite3_prepare_v2(conn_.raw(),
            "UPDATE refs SET resolved_node_id = ? WHERE id = ?",
            -1, &update_stmt, nullptr);

        // Scan all unresolved refs and resolve, collecting edge tuples in memory
        sqlite3_stmt* ref_stmt = nullptr;
        sqlite3_prepare_v2(conn_.raw(),
            incremental
                // Re-resolve ONLY the pre-computed affected ref ids. CROSS JOIN pins the tiny
                // __ct_affected as the driver so refs is reached by rowid — the repo-wide NULL
                // backlog is never scanned. (Verified index-driven via EXPLAIN QUERY PLAN.)
                ? "SELECT r.id, r.file_id, r.kind, r.name, r.containing_node_id, r.receiver_type_hint, "
                  "COALESCE(r.arg_count,-1), COALESCE(cn.qualname,''), COALESCE(f.language,'') "
                  "FROM temp.__ct_affected a CROSS JOIN refs r ON r.id = a.id "
                  "LEFT JOIN nodes cn ON cn.id=r.containing_node_id "
                  "LEFT JOIN files f ON f.id=r.file_id "
                  "WHERE r.resolved_node_id IS NULL"
                : "SELECT r.id, r.file_id, r.kind, r.name, r.containing_node_id, r.receiver_type_hint, "
                  "COALESCE(r.arg_count,-1), COALESCE(cn.qualname,''), COALESCE(f.language,'') "
                  "FROM files f INDEXED BY idx_files_root "
                  "CROSS JOIN refs r INDEXED BY idx_refs_file_id ON r.file_id = f.id "
                  "LEFT JOIN nodes cn ON cn.id=r.containing_node_id "
                  "WHERE f.root_id IS NULL AND r.resolved_node_id IS NULL",
            -1, &ref_stmt, nullptr);

        // Edge tuples collected during resolution — avoids expensive SQL join in Step 6
        struct EdgeTuple {
            int64_t src_id;   // containing symbol id or file node id fallback
            int64_t dst_id;   // resolved target node id
            const char* kind; // edge kind (static string literal)
            double confidence = 0.7;
            const char* evidence = "name-match";
        };
        std::vector<EdgeTuple> edge_tuples;
        edge_tuples.reserve(1000000);
        struct EdgeKey {
            int64_t src_id;
            int64_t dst_id;
            uint8_t kind_id; // 1: calls, 2: includes, 3: inherits, 4: references

            bool operator==(const EdgeKey& o) const noexcept {
                return src_id == o.src_id && dst_id == o.dst_id && kind_id == o.kind_id;
            }
        };

        struct EdgeKeyHash {
            size_t operator()(const EdgeKey& k) const noexcept {
                uint64_t h = static_cast<uint64_t>(k.src_id) ^ (static_cast<uint64_t>(k.dst_id) * 0x9e3779b97f4a7c15ULL);
                h ^= (static_cast<uint64_t>(k.kind_id) << 56);
                h ^= h >> 30;
                h *= 0xbf58476d1ce4e5b9ULL;
                h ^= h >> 27;
                h *= 0x94d049bb133111ebULL;
                h ^= h >> 31;
                return static_cast<size_t>(h);
            }
        };

        auto edge_kind_to_id = [](std::string_view k) noexcept -> uint8_t {
            if (k == "calls") return 1;
            if (k == "includes") return 2;
            if (k == "inherits") return 3;
            if (k == "references") return 4;
            return 0;
        };

        std::unordered_set<EdgeKey, EdgeKeyHash> seen_edge_keys;
        seen_edge_keys.reserve(1000000);

        // Incremental mode skips the global name-match edge wipe below. The cascade delete
        // already removed every edge touching the re-persisted files. Some name-match edges
        // from the source nodes we're about to re-resolve SURVIVED (e.g. call fan-out edges
        // whose target is in an unchanged file). Pre-seed the dedup set with them so we do
        // not insert duplicates when those refs are re-resolved.
        if (incremental) {
            sqlite3_stmt* pe = nullptr;
            sqlite3_prepare_v2(conn_.raw(),
                "SELECT e.src_id, e.dst_id, e.kind FROM edges e "
                "WHERE e.src_id IN ("
                "  SELECT COALESCE(r.containing_node_id, fn.id) "
                "  FROM temp.__ct_affected a CROSS JOIN refs r ON r.id = a.id "
                "  LEFT JOIN files f ON f.id = r.file_id "
                "  LEFT JOIN nodes fn ON fn.node_type = 'file' AND fn.name = f.path "
                "  WHERE r.resolved_node_id IS NULL)",
                -1, &pe, nullptr);
            while (pe && sqlite3_step(pe) == SQLITE_ROW) {
                int64_t s = sqlite3_column_int64(pe, 0);
                int64_t d = sqlite3_column_int64(pe, 1);
                const char* k = reinterpret_cast<const char*>(sqlite3_column_text(pe, 2));
                seen_edge_keys.insert(EdgeKey{s, d, edge_kind_to_id(k ? k : "")});
            }
            sqlite3_finalize(pe);
        }

        conn_.exec("BEGIN TRANSACTION");
        int batch = 0;
        int call_resolved = 0, call_ambiguous = 0, include_resolved = 0, inherit_resolved = 0, type_ref_resolved = 0;
        std::string name;
        name.reserve(256);  // reuse buffer across iterations
        auto find_cross_file_symbols = [&](
            const std::string& ref_name,
            const std::string& receiver_type_hint,
            int arguments,
            const std::string& caller_language,
            const std::string& caller_qualname
        ) -> std::vector<std::pair<int64_t, double>> {
            std::vector<std::pair<int64_t, double>> results;

            auto emit_candidates = [&](const std::vector<SymbolEntry>& entries) {
                for (const auto& sym : entries) {
                    if (!call_binding::compatible(sym.target, ref_name, receiver_type_hint,
                        arguments, caller_language, class_names, caller_qualname, namespace_names)) continue;

                    double conf = sym.is_definition ? 0.75 : 0.65;
                    if (sym.is_test_or_mock) conf -= 0.15;

                    if (!receiver_type_hint.empty()) {
                        auto owner = call_binding::owner_scope(sym.target.qualname, sym.target.name);
                        if (call_binding::same_owner(owner, call_binding::type_name(receiver_type_hint)))
                            conf = std::min(conf + 0.20, 0.95);
                    }

                    if (conf >= 0.3) {
                        results.push_back({sym.id, conf});
                    }
                }
            };

            auto it = symbol_map.find(ref_name);
            if (it != symbol_map.end()) {
                emit_candidates(it->second);
            }

            auto bare = call_binding::bare_name(ref_name);
            if (bare != ref_name) {
                auto it2 = symbol_map.find(bare);
                if (it2 != symbol_map.end() && it2 != it) {
                    emit_candidates(it2->second);
                }
            }

            std::unordered_map<int64_t, double> deduped;
            for (const auto& [nid, conf] : results) {
                auto dup_it = deduped.find(nid);
                if (dup_it == deduped.end() || conf > dup_it->second) {
                    deduped[nid] = conf;
                }
            }

            results.clear();
            results.reserve(deduped.size());
            for (const auto& [nid, conf] : deduped) {
                results.push_back({nid, conf});
            }

            std::sort(results.begin(), results.end(),
                [](const auto& a, const auto& b) {
                    return a.second != b.second ? a.second > b.second : a.first < b.first;
                });

            return results;
        };

        while (sqlite3_step(ref_stmt) == SQLITE_ROW) {
            int64_t ref_id = sqlite3_column_int64(ref_stmt, 0);
            int64_t ref_file_id = sqlite3_column_int64(ref_stmt, 1);
            const char* kind_raw = reinterpret_cast<const char*>(sqlite3_column_text(ref_stmt, 2));
            const char* name_raw = reinterpret_cast<const char*>(sqlite3_column_text(ref_stmt, 3));
            int64_t containing_node_id = sqlite3_column_type(ref_stmt, 4) != SQLITE_NULL
                ? sqlite3_column_int64(ref_stmt, 4)
                : -1;
            const char* rth_raw = sqlite3_column_type(ref_stmt, 5) != SQLITE_NULL
                ? reinterpret_cast<const char*>(sqlite3_column_text(ref_stmt, 5))
                : nullptr;
            if (!kind_raw || !name_raw) continue;

            std::string_view kind(kind_raw);
            name.assign(name_raw);
            std::string receiver_type_hint = rth_raw ? rth_raw : "";
            int arguments = sqlite3_column_int(ref_stmt, 6);
            const auto* caller_qn = reinterpret_cast<const char*>(sqlite3_column_text(ref_stmt, 7));
            const auto* caller_lang = reinterpret_cast<const char*>(sqlite3_column_text(ref_stmt, 8));
            int64_t resolved_id = 0;
            bool resolved = false;
            const char* edge_kind = nullptr;

            if (kind == "call") {
                auto candidates = find_cross_file_symbols(name, receiver_type_hint,
                    arguments, caller_lang ? caller_lang : "", caller_qn ? caller_qn : "");
                if (!candidates.empty()) {
                    bool unique = candidates.size() == 1;
                    edge_kind = "calls";
                    if (unique) {
                        resolved_id = candidates[0].first;
                        ++call_resolved;
                        db::bind(update_stmt, resolved_id, ref_id);
                        step_write(update_stmt);
                        ++total_resolved;
                    } else ++call_ambiguous;

                    auto path_it = fileid_to_path.find(ref_file_id);
                    if (path_it != fileid_to_path.end()) {
                        auto fn_it = file_node_map.find(path_it->second);
                        if (fn_it != file_node_map.end()) {
                            constexpr size_t MAX_EDGES_PER_REF = 10;
                            int64_t src_id = (containing_node_id > 0) ? containing_node_id : fn_it->second;
                            for (size_t i = 0; i < std::min(candidates.size(), MAX_EDGES_PER_REF); ++i) {
                                const auto& [cand_id, cand_conf] = candidates[i];
                                if (seen_edge_keys.insert(EdgeKey{src_id, cand_id, 1}).second) {
                                    edge_tuples.push_back({src_id, cand_id, edge_kind,
                                        unique ? cand_conf : std::min(cand_conf, 0.60),
                                        unique ? "name-match" : "call-candidate"});
                                }
                            }
                        }
                    }

                    if (++batch >= 100000) {
                        conn_.exec("COMMIT");
                        conn_.exec("BEGIN TRANSACTION");
                        batch = 0;
                    }
                    continue;
                }
            } else if (kind == "include") {
                auto it = include_map.find(name);
                if (it != include_map.end()) {
                    resolved_id = it->second.node_id;
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
            } else if (kind == "type_ref") {
                auto it = type_map.find(name);
                if (it != type_map.end()) {
                    resolved_id = it->second.id;
                    resolved = true;
                    edge_kind = "references";
                    ++type_ref_resolved;
                }
            }

            if (resolved) {
                db::bind(update_stmt, resolved_id, ref_id);
                sqlite3_step(update_stmt);
                ++total_resolved;

                // Edge tuple: containing symbol → resolved target, or file-node fallback
                auto path_it = fileid_to_path.find(ref_file_id);
                if (path_it != fileid_to_path.end()) {
                    auto fn_it = file_node_map.find(path_it->second);
                    if (fn_it != file_node_map.end()) {
                        int64_t src_id = (containing_node_id > 0) ? containing_node_id : fn_it->second;
                        uint8_t kid = edge_kind_to_id(edge_kind);
                        if (seen_edge_keys.insert(EdgeKey{src_id, resolved_id, kid}).second) {
                            edge_tuples.push_back({src_id, resolved_id, edge_kind});
                        }
                    }
                }

                if (++batch >= 100000) {
                    conn_.exec("COMMIT");
                    conn_.exec("BEGIN TRANSACTION");
                    batch = 0;
                    if (!progress_path.empty()) {
                        std::ofstream pf(progress_path, std::ios::trunc);
                        pf << "resolving_refs: " << total_resolved << '\n';
                        pf.flush();
                    }
                }
            }
        }
        conn_.exec("COMMIT");

        sqlite3_finalize(ref_stmt);
        sqlite3_finalize(update_stmt);

        std::cerr << "  Resolved "
                  << stderr_cyan(format_with_commas(call_resolved), color_output) << " call, "
                  << stderr_cyan(format_with_commas(include_resolved), color_output) << " include, "
                  << stderr_cyan(format_with_commas(inherit_resolved), color_output) << " inherit, "
                  << stderr_cyan(format_with_commas(type_ref_resolved), color_output) << " type refs\n";
        if (call_ambiguous > 0)
            std::cerr << "  Kept " << call_ambiguous << " ambiguous call refs unresolved (candidate edges only)\n";

        // --- Step 6: Add missing edges without invalidating unchanged primary or
        // workspace relationships. Keep source/destination indexes live for readers
        // and for bounded duplicate checks.

        std::cerr << "  Inserting " << stderr_cyan(format_with_commas(static_cast<int64_t>(edge_tuples.size())), color_output)
                  << " edges...\n";
        conn_.exec("BEGIN TRANSACTION");

        // Batch edge INSERT using 400-row chunks (1,600 SQL parameters, well within SQLite limit).
        const int RESOLVE_EDGE_BATCH = 400;
        const int PARAMS_PER_EDGE = 5;

        // Prepare batch statement: 400 rows × "(?,?,?,?, 'name-match')"
        std::string batch_sql =
            "WITH candidates(src_id,dst_id,kind,confidence,evidence) AS (VALUES ";
        for (int i = 0; i < RESOLVE_EDGE_BATCH; ++i) {
            if (i > 0) batch_sql += ",";
            batch_sql += "(?,?,?,?,?)";
        }
        batch_sql +=
            ") INSERT INTO edges(src_id,dst_id,kind,confidence,evidence) "
            "SELECT c.src_id,c.dst_id,c.kind,c.confidence,c.evidence FROM candidates c "
            "WHERE NOT EXISTS (SELECT 1 FROM edges e INDEXED BY idx_edges_src "
            "WHERE e.src_id=c.src_id AND e.kind=c.kind AND e.dst_id=c.dst_id)";
        sqlite3_stmt* batch_edge_stmt = nullptr;
        prepare_cached(batch_sql.c_str(), &batch_edge_stmt);

        // Single-row fallback for remainder
        sqlite3_stmt* single_edge_stmt = nullptr;
        prepare_cached(
            "INSERT INTO edges(src_id, dst_id, kind, confidence, evidence) "
            "SELECT ?, ?, ?, ?, ? "
            "WHERE NOT EXISTS (SELECT 1 FROM edges e INDEXED BY idx_edges_src "
            "WHERE e.src_id=? AND e.kind=? AND e.dst_id=?)",
            &single_edge_stmt);

        int total_edges = static_cast<int>(edge_tuples.size());
        int full_chunks = total_edges / RESOLVE_EDGE_BATCH;
        int remainder = total_edges % RESOLVE_EDGE_BATCH;
        int commit_counter = 0;

        for (int c = 0; c < full_chunks; ++c) {
            sqlite3_reset(batch_edge_stmt);
            for (int e = 0; e < RESOLVE_EDGE_BATCH; ++e) {
                const auto& t = edge_tuples[c * RESOLVE_EDGE_BATCH + e];
                int base = e * PARAMS_PER_EDGE + 1;
                sqlite3_bind_int64(batch_edge_stmt, base + 0, t.src_id);
                sqlite3_bind_int64(batch_edge_stmt, base + 1, t.dst_id);
                sqlite3_bind_text(batch_edge_stmt, base + 2, t.kind, -1, SQLITE_STATIC);
                sqlite3_bind_double(batch_edge_stmt, base + 3, t.confidence);
                sqlite3_bind_text(batch_edge_stmt, base + 4, t.evidence, -1, SQLITE_STATIC);
            }
            step_write(batch_edge_stmt);
            edges_created += sqlite3_changes(conn_.raw());
            commit_counter += RESOLVE_EDGE_BATCH;
            if (commit_counter >= 100000) {
                conn_.exec("COMMIT");
                conn_.exec("BEGIN TRANSACTION");
                commit_counter = 0;
                if (!progress_path.empty()) {
                    std::ofstream pf(progress_path, std::ios::trunc);
                    pf << "resolving_edges: " << edges_created << '\n';
                    pf.flush();
                }
            }
        }

        for (int e = 0; e < remainder; ++e) {
            const auto& t = edge_tuples[full_chunks * RESOLVE_EDGE_BATCH + e];
            db::bind(single_edge_stmt, t.src_id, t.dst_id, t.kind, t.confidence, t.evidence,
                     t.src_id, t.kind, t.dst_id);
            step_write(single_edge_stmt);
            edges_created += sqlite3_changes(conn_.raw());
        }

        conn_.exec("COMMIT");
        sqlite3_finalize(batch_edge_stmt);
        sqlite3_finalize(single_edge_stmt);

        std::cerr << "  Created " << stderr_cyan(format_with_commas(edges_created), color_output)
                  << " edges\n";

        if (incremental) {
            conn_.exec("DROP TABLE IF EXISTS temp.__ct_changed");
            conn_.exec("DROP TABLE IF EXISTS temp.__ct_defnames");
            conn_.exec("DROP TABLE IF EXISTS temp.__ct_kinds");
            conn_.exec("DROP TABLE IF EXISTS temp.__ct_affected");
            conn_.exec("DROP TABLE IF EXISTS temp.__ct_want");
        }

        conn_.exec("PRAGMA foreign_keys = ON");
        return {total_resolved, edges_created};
    }

private:
    Connection& conn_;
    bool in_batch_ = false;
    int batch_count_ = 0;

    int64_t ensure_file_node(const std::string& path) {
        auto key = make_file_stable_key(path);
        db::bind(stmt_find_file_node_, key);
        int rc = sqlite3_step(stmt_find_file_node_);
        int64_t id = rc == SQLITE_ROW ? sqlite3_column_int64(stmt_find_file_node_, 0) : 0;
        sqlite3_reset(stmt_find_file_node_);
        if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
            throw SqliteError(rc,
                "Find file node failed: " + std::string(sqlite3_errmsg(conn_.raw())));
        }
        if (id > 0) return id;
        db::bind(stmt_insert_file_node_, path, key);
        step_write(stmt_insert_file_node_);
        return sqlite3_last_insert_rowid(conn_.raw());
    }

    void step_write(sqlite3_stmt* stmt) {
        const int rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            const std::string message = sqlite3_errmsg(conn_.raw());
            sqlite3_reset(stmt);
            throw SqliteError(rc, "Persist statement failed: " + message);
        }
    }

    void prepare_cached(const char* sql, sqlite3_stmt** stmt) {
        const int rc = sqlite3_prepare_v2(conn_.raw(), sql, -1, stmt, nullptr);
        if (rc != SQLITE_OK)
            throw SqliteError(rc, "Persist prepare failed: " + std::string(sqlite3_errmsg(conn_.raw())));
    }

    // Cached prepared statements for persist_file() — prepared once, reused via reset/clear_bindings
    sqlite3_stmt* stmt_find_file_ = nullptr;
    sqlite3_stmt* stmt_find_file_node_ = nullptr;
    sqlite3_stmt* stmt_delete_file_ = nullptr;
    sqlite3_stmt* stmt_delete_file_node_ = nullptr;
    sqlite3_stmt* stmt_insert_file_ = nullptr;
    sqlite3_stmt* stmt_update_file_ = nullptr;
    sqlite3_stmt* stmt_insert_file_node_ = nullptr;
    sqlite3_stmt* stmt_insert_symbol_ = nullptr;
    sqlite3_stmt* stmt_update_symbol_ = nullptr;
    sqlite3_stmt* stmt_delete_symbol_ = nullptr;
    sqlite3_stmt* stmt_delete_refs_ = nullptr;
    sqlite3_stmt* stmt_delete_file_edges_ = nullptr;
    sqlite3_stmt* stmt_insert_ref_ = nullptr;
    sqlite3_stmt* stmt_insert_edge_ = nullptr;
    
    // DEC-038 OPT-2: Batch INSERT statements (lazy-prepared)
    sqlite3_stmt* stmt_batch_insert_ref_ = nullptr;   // 80-row batch
    sqlite3_stmt* stmt_batch_insert_edge_ = nullptr;  // 150-row batch
    // DEC-039 OPT-1: Batch symbol INSERT (lazy-prepared)
    sqlite3_stmt* stmt_batch_insert_symbol_ = nullptr; // 20-row batch
    
    bool stmts_cached_ = false;
    bool cold_index_ = false;   // R4: skip DELETE on cold index (empty files table)
    int64_t last_file_id_ = -1;  // file_id from the last persist_file() call

    void ensure_stmts_cached() {
        if (stmts_cached_) return;

        prepare_cached(
            "SELECT id FROM files WHERE path = ? AND root_id IS NULL", &stmt_find_file_);

        prepare_cached(
            "SELECT id FROM nodes WHERE node_type = 'file' AND stable_key = ?", &stmt_find_file_node_);

        prepare_cached(
            "DELETE FROM files WHERE path = ? AND root_id IS NULL", &stmt_delete_file_);

        prepare_cached(
            "DELETE FROM nodes WHERE node_type = 'file' AND stable_key = ?",
            &stmt_delete_file_node_);

        prepare_cached(
            "INSERT INTO files(path, language, size_bytes, mtime_ns, content_hash, parse_status, parse_error) "
            "VALUES(?, ?, ?, ?, ?, ?, ?)", &stmt_insert_file_);

        prepare_cached(
            "UPDATE files SET language = ?, size_bytes = ?, mtime_ns = ?, content_hash = ?, parse_status = ?, parse_error = ? WHERE id = ?",
            &stmt_update_file_);

        prepare_cached(
            "INSERT INTO nodes(node_type, file_id, kind, name, stable_key) "
            "VALUES('file', NULL, 'file', ?, ?)", &stmt_insert_file_node_);

        prepare_cached(
            "INSERT INTO nodes(node_type, file_id, kind, name, qualname, signature, fingerprint, "
            "start_line, start_col, end_line, end_col, is_definition, visibility, doc, stable_key) "
            "VALUES('symbol', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", &stmt_insert_symbol_);

        prepare_cached(
            "UPDATE nodes SET kind = ?, name = ?, qualname = ?, signature = ?, fingerprint = ?, "
            "start_line = ?, start_col = ?, end_line = ?, end_col = ?, is_definition = ?, visibility = ?, doc = ? WHERE id = ?",
            &stmt_update_symbol_);

        prepare_cached("DELETE FROM nodes WHERE id = ?", &stmt_delete_symbol_);

        prepare_cached("DELETE FROM refs WHERE file_id = ?", &stmt_delete_refs_);

        prepare_cached(
            "DELETE FROM edges INDEXED BY idx_edges_src "
            "WHERE src_id IN (SELECT ? UNION SELECT id FROM nodes "
            "INDEXED BY idx_nodes_file_id WHERE file_id = ?) AND source = 'static'",
            &stmt_delete_file_edges_);

        prepare_cached(
            "INSERT INTO refs(file_id, kind, name, start_line, start_col, end_line, end_col, evidence, containing_node_id, "
            "arg_count, arg_pattern, receiver_type_hint) "
            "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", &stmt_insert_ref_);

        prepare_cached(
            "INSERT INTO edges(src_id, dst_id, kind, confidence, evidence) "
            "VALUES(?, ?, ?, ?, ?)", &stmt_insert_edge_);

        stmts_cached_ = true;
    }

    // DEC-038 OPT-2: Lazy-prepare batch INSERT statements
    void ensure_batch_ref_stmt() {
        if (stmt_batch_insert_ref_) return;
        
        // 80-row batch: 12 params/row * 80 = 960 params (< 999 limit)
        std::string sql = "INSERT INTO refs(file_id, kind, name, start_line, start_col, end_line, end_col, evidence, containing_node_id, arg_count, arg_pattern, receiver_type_hint) VALUES ";
        for (int i = 0; i < 80; ++i) {
            if (i > 0) sql += ",";
            sql += "(?,?,?,?,?,?,?,?,?,?,?,?)";
        }
        prepare_cached(sql.c_str(), &stmt_batch_insert_ref_);
    }

    void ensure_batch_edge_stmt() {
        if (stmt_batch_insert_edge_) return;
        
        // 150-row batch: 5 params/row * 150 = 750 params (< 999 limit)
        std::string sql = "INSERT INTO edges(src_id, dst_id, kind, confidence, evidence) VALUES ";
        for (int i = 0; i < 150; ++i) {
            if (i > 0) sql += ",";
            sql += "(?,?,?,?,?)";
        }
        prepare_cached(sql.c_str(), &stmt_batch_insert_edge_);
    }

    // DEC-039 OPT-1: Lazy-prepare 20-row batch symbol INSERT
    void ensure_batch_symbol_stmt() {
        if (stmt_batch_insert_symbol_) return;

        // 100-row batch: 14 params/row * 100 = 1400 params (< 32766 limit)
        const int SYMBOL_BATCH_SIZE = 100;
        std::string sql = "INSERT INTO nodes(node_type, file_id, kind, name, qualname, signature, fingerprint, "
            "start_line, start_col, end_line, end_col, is_definition, visibility, doc, stable_key) VALUES ";
        for (int i = 0; i < SYMBOL_BATCH_SIZE; ++i) {
            if (i > 0) sql += ",";
            sql += "('symbol',?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
        }
        prepare_cached(sql.c_str(), &stmt_batch_insert_symbol_);
    }

    void finalize_cached_stmts() {
        if (stmt_find_file_)       { sqlite3_finalize(stmt_find_file_);       stmt_find_file_ = nullptr; }
        if (stmt_find_file_node_)  { sqlite3_finalize(stmt_find_file_node_);  stmt_find_file_node_ = nullptr; }
        if (stmt_delete_file_)     { sqlite3_finalize(stmt_delete_file_);     stmt_delete_file_ = nullptr; }
        if (stmt_delete_file_node_){ sqlite3_finalize(stmt_delete_file_node_);stmt_delete_file_node_ = nullptr; }
        if (stmt_insert_file_)     { sqlite3_finalize(stmt_insert_file_);     stmt_insert_file_ = nullptr; }
        if (stmt_update_file_)     { sqlite3_finalize(stmt_update_file_);     stmt_update_file_ = nullptr; }
        if (stmt_insert_file_node_){ sqlite3_finalize(stmt_insert_file_node_);stmt_insert_file_node_ = nullptr; }
        if (stmt_insert_symbol_)   { sqlite3_finalize(stmt_insert_symbol_);   stmt_insert_symbol_ = nullptr; }
        if (stmt_update_symbol_)   { sqlite3_finalize(stmt_update_symbol_);   stmt_update_symbol_ = nullptr; }
        if (stmt_delete_symbol_)   { sqlite3_finalize(stmt_delete_symbol_);   stmt_delete_symbol_ = nullptr; }
        if (stmt_delete_refs_)     { sqlite3_finalize(stmt_delete_refs_);     stmt_delete_refs_ = nullptr; }
        if (stmt_delete_file_edges_){ sqlite3_finalize(stmt_delete_file_edges_); stmt_delete_file_edges_ = nullptr; }
        if (stmt_insert_ref_)      { sqlite3_finalize(stmt_insert_ref_);      stmt_insert_ref_ = nullptr; }
        if (stmt_insert_edge_)     { sqlite3_finalize(stmt_insert_edge_);     stmt_insert_edge_ = nullptr; }
        if (stmt_batch_insert_ref_) { sqlite3_finalize(stmt_batch_insert_ref_); stmt_batch_insert_ref_ = nullptr; }
        if (stmt_batch_insert_edge_){ sqlite3_finalize(stmt_batch_insert_edge_);stmt_batch_insert_edge_ = nullptr; }
        if (stmt_batch_insert_symbol_){ sqlite3_finalize(stmt_batch_insert_symbol_);stmt_batch_insert_symbol_ = nullptr; }
        stmts_cached_ = false;
    }

    int64_t scalar_int64(const char* sql) {
        sqlite3_stmt* stmt = nullptr;
        int64_t value = 0;
        if (sqlite3_prepare_v2(conn_.raw(), sql, -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                value = sqlite3_column_int64(stmt, 0);
            }
        }
        sqlite3_finalize(stmt);
        return value;
    }
};

} // namespace codetopo
