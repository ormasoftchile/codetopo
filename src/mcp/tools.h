#pragma once
// T060-T069: MCP tool implementations
// Each tool is a function returning a JSON string.

#include "util/json.h"
#include "util/path.h"
#include "mcp/error.h"
#include "db/connection.h"
#include "db/schema.h"
#include "db/queries.h"
#include <sqlite3.h>
#include <string>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <cstdint>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <algorithm>

namespace codetopo {
namespace tools {

// Helper: build a JSON object string from sqlite statement columns
// (works with the yyjson mutable API)

// T060: server_info
inline std::string server_info(yyjson_val* /*params*/, Connection& conn,
                                QueryCache& /*cache*/, const std::string& repo_root) {
    auto version = schema::get_kv(conn, "schema_version", "0");
    auto idx_version = schema::get_kv(conn, "indexer_version", "unknown");
    auto last_index = schema::get_kv(conn, "last_index_time", "");

    // Check DB status
    std::string db_status = "ok";
    auto integrity = conn.integrity_check();
    if (integrity != "ok") db_status = "error";

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);
    yyjson_mut_obj_add_str(doc.doc, root, "protocol_version", "codetopo-mcp/1.0");
    yyjson_mut_obj_add_int(doc.doc, root, "schema_version", std::atoi(version.c_str()));
    yyjson_mut_obj_add_strcpy(doc.doc, root, "indexer_version", idx_version.c_str());

    auto* caps = doc.new_arr();
    yyjson_mut_arr_add_str(doc.doc, caps, "fts5");
    yyjson_mut_arr_add_str(doc.doc, caps, "source_snippets");
    yyjson_mut_arr_add_str(doc.doc, caps, "context_for");
    yyjson_mut_arr_add_str(doc.doc, caps, "impact_of");
    yyjson_mut_obj_add_val(doc.doc, root, "capabilities", caps);

    yyjson_mut_obj_add_strcpy(doc.doc, root, "repo_root", repo_root.c_str());
    yyjson_mut_obj_add_strcpy(doc.doc, root, "db_status", db_status.c_str());

    return doc.to_string();
}

// T061: repo_stats
inline std::string repo_stats(yyjson_val* /*params*/, Connection& conn,
                               QueryCache& cache, const std::string& /*repo_root*/) {
    auto count_query = [&](const char* table) -> int64_t {
        std::string sql = std::string("SELECT COUNT(*) FROM ") + table;
        auto* stmt = cache.get(std::string("count_") + table, sql);
        int64_t n = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int64(stmt, 0);
        return n;
    };

    auto last_index = schema::get_kv(conn, "last_index_time", "");
    auto idx_version = schema::get_kv(conn, "indexer_version", "");

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);
    yyjson_mut_obj_add_int(doc.doc, root, "file_count", count_query("files"));
    yyjson_mut_obj_add_int(doc.doc, root, "symbol_count", count_query("nodes"));
    yyjson_mut_obj_add_int(doc.doc, root, "edge_count", count_query("edges"));
    yyjson_mut_obj_add_strcpy(doc.doc, root, "last_index_time", last_index.c_str());
    yyjson_mut_obj_add_strcpy(doc.doc, root, "indexer_version", idx_version.c_str());

    return doc.to_string();
}

