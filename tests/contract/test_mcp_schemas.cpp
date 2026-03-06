// T080: Contract test — verify tool responses match expected JSON schema.
#include <catch2/catch_test_macros.hpp>
#include "db/connection.h"
#include "db/schema.h"
#include "db/queries.h"
#include "mcp/tools.h"
#include "util/json.h"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace codetopo;

static fs::path create_contract_db() {
    auto tmp = fs::temp_directory_path() / "codetopo_contract_test";
    fs::create_directories(tmp);
    auto db_path = tmp / "contract.sqlite";
    fs::remove(db_path);

    Connection conn(db_path);
    schema::ensure_schema(conn);
    conn.exec("INSERT INTO files(path, language, size_bytes, mtime_ns, content_hash, parse_status) "
              "VALUES('test.cpp', 'cpp', 100, 1000, 'abc', 'ok')");
    int64_t fid = sqlite3_last_insert_rowid(conn.raw());
    conn.exec("INSERT INTO nodes(node_type, file_id, kind, name, stable_key) "
              "VALUES('file', NULL, 'file', 'test.cpp', 'test.cpp::file')");
    {
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(conn.raw(),
            "INSERT INTO nodes(node_type, file_id, kind, name, qualname, start_line, end_line, stable_key) "
            "VALUES('symbol', ?, 'function', 'foo', 'foo', 1, 5, 'test.cpp::function::foo')", -1, &s, nullptr);
        sqlite3_bind_int64(s, 1, fid);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
    schema::set_kv(conn, "schema_version", "1");
    schema::set_kv(conn, "indexer_version", "1.0.0");
    schema::set_kv(conn, "repo_root", tmp.string());
    schema::set_kv(conn, "last_index_time", "2026-03-04T12:00:00Z");
    conn.exec("INSERT INTO nodes_fts(nodes_fts) VALUES('rebuild')");

    return db_path;
}

TEST_CASE("repo_stats response has required fields", "[contract][us2]") {
    auto db_path = create_contract_db();
    {
        Connection conn(db_path, true);
        QueryCache cache(conn);
        auto root = schema::get_kv(const_cast<Connection&>(conn), "repo_root");

        auto result = tools::repo_stats(nullptr, const_cast<Connection&>(conn), cache, root);
        auto doc = json_parse(result);
        REQUIRE(doc);
        auto* r = doc.root();

        REQUIRE(yyjson_obj_get(r, "file_count") != nullptr);
        REQUIRE(yyjson_obj_get(r, "symbol_count") != nullptr);
        REQUIRE(yyjson_obj_get(r, "edge_count") != nullptr);
        REQUIRE(yyjson_obj_get(r, "last_index_time") != nullptr);
        REQUIRE(yyjson_obj_get(r, "indexer_version") != nullptr);
    }
    fs::remove_all(db_path.parent_path());
}

TEST_CASE("server_info response has required fields", "[contract][us2]") {
    auto db_path = create_contract_db();
    {
        Connection conn(db_path, true);
        QueryCache cache(conn);
        auto root = schema::get_kv(const_cast<Connection&>(conn), "repo_root");

        auto result = tools::server_info(nullptr, const_cast<Connection&>(conn), cache, root);
        auto doc = json_parse(result);
        REQUIRE(doc);
        auto* r = doc.root();

        REQUIRE(yyjson_obj_get(r, "protocol_version") != nullptr);
        REQUIRE(yyjson_obj_get(r, "schema_version") != nullptr);
        REQUIRE(yyjson_obj_get(r, "indexer_version") != nullptr);
        REQUIRE(yyjson_obj_get(r, "capabilities") != nullptr);
        REQUIRE(yyjson_obj_get(r, "db_status") != nullptr);
    }
    fs::remove_all(db_path.parent_path());
}

TEST_CASE("symbol_search response has results array and has_more", "[contract][us2]") {
    auto db_path = create_contract_db();
    {
        Connection conn(db_path, true);
        QueryCache cache(conn);
        auto root = schema::get_kv(const_cast<Connection&>(conn), "repo_root");

        auto params_str = R"({"query": "foo"})";
        auto params_doc = json_parse(params_str);
        auto result = tools::symbol_search(params_doc.root(), const_cast<Connection&>(conn), cache, root);
        auto doc = json_parse(result);
        REQUIRE(doc);
        auto* r = doc.root();

        REQUIRE(yyjson_obj_get(r, "results") != nullptr);
        REQUIRE(yyjson_is_arr(yyjson_obj_get(r, "results")));
        REQUIRE(yyjson_obj_get(r, "has_more") != nullptr);
    }
    fs::remove_all(db_path.parent_path());
}
