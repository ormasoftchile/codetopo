#include <catch2/catch_test_macros.hpp>
#include "db/connection.h"
#include "db/fts.h"
#include "db/queries.h"
#include "db/schema.h"
#include "mcp/tools.h"
#include "util/json.h"
#include <filesystem>
#include <fstream>
#include <sqlite3.h>
#include <string>

namespace fs = std::filesystem;
using namespace codetopo;

namespace {

fs::path make_workspace_dir(const std::string& name) {
    auto dir = fs::current_path() / (".codetopo_test_" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

void cleanup_workspace_dir(const fs::path& dir) {
    std::error_code ec;
    fs::remove_all(dir, ec);
}

int64_t insert_file(Connection& conn, const char* path) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(conn.raw(),
        "INSERT INTO files(path, language, size_bytes, mtime_ns, content_hash, parse_status) "
        "VALUES(?, 'typescript', 100, 1000, ?, 'ok')",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, path, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return sqlite3_last_insert_rowid(conn.raw());
}

int64_t insert_symbol(Connection& conn, int64_t file_id, const char* name,
                      const char* stable_key, int start_line = 1) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(conn.raw(),
        "INSERT INTO nodes(node_type, file_id, kind, name, qualname, start_line, end_line, stable_key) "
        "VALUES('symbol', ?, 'method', ?, ?, ?, ?, ?)",
        -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, file_id);
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, start_line);
    sqlite3_bind_int(stmt, 5, start_line);
    sqlite3_bind_text(stmt, 6, stable_key, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return sqlite3_last_insert_rowid(conn.raw());
}

} // namespace

TEST_CASE("node_id expected_stable_key mismatch returns invalid_input", "[unit][mcp]") {
    auto dir = make_workspace_dir("stale_node_id");
    auto db_path = dir / "db.sqlite";

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);
        auto file_id = insert_file(conn, "src/map.ts");
        auto node_id = insert_symbol(conn, file_id, "accumulate", "src/map.ts::method::accumulate");
        insert_symbol(conn, file_id, "set", "src/map.ts::method::LinkedMap.set", 2);

        QueryCache cache(conn);
        auto params_doc = json_parse(
            "{\"node_id\":" + std::to_string(node_id) +
            ",\"expected_stable_key\":\"src/map.ts::method::LinkedMap.set\"}");
        auto result = tools::impact_of(params_doc.root(), conn, cache, dir.string());
        auto doc = json_parse(result);
        REQUIRE(doc);
        auto* err = yyjson_obj_get(doc.root(), "error");
        REQUIRE(err);
        REQUIRE(std::string(yyjson_get_str(yyjson_obj_get(
            yyjson_obj_get(err, "data"), "error_code"))) == "invalid_input");
        auto* message = yyjson_obj_get(err, "message");
        REQUIRE(message);
        std::string text = yyjson_get_str(message);
        REQUIRE(text.find("stale/reused") != std::string::npos);
        REQUIRE(text.find("src/map.ts::method::LinkedMap.set") != std::string::npos);
        REQUIRE(text.find("src/map.ts::method::accumulate") != std::string::npos);
    }

    cleanup_workspace_dir(dir);
}

TEST_CASE("symbol_search FTS applies file_pattern and total is post-filter count", "[unit][mcp]") {
    auto dir = make_workspace_dir("symbol_search_filter");
    auto db_path = dir / "db.sqlite";

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);
        auto map_file = insert_file(conn, "src/map.ts");
        auto event_file = insert_file(conn, "src/event.ts");
        insert_symbol(conn, map_file, "set", "src/map.ts::method::LinkedMap.set");
        insert_symbol(conn, event_file, "set", "src/event.ts::function::set");
        insert_symbol(conn, event_file, "setup", "src/event.ts::function::setup");
        fts::rebuild(conn);

        QueryCache cache(conn);
        auto params_doc = json_parse(
            R"({"query":"set","file_pattern":"src/map.ts","limit":10})");
        auto result = tools::symbol_search(params_doc.root(), conn, cache, dir.string());
        auto doc = json_parse(result);
        REQUIRE(doc);
        auto* root = doc.root();
        auto* results = yyjson_obj_get(root, "results");
        REQUIRE(results);
        REQUIRE(yyjson_arr_size(results) == 1);
        REQUIRE(yyjson_get_sint(yyjson_obj_get(root, "total")) == 1);
        auto* first = yyjson_arr_get_first(results);
        REQUIRE(std::string(yyjson_get_str(yyjson_obj_get(first, "file_path"))) == "src/map.ts");
        REQUIRE(std::string(yyjson_get_str(yyjson_obj_get(first, "stable_key"))) ==
                "src/map.ts::method::LinkedMap.set");
    }

    cleanup_workspace_dir(dir);
}

TEST_CASE("symbol_search multi-term query defaults to any-term matching", "[unit][mcp]") {
    auto dir = make_workspace_dir("symbol_search_any_terms");
    auto db_path = dir / "db.sqlite";

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);
        auto file_id = insert_file(conn, "src/collections.ts");
        insert_symbol(conn, file_id, "TernarySearchTree", "src/collections.ts::class::TernarySearchTree");
        insert_symbol(conn, file_id, "ResourceTree", "src/collections.ts::class::ResourceTree", 2);
        insert_symbol(conn, file_id, "LRUCache", "src/collections.ts::class::LRUCache", 3);
        fts::rebuild(conn);

        QueryCache cache(conn);
        auto params_doc = json_parse(
            R"({"query":"TernarySearchTree ResourceTree LRUCache","limit":10})");
        auto result = tools::symbol_search(params_doc.root(), conn, cache, dir.string());
        auto doc = json_parse(result);
        REQUIRE(doc);
        auto* root = doc.root();
        CHECK(std::string(json_get_str(root, "match")) == "any");
        auto* results = yyjson_obj_get(root, "results");
        REQUIRE(results);
        REQUIRE(yyjson_arr_size(results) == 3);

        bool found_ternary = false;
        bool found_resource = false;
        bool found_lru = false;
        yyjson_val* item = nullptr;
        yyjson_arr_iter iter;
        yyjson_arr_iter_init(results, &iter);
        while ((item = yyjson_arr_iter_next(&iter))) {
            const char* name = json_get_str(item, "name");
            if (!name) continue;
            found_ternary = found_ternary || std::string(name) == "TernarySearchTree";
            found_resource = found_resource || std::string(name) == "ResourceTree";
            found_lru = found_lru || std::string(name) == "LRUCache";
        }
        CHECK(found_ternary);
        CHECK(found_resource);
        CHECK(found_lru);
    }

    cleanup_workspace_dir(dir);
}
