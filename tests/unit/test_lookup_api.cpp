#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "db/schema.h"
#include "db/queries.h"
#include "db/fts.h"
#include "mcp/tools.h"
#include "util/json.h"
#include "util/path.h"
#include <algorithm>
#include <filesystem>
#include <vector>

using namespace codetopo;

namespace {
bool scans_graph_table(const std::string& detail) {
    for (const char* table : {"nodes", "edges", "n", "e", "sn"}) {
        auto prefix = std::string("SCAN ") + table;
        if (detail == prefix || detail.starts_with(prefix + " ")) return true;
    }
    return false;
}

int collect_lookup_sql(unsigned, void* context, void* statement, void*) {
    auto& queries = *static_cast<std::vector<std::string>*>(context);
    auto* stmt = static_cast<sqlite3_stmt*>(statement);
    const char* sql = sqlite3_sql(stmt);
    if (sql && (std::string(sql).starts_with("SELECT") || std::string(sql).starts_with("WITH"))) {
        char* expanded = sqlite3_expanded_sql(stmt);
        if (expanded) {
            std::string query(expanded);
            if (query.starts_with("SELECT") || query.starts_with("WITH"))
                queries.push_back(std::move(query));
            sqlite3_free(expanded);
        }
    }
    return 0;
}
}