// T062: symbol_search
inline std::string symbol_search(yyjson_val* params, Connection& conn,
                                  QueryCache& cache, const std::string& /*repo_root*/) {
    const char* query = params ? json_get_str(params, "query") : nullptr;
    if (!query) return McpError::invalid_input("Missing 'query' parameter").to_json_rpc(0);

    const char* kind = params ? json_get_str(params, "kind") : nullptr;
    int64_t limit = params ? json_get_int(params, "limit", 50) : 50;
    int64_t offset = params ? json_get_int(params, "offset", 0) : 0;
    bool include_source = params ? json_get_bool(params, "include_source", false) : false;

    if (limit > 500) limit = 500;

    // When query is "*" or empty, skip FTS and query nodes table directly
    bool wildcard = (strcmp(query, "*") == 0 || strlen(query) == 0);

    sqlite3_stmt* stmt = nullptr;
    int bind_idx = 1;

    if (wildcard) {
        // Direct table scan — no FTS needed
        if (kind && strlen(kind) > 0) {
            stmt = cache.get("symbol_search_wildcard_kind",
                "SELECT n.id, n.kind, n.name, n.qualname, f.path, n.start_line, n.end_line "
                "FROM nodes n LEFT JOIN files f ON n.file_id = f.id "
                "WHERE n.kind = ? "
                "ORDER BY f.path, n.start_line "
                "LIMIT ? OFFSET ?");
            sqlite3_bind_text(stmt, bind_idx++, kind, -1, SQLITE_TRANSIENT);
        } else {
            stmt = cache.get("symbol_search_wildcard",
                "SELECT n.id, n.kind, n.name, n.qualname, f.path, n.start_line, n.end_line "
                "FROM nodes n LEFT JOIN files f ON n.file_id = f.id "
                "ORDER BY f.path, n.start_line "
                "LIMIT ? OFFSET ?");
        }
    } else {
        // Build FTS5 query: quote the term for exact prefix match on name column
        // Using name: prefix to target only the name column (fastest)
        std::string fts_query = "name: \"" + std::string(query) + "\"*";

        if (kind && strlen(kind) > 0) {
            stmt = cache.get("symbol_search_kind",
                "SELECT n.id, n.kind, n.name, n.qualname, f.path, n.start_line, n.end_line "
                "FROM nodes_fts fts JOIN nodes n ON fts.rowid = n.id "
                "LEFT JOIN files f ON n.file_id = f.id "
                "WHERE nodes_fts MATCH ? AND n.kind = ? "
                "LIMIT ? OFFSET ?");
        } else {
            stmt = cache.get("symbol_search",
                "SELECT n.id, n.kind, n.name, n.qualname, f.path, n.start_line, n.end_line "
                "FROM nodes_fts fts JOIN nodes n ON fts.rowid = n.id "
                "LEFT JOIN files f ON n.file_id = f.id "
                "WHERE nodes_fts MATCH ? "
                "LIMIT ? OFFSET ?");
        }

        sqlite3_bind_text(stmt, bind_idx++, fts_query.c_str(), -1, SQLITE_TRANSIENT);
        if (kind && strlen(kind) > 0) {
            sqlite3_bind_text(stmt, bind_idx++, kind, -1, SQLITE_TRANSIENT);
        }
    }
    sqlite3_bind_int64(stmt, bind_idx++, limit + 1);  // +1 to detect has_more
    sqlite3_bind_int64(stmt, bind_idx++, offset);

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);
    auto* results = doc.new_arr();

    int count = 0;
    bool has_more = false;
    while (sqlite3_step(stmt) == SQLITE_ROW && count <= limit) {
        if (count == limit) { has_more = true; break; }
        count++;

        auto* item = doc.new_obj();
        yyjson_mut_obj_add_int(doc.doc, item, "node_id", sqlite3_column_int64(stmt, 0));
        yyjson_mut_obj_add_strcpy(doc.doc, item, "kind",
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
        yyjson_mut_obj_add_strcpy(doc.doc, item, "name",
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
        auto* qn = sqlite3_column_text(stmt, 3);
        if (qn) yyjson_mut_obj_add_strcpy(doc.doc, item, "qualname", reinterpret_cast<const char*>(qn));
        auto* fp = sqlite3_column_text(stmt, 4);
        if (fp) yyjson_mut_obj_add_strcpy(doc.doc, item, "file_path", reinterpret_cast<const char*>(fp));

        auto* span = doc.new_obj();
        yyjson_mut_obj_add_int(doc.doc, span, "start_line", sqlite3_column_int(stmt, 5));
        yyjson_mut_obj_add_int(doc.doc, span, "end_line", sqlite3_column_int(stmt, 6));
        yyjson_mut_obj_add_val(doc.doc, item, "span", span);

        yyjson_mut_arr_append(results, item);
    }

    yyjson_mut_obj_add_val(doc.doc, root, "results", results);
    yyjson_mut_obj_add_bool(doc.doc, root, "has_more", has_more);

    return doc.to_string();
}

// T062b: symbol_list — list/filter symbols without FTS, supports kind, file, and name-glob filters
inline std::string symbol_list(yyjson_val* params, Connection& conn,
                               QueryCache& cache, const std::string& /*repo_root*/) {
    const char* kind = params ? json_get_str(params, "kind") : nullptr;
    const char* file_path = params ? json_get_str(params, "file_path") : nullptr;
    const char* name_glob = params ? json_get_str(params, "name_glob") : nullptr;
    int64_t limit = params ? json_get_int(params, "limit", 200) : 200;
    int64_t offset = params ? json_get_int(params, "offset", 0) : 0;

    if (limit > 2000) limit = 2000;

    // Build SQL dynamically based on which filters are provided
    std::string sql =
        "SELECT n.id, n.kind, n.name, n.qualname, f.path, n.start_line, n.end_line "
        "FROM nodes n LEFT JOIN files f ON n.file_id = f.id WHERE 1=1";

    if (kind && strlen(kind) > 0)       sql += " AND n.kind = ?";
    if (file_path && strlen(file_path) > 0) sql += " AND f.path = ?";
    if (name_glob && strlen(name_glob) > 0) sql += " AND n.name GLOB ?";
    sql += " ORDER BY f.path, n.start_line LIMIT ? OFFSET ?";

    // Use a cache key that encodes which filters are active
    std::string cache_key = "symbol_list";
    if (kind && strlen(kind) > 0) cache_key += "_k";
    if (file_path && strlen(file_path) > 0) cache_key += "_f";
    if (name_glob && strlen(name_glob) > 0) cache_key += "_g";

    auto* stmt = cache.get(cache_key, sql);
    int bind_idx = 1;
    if (kind && strlen(kind) > 0)
        sqlite3_bind_text(stmt, bind_idx++, kind, -1, SQLITE_TRANSIENT);
    if (file_path && strlen(file_path) > 0)
        sqlite3_bind_text(stmt, bind_idx++, file_path, -1, SQLITE_TRANSIENT);
    if (name_glob && strlen(name_glob) > 0)
        sqlite3_bind_text(stmt, bind_idx++, name_glob, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, bind_idx++, limit + 1);
    sqlite3_bind_int64(stmt, bind_idx++, offset);

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);
    auto* results = doc.new_arr();

    int count = 0;
    bool has_more = false;
    while (sqlite3_step(stmt) == SQLITE_ROW && count <= limit) {
        if (count == limit) { has_more = true; break; }
        count++;

        auto* item = doc.new_obj();
        yyjson_mut_obj_add_int(doc.doc, item, "node_id", sqlite3_column_int64(stmt, 0));
        yyjson_mut_obj_add_strcpy(doc.doc, item, "kind",
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
        yyjson_mut_obj_add_strcpy(doc.doc, item, "name",
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
        auto* qn = sqlite3_column_text(stmt, 3);
        if (qn) yyjson_mut_obj_add_strcpy(doc.doc, item, "qualname", reinterpret_cast<const char*>(qn));
        auto* fp = sqlite3_column_text(stmt, 4);
        if (fp) yyjson_mut_obj_add_strcpy(doc.doc, item, "file_path", reinterpret_cast<const char*>(fp));

        auto* span = doc.new_obj();
        yyjson_mut_obj_add_int(doc.doc, span, "start_line", sqlite3_column_int(stmt, 5));
        yyjson_mut_obj_add_int(doc.doc, span, "end_line", sqlite3_column_int(stmt, 6));
        yyjson_mut_obj_add_val(doc.doc, item, "span", span);

        yyjson_mut_arr_append(results, item);
    }

    yyjson_mut_obj_add_val(doc.doc, root, "results", results);
    yyjson_mut_obj_add_bool(doc.doc, root, "has_more", has_more);

    return doc.to_string();
}

// Helper: read source snippet from disk
inline std::string read_source_snippet(const std::string& repo_root,
                                        const std::string& rel_path,
                                        int start_line, int end_line) {
    auto validated = path_util::validate_mcp_path(rel_path, repo_root);
    if (validated.empty()) return "";

    auto full_path = std::filesystem::path(repo_root) / rel_path;
    std::ifstream f(full_path);
    if (!f) return "";

    std::string result;
    std::string line;
    int ln = 0;
    while (std::getline(f, line)) {
        ln++;
        if (ln >= start_line && ln <= end_line) {
            result += line + "\n";
        }
        if (ln > end_line) break;
    }
    return result;
}

// T063: symbol_get
inline std::string symbol_get(yyjson_val* params, Connection& conn,
                               QueryCache& cache, const std::string& repo_root) {
    int64_t node_id = params ? json_get_int(params, "node_id", -1) : -1;
    if (node_id < 0) return McpError::invalid_input("Missing 'node_id'").to_json_rpc(0);

    bool include_source = params ? json_get_bool(params, "include_source", true) : true;

    auto* stmt = cache.get("symbol_get",
        "SELECT n.id, n.kind, n.name, n.qualname, n.signature, f.path, "
        "n.start_line, n.start_col, n.end_line, n.end_col, n.doc, f.mtime_ns "
        "FROM nodes n LEFT JOIN files f ON n.file_id = f.id "
        "WHERE n.id = ? AND n.node_type = 'symbol'");

    sqlite3_bind_int64(stmt, 1, node_id);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        return McpError::not_found("Symbol not found: " + std::to_string(node_id)).to_json_rpc(0);
    }

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);

    yyjson_mut_obj_add_int(doc.doc, root, "node_id", sqlite3_column_int64(stmt, 0));
    yyjson_mut_obj_add_strcpy(doc.doc, root, "kind",
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
    yyjson_mut_obj_add_strcpy(doc.doc, root, "name",
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));

    auto* qn = sqlite3_column_text(stmt, 3);
    if (qn) yyjson_mut_obj_add_strcpy(doc.doc, root, "qualname", reinterpret_cast<const char*>(qn));
    auto* sig = sqlite3_column_text(stmt, 4);
    if (sig) yyjson_mut_obj_add_strcpy(doc.doc, root, "signature", reinterpret_cast<const char*>(sig));

    auto* fp = sqlite3_column_text(stmt, 5);
    std::string file_path = fp ? reinterpret_cast<const char*>(fp) : "";
    if (!file_path.empty()) yyjson_mut_obj_add_strcpy(doc.doc, root, "file_path", file_path.c_str());

    int start_line = sqlite3_column_int(stmt, 6);
    int end_line = sqlite3_column_int(stmt, 8);

    auto* span = doc.new_obj();
    yyjson_mut_obj_add_int(doc.doc, span, "start_line", start_line);
    yyjson_mut_obj_add_int(doc.doc, span, "end_line", end_line);
    yyjson_mut_obj_add_val(doc.doc, root, "span", span);

    auto* doc_text = sqlite3_column_text(stmt, 10);
    if (doc_text) yyjson_mut_obj_add_strcpy(doc.doc, root, "doc", reinterpret_cast<const char*>(doc_text));

    // Source snippet + staleness detection (T071)
    if (include_source && !file_path.empty()) {
        auto source = read_source_snippet(repo_root, file_path, start_line, end_line);
        if (!source.empty()) {
            yyjson_mut_obj_add_strcpy(doc.doc, root, "source", source.c_str());
        }

        // Staleness check
        int64_t indexed_mtime = sqlite3_column_int64(stmt, 11);
        auto full = std::filesystem::path(repo_root) / file_path;
        std::error_code ec;
        auto current_mtime = std::filesystem::last_write_time(full, ec);
        if (!ec) {
            auto current_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                current_mtime.time_since_epoch()).count();
            if (current_ns != indexed_mtime) {
                yyjson_mut_obj_add_bool(doc.doc, root, "stale", true);
                yyjson_mut_obj_add_str(doc.doc, root, "stale_reason", "file modified since last index");
            }
        }
    }

    return doc.to_string();
}

