#pragma once

#include "db/connection.h"
#include "index/extractor.h"
#include "index/scanner.h"
#include "util/hash.h"
#include "db/schema.h"
#include <sqlite3.h>
#include <string>
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
    void flush_if_needed(int batch_size) {
        if (in_batch_ && ++batch_count_ >= batch_size) {
            conn_.exec("COMMIT");
            conn_.exec("BEGIN TRANSACTION");
            batch_count_ = 0;
        }
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

    // T040: Persist a single file's extraction results.
    // When used with begin_batch/commit_batch, caller manages the transaction.
    // When used standalone, wraps in its own transaction.
    bool persist_file(const ScannedFile& file,
                      const ExtractionResult& extraction,
                      const std::string& content_hash,
                      const std::string& parse_status,
                      const std::string& parse_error = "") {
        bool own_txn = !in_batch_;
        if (own_txn) conn_.exec("BEGIN TRANSACTION");

        try {
            // Delete existing file record (cascades to nodes → edges, refs)
            {
                sqlite3_stmt* stmt = nullptr;
                sqlite3_prepare_v2(conn_.raw(),
                    "DELETE FROM files WHERE path = ?", -1, &stmt, nullptr);
                sqlite3_bind_text(stmt, 1, file.relative_path.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }

            // Insert file record
            int64_t file_id;
            {
                sqlite3_stmt* stmt = nullptr;
                sqlite3_prepare_v2(conn_.raw(),
                    "INSERT INTO files(path, language, size_bytes, mtime_ns, content_hash, parse_status, parse_error) "
                    "VALUES(?, ?, ?, ?, ?, ?, ?)", -1, &stmt, nullptr);
                sqlite3_bind_text(stmt, 1, file.relative_path.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, file.language.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(stmt, 3, file.size_bytes);
                sqlite3_bind_int64(stmt, 4, file.mtime_ns);
                sqlite3_bind_text(stmt, 5, content_hash.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 6, parse_status.c_str(), -1, SQLITE_TRANSIENT);
                if (parse_error.empty()) {
                    sqlite3_bind_null(stmt, 7);
                } else {
                    sqlite3_bind_text(stmt, 7, parse_error.c_str(), -1, SQLITE_TRANSIENT);
                }
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
                file_id = sqlite3_last_insert_rowid(conn_.raw());
            }

            // Insert file node
            int64_t file_node_id;
            {
                auto file_key = make_file_stable_key(file.relative_path);
                sqlite3_stmt* stmt = nullptr;
                sqlite3_prepare_v2(conn_.raw(),
                    "INSERT INTO nodes(node_type, file_id, kind, name, stable_key) "
                    "VALUES('file', NULL, 'file', ?, ?)", -1, &stmt, nullptr);
                sqlite3_bind_text(stmt, 1, file.relative_path.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, file_key.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
                file_node_id = sqlite3_last_insert_rowid(conn_.raw());
            }

            // Insert symbol nodes
            std::vector<int64_t> symbol_ids;
            {
                sqlite3_stmt* stmt = nullptr;
                sqlite3_prepare_v2(conn_.raw(),
                    "INSERT INTO nodes(node_type, file_id, kind, name, qualname, signature, "
                    "start_line, start_col, end_line, end_col, is_definition, visibility, doc, stable_key) "
                    "VALUES('symbol', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", -1, &stmt, nullptr);

                for (const auto& sym : extraction.symbols) {
                    sqlite3_reset(stmt);
                    sqlite3_bind_int64(stmt, 1, file_id);
                    sqlite3_bind_text(stmt, 2, sym.kind.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(stmt, 3, sym.name.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(stmt, 4, sym.qualname.c_str(), -1, SQLITE_TRANSIENT);
                    if (sym.signature.empty()) sqlite3_bind_null(stmt, 5);
                    else sqlite3_bind_text(stmt, 5, sym.signature.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int(stmt, 6, sym.start_line);
                    sqlite3_bind_int(stmt, 7, sym.start_col);
                    sqlite3_bind_int(stmt, 8, sym.end_line);
                    sqlite3_bind_int(stmt, 9, sym.end_col);
                    sqlite3_bind_int(stmt, 10, sym.is_definition ? 1 : 0);
                    if (sym.visibility.empty()) sqlite3_bind_null(stmt, 11);
                    else sqlite3_bind_text(stmt, 11, sym.visibility.c_str(), -1, SQLITE_TRANSIENT);
                    if (sym.doc.empty()) sqlite3_bind_null(stmt, 12);
                    else sqlite3_bind_text(stmt, 12, sym.doc.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(stmt, 13, sym.stable_key.c_str(), -1, SQLITE_TRANSIENT);

                    sqlite3_step(stmt);
                    symbol_ids.push_back(sqlite3_last_insert_rowid(conn_.raw()));
                }
                sqlite3_finalize(stmt);
            }

            // Insert refs
            {
                sqlite3_stmt* stmt = nullptr;
                sqlite3_prepare_v2(conn_.raw(),
                    "INSERT INTO refs(file_id, kind, name, start_line, start_col, end_line, end_col, evidence) "
                    "VALUES(?, ?, ?, ?, ?, ?, ?, ?)", -1, &stmt, nullptr);

                for (const auto& ref : extraction.refs) {
                    sqlite3_reset(stmt);
                    sqlite3_bind_int64(stmt, 1, file_id);
                    sqlite3_bind_text(stmt, 2, ref.kind.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(stmt, 3, ref.name.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int(stmt, 4, ref.start_line);
                    sqlite3_bind_int(stmt, 5, ref.start_col);
                    sqlite3_bind_int(stmt, 6, ref.end_line);
                    sqlite3_bind_int(stmt, 7, ref.end_col);
                    if (ref.evidence.empty()) sqlite3_bind_null(stmt, 8);
                    else sqlite3_bind_text(stmt, 8, ref.evidence.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_step(stmt);
                }
                sqlite3_finalize(stmt);
            }

            // Insert edges (containment: file_node → symbol)
            {
                sqlite3_stmt* stmt = nullptr;
                sqlite3_prepare_v2(conn_.raw(),
                    "INSERT INTO edges(src_id, dst_id, kind, confidence, evidence) "
                    "VALUES(?, ?, ?, ?, ?)", -1, &stmt, nullptr);

                for (const auto& edge : extraction.edges) {
                    int64_t src_id = (edge.src_index < 0) ? file_node_id : symbol_ids[edge.src_index];
                    int64_t dst_id;

                    if (edge.dst_index >= 0 && edge.dst_index < static_cast<int>(symbol_ids.size())) {
                        dst_id = symbol_ids[edge.dst_index];
                    } else {
                        continue;  // Unresolved cross-file edge — skip for now
                    }

                    if (edge.confidence < 0.3) continue;  // FR-044

                    sqlite3_reset(stmt);
                    sqlite3_bind_int64(stmt, 1, src_id);
                    sqlite3_bind_int64(stmt, 2, dst_id);
                    sqlite3_bind_text(stmt, 3, edge.kind.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_double(stmt, 4, edge.confidence);
                    if (edge.evidence.empty()) sqlite3_bind_null(stmt, 5);
                    else sqlite3_bind_text(stmt, 5, edge.evidence.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_step(stmt);
                }
                sqlite3_finalize(stmt);
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
    }

    // T039: Post-index cross-file reference resolution.
    // Uses in-memory hash map for O(N) resolution instead of O(N*M) correlated subqueries.
    // Returns {refs_resolved, edges_created}.
    std::pair<int,int> resolve_references() {
        int total_resolved = 0;
        int edges_created = 0;

        // --- Step 1: Build in-memory lookup: name → (node_id, file_id, is_definition) ---
        // For each symbol name, keep the best candidate (prefer definitions, then lowest id).
        struct SymbolEntry {
            int64_t id;
            int64_t file_id;
            bool is_definition;
        };
        std::unordered_map<std::string, SymbolEntry> symbol_map;
        // Reserve generously — we expect ~2M symbols
        symbol_map.reserve(2500000);

        {
            sqlite3_stmt* stmt = nullptr;
            sqlite3_prepare_v2(conn_.raw(),
                "SELECT id, name, file_id, is_definition FROM nodes WHERE node_type = 'symbol'",
                -1, &stmt, nullptr);

            int loaded = 0;
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                int64_t id = sqlite3_column_int64(stmt, 0);
                const char* name_raw = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                int64_t file_id = sqlite3_column_int64(stmt, 2);
                bool is_def = sqlite3_column_int(stmt, 3) != 0;

                if (!name_raw) continue;
                std::string name(name_raw);

                auto it = symbol_map.find(name);
                if (it == symbol_map.end()) {
                    symbol_map[name] = {id, file_id, is_def};
                } else {
                    // Prefer definitions over declarations, then lower id
                    auto& existing = it->second;
                    if ((!existing.is_definition && is_def) ||
                        (existing.is_definition == is_def && id < existing.id)) {
                        existing = {id, file_id, is_def};
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

        // --- Step 4: Build class/struct lookup for inherit refs ---
        struct ClassEntry { int64_t id; bool is_def; };
        std::unordered_map<std::string, ClassEntry> class_map;
        {
            sqlite3_stmt* stmt = nullptr;
            sqlite3_prepare_v2(conn_.raw(),
                "SELECT id, name, is_definition FROM nodes "
                "WHERE node_type = 'symbol' AND kind IN ('class','struct','interface')",
                -1, &stmt, nullptr);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                int64_t id = sqlite3_column_int64(stmt, 0);
                const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                bool is_def = sqlite3_column_int(stmt, 2) != 0;
                if (!name) continue;
                auto it = class_map.find(name);
                if (it == class_map.end()) {
                    class_map[name] = {id, is_def};
                } else if (!it->second.is_def && is_def) {
                    it->second = {id, is_def};
                }
            }
            sqlite3_finalize(stmt);
        }

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

        // Scan all unresolved refs and resolve
        sqlite3_stmt* ref_stmt = nullptr;
        sqlite3_prepare_v2(conn_.raw(),
            "SELECT id, file_id, kind, name FROM refs WHERE resolved_node_id IS NULL",
            -1, &ref_stmt, nullptr);

        conn_.exec("BEGIN TRANSACTION");
        int batch = 0;
        int call_resolved = 0, include_resolved = 0, inherit_resolved = 0;

        while (sqlite3_step(ref_stmt) == SQLITE_ROW) {
            int64_t ref_id = sqlite3_column_int64(ref_stmt, 0);
            int64_t ref_file_id = sqlite3_column_int64(ref_stmt, 1);
            const char* kind_raw = reinterpret_cast<const char*>(sqlite3_column_text(ref_stmt, 2));
            const char* name_raw = reinterpret_cast<const char*>(sqlite3_column_text(ref_stmt, 3));
            if (!kind_raw || !name_raw) continue;

            std::string kind(kind_raw);
            std::string name(name_raw);
            int64_t resolved_id = 0;
            bool resolved = false;

            if (kind == "call") {
                auto it = symbol_map.find(name);
                if (it != symbol_map.end() && it->second.file_id != ref_file_id) {
                    resolved_id = it->second.id;
                    resolved = true;
                    ++call_resolved;
                }
            } else if (kind == "include") {
                auto it = include_map.find(name);
                if (it != include_map.end()) {
                    resolved_id = it->second;
                    resolved = true;
                    ++include_resolved;
                }
            } else if (kind == "inherit") {
                auto it = class_map.find(name);
                if (it != class_map.end()) {
                    resolved_id = it->second.id;
                    resolved = true;
                    ++inherit_resolved;
                }
            }

            if (resolved) {
                sqlite3_reset(update_stmt);
                sqlite3_bind_int64(update_stmt, 1, resolved_id);
                sqlite3_bind_int64(update_stmt, 2, ref_id);
                sqlite3_step(update_stmt);
                ++total_resolved;

                if (++batch >= 10000) {
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

        // --- Step 6: Create edges from resolved refs (batch INSERT) ---
        std::cerr << "  Creating edges from resolved refs...\n";
        conn_.exec("BEGIN TRANSACTION");
        char* errmsg = nullptr;
        auto rc = sqlite3_exec(conn_.raw(), R"SQL(
            INSERT OR IGNORE INTO edges(src_id, dst_id, kind, confidence, evidence)
            SELECT
                fn.id,
                r.resolved_node_id,
                CASE r.kind
                    WHEN 'call' THEN 'calls'
                    WHEN 'include' THEN 'includes'
                    WHEN 'inherit' THEN 'inherits'
                    ELSE 'references'
                END,
                0.7,
                'name-match'
            FROM refs r
            JOIN files f ON r.file_id = f.id
            JOIN nodes fn ON fn.node_type = 'file' AND fn.name = f.path
            WHERE r.resolved_node_id IS NOT NULL
        )SQL", nullptr, nullptr, &errmsg);
        if (rc != SQLITE_OK) {
            std::cerr << "  ERROR creating edges: " << (errmsg ? errmsg : "unknown") << "\n";
            if (errmsg) sqlite3_free(errmsg);
        } else {
            edges_created = sqlite3_changes(conn_.raw());
            std::cerr << "  Created " << edges_created << " edges\n";
        }
        conn_.exec("COMMIT");

        return {total_resolved, edges_created};
    }

private:
    Connection& conn_;
    bool in_batch_ = false;
    int batch_count_ = 0;
};

} // namespace codetopo