TEST_CASE("Explicit primary scopes exclude extra roots and use bounded production plans", "[lookup-api][contract]") {
    Connection conn(":memory:");
    schema::ensure_schema(conn);
    fts::create_sync_triggers(conn);
    auto root = std::filesystem::current_path();
    const std::vector<std::string> extra_spellings = {
        (root / ".lookup-extra").string(),
        (root / ".lookup-extra").generic_string(),
        root.string() + "/.lookup-extra"
    };
    auto extra = extra_spellings[GENERATE(0, 1, 2)];
    auto extra_file = (GENERATE(false, true) ? path_util::lookup_path(extra) : extra) + "/src/test.cpp";
    INFO("Registered root: " << extra << "; stored file: " << extra_file);
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(conn.raw(), "INSERT INTO roots(id,path,added_at) VALUES(1,?,'now')", -1, &stmt, nullptr) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, extra.c_str(), -1, SQLITE_TRANSIENT);
    REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    conn.exec("INSERT INTO files(id,path,language,size_bytes,mtime_ns,content_hash,parse_status) "
              "VALUES(1,'top.cpp','cpp',1,1,'x','ok'),"
              "(2,'src/test.cpp','cpp',1,1,'x','ok'),"
              "(3,'src/deep/test.cpp','cpp',1,1,'x','ok')");
    REQUIRE(sqlite3_prepare_v2(conn.raw(),
        "INSERT INTO files(id,path,root_id,language,size_bytes,mtime_ns,content_hash,parse_status) "
        "VALUES(4,?,1,'cpp',1,1,'x','ok')", -1, &stmt, nullptr) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, extra_file.c_str(), -1, SQLITE_TRANSIENT);
    REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    conn.exec("INSERT INTO nodes(id,node_type,file_id,kind,name,qualname,start_line,end_line,stable_key) "
              "VALUES(1,'symbol',1,'function','target','top::target',1,1,'top'),"
              "(2,'symbol',2,'function','target','src::target',1,1,'src'),"
              "(3,'symbol',3,'function','target','deep::target',1,1,'deep'),"
              "(4,'symbol',4,'function','target','extra::target',1,1,'extra'),"
              "(5,'symbol',1,'function','main','main',2,2,'main')");
    conn.exec("INSERT INTO edges(src_id,dst_id,kind,confidence) VALUES(5,2,'calls',1),(4,4,'calls',1)");
    conn.exec("INSERT INTO refs(file_id,kind,name,start_line,start_col,end_line,end_col,containing_node_id) "
              "VALUES(2,'http_call','/primary',1,1,1,2,2),(4,'http_call','/extra',1,1,1,2,4)");
    QueryCache cache(conn);
    std::vector<std::string> queries;
    auto invoke = [&](auto handler, const std::string& path, bool pattern, bool recursive = true,
                      const char* query = "*") {
        JsonMutDoc args;
        auto* params = args.new_obj();
        args.set_root(params);
        const char* key = pattern ? (handler == tools::file_search ? "pattern" : "file_pattern") : "path";
        yyjson_mut_obj_add_strcpy(args.doc, params, key, path.c_str());
        yyjson_mut_obj_add_strcpy(args.doc, params, "query", query);
        yyjson_mut_obj_add_str(args.doc, params, "name", "target");
        yyjson_mut_obj_add_bool(args.doc, params, "recursive", recursive);
        yyjson_mut_obj_add_str(args.doc, params, "kind", "function");
        yyjson_mut_obj_add_int(args.doc, params, "min_span_lines", 1);
        auto parsed = json_parse(args.to_string());
        auto result = json_parse(handler(parsed.root(), conn, cache, root.string()));
        REQUIRE(result);
        REQUIRE_FALSE(yyjson_obj_get(result.root(), "error"));
        cache.reset_all();
        return result;
    };
    // "." remains workspace-wide, but is deliberately not part of scoped plan assertions.
    REQUIRE(json_get_int(invoke(tools::symbols_in_path, ".", false).root(), "total_candidates") == 5);
    sqlite3_trace_v2(conn.raw(), SQLITE_TRACE_STMT, collect_lookup_sql, &queries);
    for (const auto& primary : {root.string(), root.generic_string()}) {
        auto recursive = invoke(tools::symbols_in_path, primary, false);
        REQUIRE(json_get_int(recursive.root(), "total") == 4);
        REQUIRE(json_get_int(recursive.root(), "total_candidates") == 4);
        REQUIRE(json_get_int(invoke(tools::symbols_in_path, primary, false, false).root(), "total") == 2);
        REQUIRE(json_get_int(invoke(tools::dir_tree, primary, false).root(), "file_count") == 3);
        REQUIRE(json_get_int(invoke(tools::dir_list, primary, false).root(), "total") == 2);
        REQUIRE(json_get_int(invoke(tools::file_search, primary + "/test.cpp", true).root(), "total") == 0);
        REQUIRE(json_get_int(invoke(tools::file_search, primary + "/te*.cpp", true).root(), "total") == 0);
        REQUIRE(json_get_int(invoke(tools::file_search, primary + "/t*.cpp", true).root(), "total") == 1);
        for (const auto& suffix : {"*", "**", "src/*", "src/**"}) {
            auto pattern = primary + "/" + suffix;
            const int expected = std::string(suffix).starts_with("src/") ? 2 : 3;
            auto files = invoke(tools::file_search, pattern, true);
            REQUIRE(json_get_int(files.root(), "total") == expected);
            auto symbols = invoke(tools::symbol_search, pattern, true);
            REQUIRE(json_get_int(symbols.root(), "total") == expected + (expected == 3 ? 1 : 0));
            REQUIRE(json_get_int(invoke(tools::symbol_search, pattern, true, true, "target").root(), "total") == expected);
            auto http = invoke(tools::list_http_calls, pattern, true);
            REQUIRE(json_get_int(http.root(), "total") == 1);
            auto context = invoke(tools::context_by_name, pattern, true);
            REQUIRE(yyjson_arr_size(yyjson_obj_get(context.root(), "candidates")) == expected);
#ifdef _WIN32
            std::replace(pattern.begin(), pattern.end(), '/', '\\');
            REQUIRE(json_get_int(invoke(tools::file_search, pattern, true).root(), "total") == expected);
            REQUIRE(json_get_int(invoke(tools::symbol_search, pattern, true).root(), "total") == expected + (expected == 3 ? 1 : 0));
#endif
        }
        JsonMutDoc args;
        auto* params = args.new_obj();
        args.set_root(params);
        yyjson_mut_obj_add_strcpy(args.doc, params, "scope", primary.c_str());
        auto parsed = json_parse(args.to_string());
        auto entries = json_parse(tools::entrypoints(parsed.root(), conn, cache, root.string()));
        REQUIRE_FALSE(yyjson_obj_get(entries.root(), "error"));
        REQUIRE(yyjson_arr_size(yyjson_obj_get(entries.root(), "results")) == 2);
        cache.reset_all();
    }
    // A registered extra root remains addressable even when nested under the primary root.
    for (const auto& registered : extra_spellings) {
        for (const auto& suffix : {"/*", "/**", "/src/*", "/src/**"}) {
            REQUIRE(json_get_int(invoke(tools::file_search, registered + suffix, true).root(), "total") == 1);
            REQUIRE(json_get_int(invoke(tools::symbol_search, registered + suffix, true).root(), "total") == 1);
#ifdef _WIN32
            auto pattern = registered + suffix;
            std::replace(pattern.begin(), pattern.end(), '/', '\\');
            REQUIRE(json_get_int(invoke(tools::file_search, pattern, true).root(), "total") == 1);
            REQUIRE(json_get_int(invoke(tools::symbol_search, pattern, true).root(), "total") == 1);
#endif
        }
        REQUIRE(json_get_int(invoke(tools::file_search, registered + "-other/*", true).root(), "total") == 0);
        REQUIRE(json_get_int(invoke(tools::symbol_search, registered + "-other/*", true).root(), "total") == 0);
    }
    sqlite3_trace_v2(conn.raw(), 0, nullptr, nullptr);
    REQUIRE_FALSE(queries.empty());
    bool saw_primary_files_index = false;
    bool saw_file_bounded_nodes = false;
    bool saw_bounded_edges = false;
    for (const auto& query : queries) {
        INFO(query);
        auto explain = "EXPLAIN QUERY PLAN " + query;
        REQUIRE(sqlite3_prepare_v2(conn.raw(), explain.c_str(), -1, &stmt, nullptr) == SQLITE_OK);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string detail(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)));
            INFO(detail);
            REQUIRE_FALSE(scans_graph_table(detail));
            saw_primary_files_index |= detail.find("idx_files_root") != std::string::npos;
            saw_file_bounded_nodes |= detail.find("SEARCH n USING INDEX idx_nodes_file_id (file_id=?)") != std::string::npos;
            saw_bounded_edges |= detail.find("SEARCH e USING COVERING INDEX idx_edges_dst (dst_id=?)") != std::string::npos;
        }
        sqlite3_finalize(stmt);
    }
    REQUIRE(saw_primary_files_index);
    REQUIRE(saw_file_bounded_nodes);
    REQUIRE(saw_bounded_edges);
}