// T064: symbol_get_batch — multi-ID lookup
inline std::string symbol_get_batch(yyjson_val* params, Connection& conn,
                                     QueryCache& cache, const std::string& repo_root) {
    if (!params) return McpError::invalid_input("Missing parameters").to_json_rpc(0);

    auto* ids_val = yyjson_obj_get(params, "node_ids");
    if (!ids_val || !yyjson_is_arr(ids_val))
        return McpError::invalid_input("Missing 'node_ids' array").to_json_rpc(0);

    size_t count = yyjson_arr_size(ids_val);
    if (count > 50) count = 50;

    bool include_source = json_get_bool(params, "include_source", true);

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);
    auto* results = doc.new_arr();

    yyjson_val* id_item;
    size_t idx, max;
    yyjson_arr_foreach(ids_val, idx, max, id_item) {
        if (idx >= 50) break;

        int64_t nid = yyjson_get_sint(id_item);

        auto* stmt = cache.get("symbol_get",
            "SELECT n.id, n.kind, n.name, n.qualname, n.signature, f.path, "
            "n.start_line, n.start_col, n.end_line, n.end_col, n.doc, f.mtime_ns "
            "FROM nodes n LEFT JOIN files f ON n.file_id = f.id "
            "WHERE n.id = ? AND n.node_type = 'symbol'");
        sqlite3_bind_int64(stmt, 1, nid);

        if (sqlite3_step(stmt) != SQLITE_ROW) continue;  // Silently omit missing

        auto* item = doc.new_obj();
        yyjson_mut_obj_add_int(doc.doc, item, "node_id", sqlite3_column_int64(stmt, 0));
        yyjson_mut_obj_add_strcpy(doc.doc, item, "kind",
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
        yyjson_mut_obj_add_strcpy(doc.doc, item, "name",
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
        auto* qn = sqlite3_column_text(stmt, 3);
        if (qn) yyjson_mut_obj_add_strcpy(doc.doc, item, "qualname", reinterpret_cast<const char*>(qn));
        auto* sig = sqlite3_column_text(stmt, 4);
        if (sig) yyjson_mut_obj_add_strcpy(doc.doc, item, "signature", reinterpret_cast<const char*>(sig));
        auto* fp = sqlite3_column_text(stmt, 5);
        std::string file_path = fp ? reinterpret_cast<const char*>(fp) : "";
        if (!file_path.empty()) yyjson_mut_obj_add_strcpy(doc.doc, item, "file_path", file_path.c_str());

        int start_line = sqlite3_column_int(stmt, 6);
        int end_line = sqlite3_column_int(stmt, 8);
        auto* span = doc.new_obj();
        yyjson_mut_obj_add_int(doc.doc, span, "start_line", start_line);
        yyjson_mut_obj_add_int(doc.doc, span, "end_line", end_line);
        yyjson_mut_obj_add_val(doc.doc, item, "span", span);

        if (include_source && !file_path.empty()) {
            auto source = read_source_snippet(repo_root, file_path, start_line, end_line);
            if (!source.empty()) yyjson_mut_obj_add_strcpy(doc.doc, item, "source", source.c_str());
        }

        yyjson_mut_arr_append(results, item);
    }

    yyjson_mut_obj_add_val(doc.doc, root, "results", results);
    return doc.to_string();
}

// T061: repo_stats already defined above

// T066: callers_approx
inline std::string callers_approx(yyjson_val* params, Connection& conn,
                                    QueryCache& cache, const std::string& repo_root) {
    int64_t node_id = params ? json_get_int(params, "node_id", -1) : -1;
    if (node_id < 0) return McpError::invalid_input("Missing 'node_id'").to_json_rpc(0);

    int64_t limit = params ? json_get_int(params, "limit", 50) : 50;
    if (limit > 500) limit = 500;

    auto* stmt = cache.get("callers_approx",
        "SELECT e.src_id, n.name, f.path, e.confidence "
        "FROM edges e "
        "JOIN nodes n ON e.src_id = n.id "
        "LEFT JOIN files f ON n.file_id = f.id "
        "WHERE e.dst_id = ? AND e.kind = 'calls' "
        "ORDER BY e.confidence DESC LIMIT ?");

    sqlite3_bind_int64(stmt, 1, node_id);
    sqlite3_bind_int64(stmt, 2, limit);

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);
    auto* results = doc.new_arr();

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        auto* item = doc.new_obj();
        yyjson_mut_obj_add_int(doc.doc, item, "caller_node_id", sqlite3_column_int64(stmt, 0));
        yyjson_mut_obj_add_strcpy(doc.doc, item, "caller_name",
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
        auto* fp = sqlite3_column_text(stmt, 2);
        if (fp) yyjson_mut_obj_add_strcpy(doc.doc, item, "file_path", reinterpret_cast<const char*>(fp));
        yyjson_mut_obj_add_real(doc.doc, item, "confidence", sqlite3_column_double(stmt, 3));
        yyjson_mut_arr_append(results, item);
    }

    yyjson_mut_obj_add_val(doc.doc, root, "results", results);
    yyjson_mut_obj_add_bool(doc.doc, root, "has_more", false);

    return doc.to_string();
}

// T067: callees_approx
inline std::string callees_approx(yyjson_val* params, Connection& conn,
                                    QueryCache& cache, const std::string& /*repo_root*/) {
    int64_t node_id = params ? json_get_int(params, "node_id", -1) : -1;
    if (node_id < 0) return McpError::invalid_input("Missing 'node_id'").to_json_rpc(0);

    int64_t limit = params ? json_get_int(params, "limit", 50) : 50;
    if (limit > 500) limit = 500;

    auto* stmt = cache.get("callees_approx",
        "SELECT e.dst_id, n.name, n.qualname, n.signature, e.confidence "
        "FROM edges e "
        "JOIN nodes n ON e.dst_id = n.id "
        "WHERE e.src_id = ? AND e.kind = 'calls' "
        "ORDER BY e.confidence DESC LIMIT ?");

    sqlite3_bind_int64(stmt, 1, node_id);
    sqlite3_bind_int64(stmt, 2, limit);

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);
    auto* results = doc.new_arr();

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        auto* item = doc.new_obj();
        yyjson_mut_obj_add_int(doc.doc, item, "callee_node_id", sqlite3_column_int64(stmt, 0));
        yyjson_mut_obj_add_strcpy(doc.doc, item, "callee_name",
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
        auto* qn = sqlite3_column_text(stmt, 2);
        if (qn) yyjson_mut_obj_add_strcpy(doc.doc, item, "qualname", reinterpret_cast<const char*>(qn));
        auto* sig = sqlite3_column_text(stmt, 3);
        if (sig) yyjson_mut_obj_add_strcpy(doc.doc, item, "signature", reinterpret_cast<const char*>(sig));
        yyjson_mut_obj_add_real(doc.doc, item, "confidence", sqlite3_column_double(stmt, 4));
        yyjson_mut_arr_append(results, item);
    }

    yyjson_mut_obj_add_val(doc.doc, root, "results", results);
    return doc.to_string();
}

// T065: references
inline std::string references(yyjson_val* params, Connection& conn,
                               QueryCache& cache, const std::string& /*repo_root*/) {
    int64_t node_id = params ? json_get_int(params, "node_id", -1) : -1;
    if (node_id < 0) return McpError::invalid_input("Missing 'node_id'").to_json_rpc(0);

    int64_t limit = params ? json_get_int(params, "limit", 50) : 50;
    if (limit > 500) limit = 500;

    auto* stmt = cache.get("references",
        "SELECT r.id, r.kind, r.name, f.path, r.start_line, r.start_col, r.end_line, r.end_col, r.evidence "
        "FROM refs r LEFT JOIN files f ON r.file_id = f.id "
        "WHERE r.resolved_node_id = ? LIMIT ?");

    sqlite3_bind_int64(stmt, 1, node_id);
    sqlite3_bind_int64(stmt, 2, limit);

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);
    auto* results = doc.new_arr();

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        auto* item = doc.new_obj();
        yyjson_mut_obj_add_int(doc.doc, item, "ref_id", sqlite3_column_int64(stmt, 0));
        yyjson_mut_obj_add_strcpy(doc.doc, item, "kind",
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
        yyjson_mut_obj_add_strcpy(doc.doc, item, "name",
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
        auto* fp = sqlite3_column_text(stmt, 3);
        if (fp) yyjson_mut_obj_add_strcpy(doc.doc, item, "file_path", reinterpret_cast<const char*>(fp));

        auto* span = doc.new_obj();
        yyjson_mut_obj_add_int(doc.doc, span, "start_line", sqlite3_column_int(stmt, 4));
        yyjson_mut_obj_add_int(doc.doc, span, "start_col", sqlite3_column_int(stmt, 5));
        yyjson_mut_obj_add_int(doc.doc, span, "end_line", sqlite3_column_int(stmt, 6));
        yyjson_mut_obj_add_int(doc.doc, span, "end_col", sqlite3_column_int(stmt, 7));
        yyjson_mut_obj_add_val(doc.doc, item, "span", span);

        yyjson_mut_arr_append(results, item);
    }

    yyjson_mut_obj_add_val(doc.doc, root, "results", results);
    return doc.to_string();
}