TEST_CASE("Scoped lookup production queries remain index-driven without ANALYZE", "[lookup-api][contract]") {
    Connection conn(":memory:");
    schema::ensure_schema(conn);
    conn.exec("INSERT INTO files(id,path,language,size_bytes,mtime_ns,content_hash,parse_status) "
              "VALUES(1,'src/test.cpp','cpp',1,1,'x','ok')");
    conn.exec("INSERT INTO nodes(id,node_type,file_id,kind,name,qualname,start_line,end_line,stable_key) "
              "VALUES(1,'symbol',1,'function','target','ns::target',1,1,'target'),"
              "(2,'symbol',1,'function','caller','caller',2,2,'caller')");
    conn.exec("INSERT INTO edges(src_id,dst_id,kind,confidence) VALUES(2,1,'calls',1)");
    QueryCache cache(conn);
    auto root = std::filesystem::current_path().string();
    std::vector<std::string> queries;
    sqlite3_trace_v2(conn.raw(), SQLITE_TRACE_STMT, collect_lookup_sql, &queries);
    auto call = [&](auto handler, const char* arguments) {
        auto params = json_parse(arguments);
        auto result = json_parse(handler(params.root(), conn, cache, root));
        REQUIRE(result);
        REQUIRE_FALSE(yyjson_obj_get(result.root(), "error"));
        cache.reset_all();
    };
    call(tools::context_for, R"({"symbol":"ns::target","file":"src/test.cpp","include_source":false,"include_candidates":false})");
    call(tools::symbol_list, R"({"file_path":"src/test.cpp","kind":"function","min_span_lines":1})");
    call(tools::symbols_in_path, R"({"path":"src","kind":["function"],"min_span_lines":1})");
    call(tools::symbol_search, R"({"query":"*","file_pattern":"src/*","kind":"function"})");
    call(tools::context_by_name, R"({"name":"target","file_pattern":"src/*"})");
    sqlite3_trace_v2(conn.raw(), 0, nullptr, nullptr);
    REQUIRE_FALSE(queries.empty());
    bool saw_selector = false;
    bool saw_files_equality = false;
    for (const auto& query : queries) {
        saw_selector |= query.find("resolve_node") != std::string::npos ||
                        query.find("n.name = 'ns::target'") != std::string::npos;
        saw_files_equality |= query.find("SELECT path FROM files WHERE path = ") != std::string::npos;
        sqlite3_stmt* stmt = nullptr;
        auto explain = "EXPLAIN QUERY PLAN " + query;
        REQUIRE(sqlite3_prepare_v2(conn.raw(), explain.c_str(), -1, &stmt, nullptr) == SQLITE_OK);
        std::vector<std::string> details;
        while (sqlite3_step(stmt) == SQLITE_ROW)
            details.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)));
        sqlite3_finalize(stmt);
        INFO(query);
        for (const auto& detail : details) {
            INFO(detail);
            REQUIRE_FALSE(scans_graph_table(detail));
            if (query.find("SELECT path FROM files WHERE path = ") != std::string::npos)
                REQUIRE_FALSE(detail.starts_with("SCAN files"));
        }
    }
    REQUIRE(saw_selector);
    REQUIRE(saw_files_equality);
}

TEST_CASE("Shared selectors prefer definitions and reject invalid inputs", "[lookup-api][contract]") {
    Connection conn(":memory:");
    schema::ensure_schema(conn);
    conn.exec("INSERT INTO files(id,path,language,size_bytes,mtime_ns,content_hash,parse_status) "
              "VALUES(1,'src/test.cpp','cpp',1,1,'x','ok')");
    conn.exec("INSERT INTO nodes(id,node_type,file_id,kind,name,qualname,is_definition,stable_key) "
              "VALUES(1,'symbol',1,'function','target','ns::target',0,'decl'),"
              "(2,'symbol',1,'function','target','ns::target',1,'def')");
    QueryCache cache(conn);
    auto root = std::filesystem::current_path().string();
    auto invoke = [&](const std::string& arguments) {
        auto params = json_parse(arguments);
        auto result = tools::context_for(params.root(), conn, cache, root);
        cache.reset_all();
        return result;
    };
    auto definition = json_parse(invoke(R"({"symbol":"target","file":"src/test.cpp","include_source":false})"));
    REQUIRE(json_get_int(yyjson_obj_get(definition.root(), "symbol"), "node_id") == 2);
    auto qualified = json_parse(invoke(R"({"symbol":"ns::target","file":"src/test.cpp","include_source":false})"));
    REQUIRE(json_get_int(yyjson_obj_get(qualified.root(), "symbol"), "node_id") == 2);
    conn.exec("INSERT INTO nodes(id,node_type,file_id,kind,name,qualname,is_definition,stable_key) "
              "VALUES(3,'symbol',1,'function','target','other::target',1,'other-def')");
    auto ambiguous = json_parse(invoke(R"({"symbol":"target","file":"src/test.cpp"})"));
    REQUIRE(json_get_bool(ambiguous.root(), "ambiguous"));
    REQUIRE(yyjson_arr_size(yyjson_obj_get(ambiguous.root(), "candidates")) == 2);
    auto selected = json_parse(invoke(R"({"symbol":"ns::target","file":"src/test.cpp","include_source":false})"));
    REQUIRE(json_get_int(yyjson_obj_get(selected.root(), "symbol"), "node_id") == 2);
    auto missing = json_parse(invoke(R"({"symbol":"missing","file":"src/test.cpp"})"));
    auto* data = yyjson_obj_get(yyjson_obj_get(missing.root(), "error"), "data");
    REQUIRE(std::string(json_get_str(data, "error_code")) == "not_found");
}