// T069: file_summary
inline std::string file_summary(yyjson_val* params, Connection& conn,
                                 QueryCache& cache, const std::string& repo_root) {
    const char* path = params ? json_get_str(params, "path") : nullptr;
    if (!path) return McpError::invalid_input("Missing 'path' parameter").to_json_rpc(0);

    // T072: Path traversal guard
    auto validated = path_util::validate_mcp_path(path, repo_root);
    if (validated.empty()) return McpError::invalid_input("Invalid path: rejected by traversal guard").to_json_rpc(0);

    auto* stmt = cache.get("file_by_path",
        "SELECT f.id, f.language, f.path FROM files f WHERE f.path = ?");
    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        return McpError::not_found(std::string("File not found: ") + path).to_json_rpc(0);
    }

    int64_t file_id = sqlite3_column_int64(stmt, 0);
    std::string language = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));

    // Count lines
    auto full = std::filesystem::path(repo_root) / path;
    int lines = 0;
    {
        std::ifstream f(full);
        std::string line;
        while (std::getline(f, line)) lines++;
    }

    // Get symbols
    auto* sym_stmt = cache.get("file_symbols",
        "SELECT kind, name, qualname, visibility, signature, start_line, end_line "
        "FROM nodes WHERE file_id = ? AND node_type = 'symbol' ORDER BY start_line");
    sqlite3_bind_int64(sym_stmt, 1, file_id);

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);
    yyjson_mut_obj_add_strcpy(doc.doc, root, "path", path);
    yyjson_mut_obj_add_int(doc.doc, root, "lines", lines);
    yyjson_mut_obj_add_strcpy(doc.doc, root, "language", language.c_str());

    auto* symbols = doc.new_arr();
    while (sqlite3_step(sym_stmt) == SQLITE_ROW) {
        auto* sym = doc.new_obj();
        yyjson_mut_obj_add_strcpy(doc.doc, sym, "kind",
            reinterpret_cast<const char*>(sqlite3_column_text(sym_stmt, 0)));
        yyjson_mut_obj_add_strcpy(doc.doc, sym, "name",
            reinterpret_cast<const char*>(sqlite3_column_text(sym_stmt, 1)));
        auto* qn = sqlite3_column_text(sym_stmt, 2);
        if (qn) yyjson_mut_obj_add_strcpy(doc.doc, sym, "qualname", reinterpret_cast<const char*>(qn));
        auto* vis = sqlite3_column_text(sym_stmt, 3);
        if (vis) yyjson_mut_obj_add_strcpy(doc.doc, sym, "visibility", reinterpret_cast<const char*>(vis));

        auto* span = doc.new_obj();
        yyjson_mut_obj_add_int(doc.doc, span, "start_line", sqlite3_column_int(sym_stmt, 5));
        yyjson_mut_obj_add_int(doc.doc, span, "end_line", sqlite3_column_int(sym_stmt, 6));
        yyjson_mut_obj_add_val(doc.doc, sym, "span", span);

        yyjson_mut_arr_append(symbols, sym);
    }
    yyjson_mut_obj_add_val(doc.doc, root, "symbols", symbols);

    return doc.to_string();
}

// T068: context_for (one-shot symbol understanding)
inline std::string context_for(yyjson_val* params, Connection& conn,
                                QueryCache& cache, const std::string& repo_root) {
    int64_t node_id = params ? json_get_int(params, "node_id", -1) : -1;
    if (node_id < 0) return McpError::invalid_input("Missing 'node_id'").to_json_rpc(0);

    int64_t max_callers = params ? json_get_int(params, "max_callers", 10) : 10;
    int64_t max_callees = params ? json_get_int(params, "max_callees", 10) : 10;

    // Get the symbol itself
    std::string sym_json = symbol_get(params, conn, cache, repo_root);

    // Get callers
    JsonMutDoc caller_params_doc;
    auto* cp = caller_params_doc.new_obj();
    caller_params_doc.set_root(cp);
    yyjson_mut_obj_add_int(caller_params_doc.doc, cp, "node_id", node_id);
    yyjson_mut_obj_add_int(caller_params_doc.doc, cp, "limit", max_callers);

    // We reuse the callers/callees functions by creating param JSON
    // For simplicity, build the response directly

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);

    // Symbol info (inline from symbol_get query)
    auto* sym_stmt = cache.get("context_symbol",
        "SELECT n.id, n.kind, n.name, n.qualname, n.signature, f.path, "
        "n.start_line, n.end_line, n.doc "
        "FROM nodes n LEFT JOIN files f ON n.file_id = f.id "
        "WHERE n.id = ? AND n.node_type = 'symbol'");
    sqlite3_bind_int64(sym_stmt, 1, node_id);

    if (sqlite3_step(sym_stmt) != SQLITE_ROW) {
        return McpError::not_found("Symbol not found").to_json_rpc(0);
    }

    auto* symbol = doc.new_obj();
    yyjson_mut_obj_add_int(doc.doc, symbol, "node_id", sqlite3_column_int64(sym_stmt, 0));
    yyjson_mut_obj_add_strcpy(doc.doc, symbol, "kind",
        reinterpret_cast<const char*>(sqlite3_column_text(sym_stmt, 1)));
    yyjson_mut_obj_add_strcpy(doc.doc, symbol, "name",
        reinterpret_cast<const char*>(sqlite3_column_text(sym_stmt, 2)));
    auto* qn = sqlite3_column_text(sym_stmt, 3);
    if (qn) yyjson_mut_obj_add_strcpy(doc.doc, symbol, "qualname", reinterpret_cast<const char*>(qn));
    auto* sig = sqlite3_column_text(sym_stmt, 4);
    if (sig) yyjson_mut_obj_add_strcpy(doc.doc, symbol, "signature", reinterpret_cast<const char*>(sig));
    auto* fp = sqlite3_column_text(sym_stmt, 5);
    std::string file_path = fp ? reinterpret_cast<const char*>(fp) : "";
    if (!file_path.empty()) {
        yyjson_mut_obj_add_strcpy(doc.doc, symbol, "file_path", file_path.c_str());
        int sl = sqlite3_column_int(sym_stmt, 6);
        int el = sqlite3_column_int(sym_stmt, 7);
        auto source = read_source_snippet(repo_root, file_path, sl, el);
        if (!source.empty()) yyjson_mut_obj_add_strcpy(doc.doc, symbol, "source", source.c_str());
    }

    yyjson_mut_obj_add_val(doc.doc, root, "symbol", symbol);

    // Callers
    auto* callers_stmt = cache.get("context_callers",
        "SELECT e.src_id, n.name, f.path, e.confidence "
        "FROM edges e JOIN nodes n ON e.src_id = n.id "
        "LEFT JOIN files f ON n.file_id = f.id "
        "WHERE e.dst_id = ? AND e.kind = 'calls' LIMIT ?");
    sqlite3_bind_int64(callers_stmt, 1, node_id);
    sqlite3_bind_int64(callers_stmt, 2, max_callers);

    auto* callers_arr = doc.new_arr();
    while (sqlite3_step(callers_stmt) == SQLITE_ROW) {
        auto* c = doc.new_obj();
        yyjson_mut_obj_add_strcpy(doc.doc, c, "caller_name",
            reinterpret_cast<const char*>(sqlite3_column_text(callers_stmt, 1)));
        auto* cfp = sqlite3_column_text(callers_stmt, 2);
        if (cfp) yyjson_mut_obj_add_strcpy(doc.doc, c, "file_path", reinterpret_cast<const char*>(cfp));
        yyjson_mut_obj_add_real(doc.doc, c, "confidence", sqlite3_column_double(callers_stmt, 3));
        yyjson_mut_arr_append(callers_arr, c);
    }
    yyjson_mut_obj_add_val(doc.doc, root, "callers", callers_arr);

    // Callees
    auto* callees_stmt = cache.get("context_callees",
        "SELECT e.dst_id, n.name, n.qualname, n.signature, e.confidence "
        "FROM edges e JOIN nodes n ON e.dst_id = n.id "
        "WHERE e.src_id = ? AND e.kind = 'calls' LIMIT ?");
    sqlite3_bind_int64(callees_stmt, 1, node_id);
    sqlite3_bind_int64(callees_stmt, 2, max_callees);

    auto* callees_arr = doc.new_arr();
    while (sqlite3_step(callees_stmt) == SQLITE_ROW) {
        auto* c = doc.new_obj();
        yyjson_mut_obj_add_strcpy(doc.doc, c, "callee_name",
            reinterpret_cast<const char*>(sqlite3_column_text(callees_stmt, 1)));
        auto* cqn = sqlite3_column_text(callees_stmt, 2);
        if (cqn) yyjson_mut_obj_add_strcpy(doc.doc, c, "qualname", reinterpret_cast<const char*>(cqn));
        yyjson_mut_obj_add_real(doc.doc, c, "confidence", sqlite3_column_double(callees_stmt, 4));
        yyjson_mut_arr_append(callees_arr, c);
    }
    yyjson_mut_obj_add_val(doc.doc, root, "callees", callees_arr);

    return doc.to_string();
}

// ===== US3: Graph Exploration Tools =====

// T084: entrypoints — find natural starting points
inline std::string entrypoints(yyjson_val* params, Connection& conn,
                                QueryCache& cache, const std::string& /*repo_root*/) {
    int64_t limit = params ? json_get_int(params, "limit", 20) : 20;

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);
    auto* results = doc.new_arr();

    // 1. Main functions
    {
        auto* stmt = cache.get("entrypoints_main",
            "SELECT n.id, n.kind, n.name, f.path, n.start_line, n.end_line "
            "FROM nodes n LEFT JOIN files f ON n.file_id = f.id "
            "WHERE n.node_type = 'symbol' AND n.kind = 'function' "
            "AND (n.name = 'main' OR n.name = 'Main' OR n.name = 'wmain') "
            "LIMIT ?");
        sqlite3_bind_int64(stmt, 1, limit);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            auto* item = doc.new_obj();
            yyjson_mut_obj_add_int(doc.doc, item, "node_id", sqlite3_column_int64(stmt, 0));
            yyjson_mut_obj_add_strcpy(doc.doc, item, "kind",
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
            yyjson_mut_obj_add_strcpy(doc.doc, item, "name",
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
            auto* fp = sqlite3_column_text(stmt, 3);
            if (fp) yyjson_mut_obj_add_strcpy(doc.doc, item, "file_path", reinterpret_cast<const char*>(fp));
            auto* span = doc.new_obj();
            yyjson_mut_obj_add_int(doc.doc, span, "start_line", sqlite3_column_int(stmt, 4));
            yyjson_mut_obj_add_int(doc.doc, span, "end_line", sqlite3_column_int(stmt, 5));
            yyjson_mut_obj_add_val(doc.doc, item, "span", span);
            yyjson_mut_obj_add_str(doc.doc, item, "reason", "main_function");
            yyjson_mut_arr_append(results, item);
        }
    }

    // 2. High in-degree symbols (most referenced)
    // Use a subquery on edges alone (indexed) to find top dst_ids,
    // then join to nodes only for those few results.
    {
        auto* stmt = cache.get("entrypoints_indegree",
            "SELECT n.id, n.kind, n.name, f.path, n.start_line, n.end_line, top.cnt "
            "FROM (SELECT dst_id, COUNT(*) as cnt FROM edges GROUP BY dst_id ORDER BY cnt DESC LIMIT ?) top "
            "JOIN nodes n ON n.id = top.dst_id "
            "LEFT JOIN files f ON n.file_id = f.id "
            "WHERE n.node_type = 'symbol' "
            "ORDER BY top.cnt DESC");
        sqlite3_bind_int64(stmt, 1, limit);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            auto* item = doc.new_obj();
            yyjson_mut_obj_add_int(doc.doc, item, "node_id", sqlite3_column_int64(stmt, 0));
            yyjson_mut_obj_add_strcpy(doc.doc, item, "kind",
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
            yyjson_mut_obj_add_strcpy(doc.doc, item, "name",
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
            auto* fp = sqlite3_column_text(stmt, 3);
            if (fp) yyjson_mut_obj_add_strcpy(doc.doc, item, "file_path", reinterpret_cast<const char*>(fp));
            auto* span = doc.new_obj();
            yyjson_mut_obj_add_int(doc.doc, span, "start_line", sqlite3_column_int(stmt, 4));
            yyjson_mut_obj_add_int(doc.doc, span, "end_line", sqlite3_column_int(stmt, 5));
            yyjson_mut_obj_add_val(doc.doc, item, "span", span);
            yyjson_mut_obj_add_str(doc.doc, item, "reason", "high_in_degree");
            yyjson_mut_arr_append(results, item);
        }
    }

    yyjson_mut_obj_add_val(doc.doc, root, "results", results);
    return doc.to_string();
}

// T085: impact_of — transitive dependents via BFS
inline std::string impact_of(yyjson_val* params, Connection& conn,
                              QueryCache& cache, const std::string& /*repo_root*/) {
    int64_t node_id = params ? json_get_int(params, "node_id", -1) : -1;
    if (node_id < 0) return McpError::invalid_input("Missing 'node_id'").to_json_rpc(0);

    int64_t depth = params ? json_get_int(params, "depth", 2) : 2;
    if (depth > 3) depth = 3;
    int64_t max_nodes = params ? json_get_int(params, "max_nodes", 50) : 50;
    if (max_nodes > 200) max_nodes = 200;

    // BFS from node_id following reverse edges
    std::vector<int64_t> frontier = {node_id};
    std::unordered_set<int64_t> visited = {node_id};

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);

    // Symbol info
    auto* sym = doc.new_obj();
    {
        auto* stmt = cache.get("impact_symbol",
            "SELECT name, f.path FROM nodes n LEFT JOIN files f ON n.file_id = f.id WHERE n.id = ?");
        sqlite3_bind_int64(stmt, 1, node_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            yyjson_mut_obj_add_strcpy(doc.doc, sym, "name",
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
            auto* fp = sqlite3_column_text(stmt, 1);
            if (fp) yyjson_mut_obj_add_strcpy(doc.doc, sym, "file_path", reinterpret_cast<const char*>(fp));
        }
    }
    yyjson_mut_obj_add_val(doc.doc, root, "symbol", sym);

    auto* impacted = doc.new_arr();
    std::unordered_set<std::string> impacted_files;
    bool truncated = false;
    int total = 0;

    for (int d = 1; d <= depth && !truncated; ++d) {
        std::vector<int64_t> next_frontier;
        for (int64_t nid : frontier) {
            // Find nodes that depend on nid (reverse edges: dst_id = nid)
            auto* stmt = cache.get("impact_reverse",
                "SELECT e.src_id, n.name, f.path, e.kind, e.confidence "
                "FROM edges e JOIN nodes n ON e.src_id = n.id "
                "LEFT JOIN files f ON n.file_id = f.id "
                "WHERE e.dst_id = ? AND e.kind != 'contains'");
            sqlite3_bind_int64(stmt, 1, nid);

            while (sqlite3_step(stmt) == SQLITE_ROW) {
                int64_t src_id = sqlite3_column_int64(stmt, 0);
                if (visited.count(src_id)) continue;
                visited.insert(src_id);

                if (total >= max_nodes) { truncated = true; break; }
                total++;

                auto* item = doc.new_obj();
                yyjson_mut_obj_add_int(doc.doc, item, "node_id", src_id);
                yyjson_mut_obj_add_strcpy(doc.doc, item, "name",
                    reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
                auto* fp = sqlite3_column_text(stmt, 2);
                if (fp) {
                    std::string fpath(reinterpret_cast<const char*>(fp));
                    yyjson_mut_obj_add_strcpy(doc.doc, item, "file_path", fpath.c_str());
                    impacted_files.insert(fpath);
                }
                yyjson_mut_obj_add_strcpy(doc.doc, item, "relationship",
                    reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)));
                yyjson_mut_obj_add_int(doc.doc, item, "distance", d);
                yyjson_mut_arr_append(impacted, item);

                next_frontier.push_back(src_id);
            }
            if (truncated) break;
        }
        frontier = std::move(next_frontier);
    }

    yyjson_mut_obj_add_val(doc.doc, root, "impacted", impacted);

    auto* files_arr = doc.new_arr();
    for (auto& f : impacted_files) {
        yyjson_mut_arr_add_strcpy(doc.doc, files_arr, f.c_str());
    }
    yyjson_mut_obj_add_val(doc.doc, root, "impacted_files", files_arr);
    yyjson_mut_obj_add_bool(doc.doc, root, "truncated", truncated);

    return doc.to_string();
}

// T086: file_deps — include relationships for a file
inline std::string file_deps(yyjson_val* params, Connection& conn,
                              QueryCache& cache, const std::string& repo_root) {
    const char* path = params ? json_get_str(params, "path") : nullptr;
    if (!path) return McpError::invalid_input("Missing 'path'").to_json_rpc(0);

    auto validated = path_util::validate_mcp_path(path, repo_root);
    if (validated.empty()) return McpError::invalid_input("Invalid path").to_json_rpc(0);

    // Find file node
    auto* file_stmt = cache.get("file_deps_file",
        "SELECT n.id FROM nodes n WHERE n.kind = 'file' AND n.name = ?");
    sqlite3_bind_text(file_stmt, 1, path, -1, SQLITE_TRANSIENT);

    int64_t file_node_id = -1;
    if (sqlite3_step(file_stmt) == SQLITE_ROW) {
        file_node_id = sqlite3_column_int64(file_stmt, 0);
    }

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);
    yyjson_mut_obj_add_strcpy(doc.doc, root, "path", path);

    auto* includes = doc.new_arr();
    auto* included_by = doc.new_arr();

    if (file_node_id >= 0) {
        // Files this file includes (outgoing include edges)
        auto* out_stmt = cache.get("file_deps_out",
            "SELECT n.name FROM edges e JOIN nodes n ON e.dst_id = n.id "
            "WHERE e.src_id = ? AND e.kind = 'includes'");
        sqlite3_bind_int64(out_stmt, 1, file_node_id);
        while (sqlite3_step(out_stmt) == SQLITE_ROW) {
            yyjson_mut_arr_add_strcpy(doc.doc, includes,
                reinterpret_cast<const char*>(sqlite3_column_text(out_stmt, 0)));
        }

        // Files that include this file (incoming include edges)
        auto* in_stmt = cache.get("file_deps_in",
            "SELECT n.name FROM edges e JOIN nodes n ON e.src_id = n.id "
            "WHERE e.dst_id = ? AND e.kind = 'includes'");
        sqlite3_bind_int64(in_stmt, 1, file_node_id);
        while (sqlite3_step(in_stmt) == SQLITE_ROW) {
            yyjson_mut_arr_add_strcpy(doc.doc, included_by,
                reinterpret_cast<const char*>(sqlite3_column_text(in_stmt, 0)));
        }
    }

    yyjson_mut_obj_add_val(doc.doc, root, "includes", includes);
    yyjson_mut_obj_add_val(doc.doc, root, "included_by", included_by);
    return doc.to_string();
}

// T087: subgraph — multi-seed BFS extraction
inline std::string subgraph(yyjson_val* params, Connection& conn,
                             QueryCache& cache, const std::string& /*repo_root*/) {
    if (!params) return McpError::invalid_input("Missing parameters").to_json_rpc(0);

    auto* seeds_val = yyjson_obj_get(params, "seed_symbols");
    if (!seeds_val || !yyjson_is_arr(seeds_val))
        return McpError::invalid_input("Missing 'seed_symbols' array").to_json_rpc(0);

    int64_t depth = json_get_int(params, "depth", 1);
    if (depth > 5) depth = 5;
    int64_t max_nodes = json_get_int(params, "max_nodes", 200);
    if (max_nodes > 1000) max_nodes = 1000;

    std::vector<int64_t> frontier;
    yyjson_val* seed_item;
    size_t idx, max;
    yyjson_arr_foreach(seeds_val, idx, max, seed_item) {
        frontier.push_back(yyjson_get_sint(seed_item));
    }

    std::unordered_set<int64_t> visited_nodes(frontier.begin(), frontier.end());

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);
    auto* nodes_arr = doc.new_arr();
    auto* edges_arr = doc.new_arr();
    bool truncated = false;

    // Add seed nodes
    for (int64_t nid : frontier) {
        auto* stmt = cache.get("subgraph_node",
            "SELECT n.id, n.kind, n.name, f.path "
            "FROM nodes n LEFT JOIN files f ON n.file_id = f.id WHERE n.id = ?");
        sqlite3_bind_int64(stmt, 1, nid);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            auto* item = doc.new_obj();
            yyjson_mut_obj_add_int(doc.doc, item, "node_id", sqlite3_column_int64(stmt, 0));
            yyjson_mut_obj_add_strcpy(doc.doc, item, "kind",
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
            yyjson_mut_obj_add_strcpy(doc.doc, item, "name",
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
            auto* fp = sqlite3_column_text(stmt, 3);
            if (fp) yyjson_mut_obj_add_strcpy(doc.doc, item, "file_path", reinterpret_cast<const char*>(fp));
            yyjson_mut_arr_append(nodes_arr, item);
        }
    }

    // BFS
    for (int d = 0; d < depth && !truncated; ++d) {
        std::vector<int64_t> next;
        for (int64_t nid : frontier) {
            // Forward edges
            auto* fwd = cache.get("subgraph_fwd",
                "SELECT dst_id, kind, confidence FROM edges WHERE src_id = ?");
            sqlite3_bind_int64(fwd, 1, nid);
            while (sqlite3_step(fwd) == SQLITE_ROW) {
                int64_t dst = sqlite3_column_int64(fwd, 0);
                // Add edge
                auto* e = doc.new_obj();
                yyjson_mut_obj_add_int(doc.doc, e, "src_id", nid);
                yyjson_mut_obj_add_int(doc.doc, e, "dst_id", dst);
                yyjson_mut_obj_add_strcpy(doc.doc, e, "kind",
                    reinterpret_cast<const char*>(sqlite3_column_text(fwd, 1)));
                yyjson_mut_obj_add_real(doc.doc, e, "confidence", sqlite3_column_double(fwd, 2));
                yyjson_mut_arr_append(edges_arr, e);

                if (!visited_nodes.count(dst)) {
                    if (static_cast<int64_t>(visited_nodes.size()) >= max_nodes) {
                        truncated = true; break;
                    }
                    visited_nodes.insert(dst);
                    next.push_back(dst);

                    // Add node
                    auto* ns = cache.get("subgraph_node",
                        "SELECT n.id, n.kind, n.name, f.path "
                        "FROM nodes n LEFT JOIN files f ON n.file_id = f.id WHERE n.id = ?");
                    sqlite3_bind_int64(ns, 1, dst);
                    if (sqlite3_step(ns) == SQLITE_ROW) {
                        auto* ni = doc.new_obj();
                        yyjson_mut_obj_add_int(doc.doc, ni, "node_id", sqlite3_column_int64(ns, 0));
                        yyjson_mut_obj_add_strcpy(doc.doc, ni, "kind",
                            reinterpret_cast<const char*>(sqlite3_column_text(ns, 1)));
                        yyjson_mut_obj_add_strcpy(doc.doc, ni, "name",
                            reinterpret_cast<const char*>(sqlite3_column_text(ns, 2)));
                        auto* fp = sqlite3_column_text(ns, 3);
                        if (fp) yyjson_mut_obj_add_strcpy(doc.doc, ni, "file_path",
                            reinterpret_cast<const char*>(fp));
                        yyjson_mut_arr_append(nodes_arr, ni);
                    }
                }
            }
            if (truncated) break;
        }
        frontier = std::move(next);
    }

    yyjson_mut_obj_add_val(doc.doc, root, "nodes", nodes_arr);
    yyjson_mut_obj_add_val(doc.doc, root, "edges", edges_arr);
    yyjson_mut_obj_add_bool(doc.doc, root, "truncated", truncated);
    return doc.to_string();
}

// T088: shortest_path — BFS between two nodes
inline std::string shortest_path(yyjson_val* params, Connection& conn,
                                  QueryCache& cache, const std::string& /*repo_root*/) {
    int64_t src_id = params ? json_get_int(params, "src_node_id", -1) : -1;
    int64_t dst_id = params ? json_get_int(params, "dst_node_id", -1) : -1;
    if (src_id < 0 || dst_id < 0)
        return McpError::invalid_input("Missing src_node_id or dst_node_id").to_json_rpc(0);

    int64_t max_depth = params ? json_get_int(params, "max_depth", 10) : 10;
    if (max_depth > 20) max_depth = 20;

    // BFS to find shortest path
    struct BfsNode { int64_t id; int64_t parent; std::string edge_kind; double confidence; };
    std::vector<BfsNode> queue = {{src_id, -1, "", 0}};
    std::unordered_map<int64_t, size_t> visited;  // node_id -> index in queue
    visited[src_id] = 0;
    size_t head = 0;
    bool found = false;

    while (head < queue.size() && !found) {
        auto& current = queue[head];
        if (static_cast<int64_t>(head) > max_depth * 100) break;  // Safety limit

        // Count depth
        int depth = 0;
        int64_t trace = static_cast<int64_t>(head);
        while (trace > 0) { trace = static_cast<int64_t>(queue[trace].parent); depth++; }
        if (depth >= max_depth) { head++; continue; }

        auto* stmt = cache.get("sp_edges",
            "SELECT dst_id, kind, confidence FROM edges WHERE src_id = ? "
            "UNION ALL "
            "SELECT src_id, kind, confidence FROM edges WHERE dst_id = ?");
        sqlite3_bind_int64(stmt, 1, current.id);
        sqlite3_bind_int64(stmt, 2, current.id);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            int64_t next_id = sqlite3_column_int64(stmt, 0);
            if (visited.count(next_id)) continue;

            std::string kind(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
            double conf = sqlite3_column_double(stmt, 2);

            size_t new_idx = queue.size();
            queue.push_back({next_id, static_cast<int64_t>(head), kind, conf});
            visited[next_id] = new_idx;

            if (next_id == dst_id) { found = true; break; }
        }
        head++;
    }

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);
    auto* path_arr = doc.new_arr();

    if (found) {
        // Reconstruct path
        std::vector<size_t> path_indices;
        size_t idx = visited[dst_id];
        while (idx != 0) {
            path_indices.push_back(idx);
            idx = static_cast<size_t>(queue[idx].parent);
        }
        path_indices.push_back(0);
        std::reverse(path_indices.begin(), path_indices.end());

        for (size_t i = 0; i < path_indices.size(); ++i) {
            auto& node = queue[path_indices[i]];

            // Add node
            auto* stmt = cache.get("sp_node",
                "SELECT n.kind, n.name FROM nodes n WHERE n.id = ?");
            sqlite3_bind_int64(stmt, 1, node.id);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                auto* ni = doc.new_obj();
                yyjson_mut_obj_add_int(doc.doc, ni, "node_id", node.id);
                yyjson_mut_obj_add_strcpy(doc.doc, ni, "kind",
                    reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
                yyjson_mut_obj_add_strcpy(doc.doc, ni, "name",
                    reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
                yyjson_mut_arr_append(path_arr, ni);
            }

            // Add edge (except for last)
            if (i + 1 < path_indices.size()) {
                auto& next = queue[path_indices[i + 1]];
                auto* ei = doc.new_obj();
                yyjson_mut_obj_add_strcpy(doc.doc, ei, "edge_kind", next.edge_kind.c_str());
                yyjson_mut_obj_add_real(doc.doc, ei, "confidence", next.confidence);
                yyjson_mut_arr_append(path_arr, ei);
            }
        }
        yyjson_mut_obj_add_int(doc.doc, root, "distance",
            static_cast<int64_t>(path_indices.size() - 1));
    } else {
        yyjson_mut_obj_add_int(doc.doc, root, "distance", 0);
    }

    yyjson_mut_obj_add_val(doc.doc, root, "path", path_arr);
    return doc.to_string();
}

} // namespace tools
} // namespace codetopo
