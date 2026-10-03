#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "db/connection.h"
#include "db/schema.h"
#include "index/pagerank.h"
#include "mcp/tools.h"
#include "util/json.h"
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace codetopo;

namespace {

struct TempDb {
    fs::path dir;
    fs::path path;

    TempDb(const std::string& name) {
        dir = fs::temp_directory_path() / ("codetopo_test_pr_" + name + "_" + std::to_string(std::time(nullptr)));
        fs::create_directories(dir);
        path = dir / "test.db";
    }

    ~TempDb() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

} // namespace

TEST_CASE("PageRank empty graph handles cleanly", "[unit][pagerank]") {
    TempDb temp("empty");
    Connection conn(temp.path);
    schema::ensure_schema(conn);

    int ranked = compute_and_persist_pagerank(conn);
    REQUIRE(ranked == 0);
}

TEST_CASE("PageRank computes centrality ordering on call graph", "[unit][pagerank]") {
    TempDb temp("centrality");
    Connection conn(temp.path);
    schema::ensure_schema(conn);

    conn.exec("INSERT INTO files(id, path, language, size_bytes, mtime_ns, content_hash, parse_status) "
              "VALUES(1, 'src/main.cpp', 'cpp', 100, 1, 'h1', 'ok')");
    conn.exec("INSERT INTO files(id, path, language, size_bytes, mtime_ns, content_hash, parse_status) "
              "VALUES(2, 'src/core.cpp', 'cpp', 200, 1, 'h2', 'ok')");

    // Symbol 1: entrypoint (caller of 2 and 3)
    // Symbol 2: mid helper (called by 1, calls 3)
    // Symbol 3: core engine (called by 1 and 2)
    // Symbol 4: isolated leaf (no edges)
    conn.exec("INSERT INTO nodes(id, node_type, file_id, kind, name, qualname, stable_key) "
              "VALUES(1, 'symbol', 1, 'function', 'main', 'main', 'src/main.cpp::function::main')");
    conn.exec("INSERT INTO nodes(id, node_type, file_id, kind, name, qualname, stable_key) "
              "VALUES(2, 'symbol', 2, 'function', 'helper', 'helper', 'src/core.cpp::function::helper')");
    conn.exec("INSERT INTO nodes(id, node_type, file_id, kind, name, qualname, stable_key) "
              "VALUES(3, 'symbol', 2, 'function', 'engine', 'engine', 'src/core.cpp::function::engine')");
    conn.exec("INSERT INTO nodes(id, node_type, file_id, kind, name, qualname, stable_key) "
              "VALUES(4, 'symbol', 2, 'function', 'isolated', 'isolated', 'src/core.cpp::function::isolated')");

    // Edges: 1 -> 2, 1 -> 3, 2 -> 3
    conn.exec("INSERT INTO edges(src_id, dst_id, kind, confidence) VALUES(1, 2, 'calls', 1.0)");
    conn.exec("INSERT INTO edges(src_id, dst_id, kind, confidence) VALUES(1, 3, 'calls', 1.0)");
    conn.exec("INSERT INTO edges(src_id, dst_id, kind, confidence) VALUES(2, 3, 'calls', 1.0)");

    int ranked = compute_and_persist_pagerank(conn);
    REQUIRE(ranked == 3);

    // Query ranks
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(conn.raw(), "SELECT id, rank FROM nodes ORDER BY id", -1, &stmt, nullptr);

    double r1 = 0.0, r2 = 0.0, r3 = 0.0, r4 = 0.0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(stmt, 0);
        double r = sqlite3_column_double(stmt, 1);
        if (id == 1) r1 = r;
        if (id == 2) r2 = r;
        if (id == 3) r3 = r;
        if (id == 4) r4 = r;
    }
    sqlite3_finalize(stmt);

    // 3 has the most incoming callers (highest centrality), normalized to 1.0
    REQUIRE(r3 == Catch::Approx(1.0));
    // 2 has 1 incoming caller
    REQUIRE(r2 > r1);
    // 1 has 0 incoming callers but participates in graph
    REQUIRE(r1 > 0.0);
    // 4 has no edges, rank stays default 0.0
    REQUIRE(r4 == 0.0);
}

TEST_CASE("symbol_search ranks higher-centrality symbols first", "[unit][pagerank][symbol_search]") {
    TempDb temp("search_rank");
    Connection conn(temp.path);
    schema::ensure_schema(conn);
    QueryCache cache(conn);

    conn.exec("INSERT INTO files(id, path, language, size_bytes, mtime_ns, content_hash, parse_status) "
              "VALUES(1, 'src/parse.cpp', 'cpp', 100, 1, 'h1', 'ok')");

    // Two functions matching 'parse':
    // parse_core: central (called by 10 callers)
    // parse_leaf: leaf (called by 0 callers)
    conn.exec("INSERT INTO nodes(id, node_type, file_id, kind, name, qualname, stable_key) "
              "VALUES(10, 'symbol', 1, 'function', 'parse_core', 'parse_core', 'src/parse.cpp::function::parse_core')");
    conn.exec("INSERT INTO nodes(id, node_type, file_id, kind, name, qualname, stable_key) "
              "VALUES(20, 'symbol', 1, 'function', 'parse_leaf', 'parse_leaf', 'src/parse.cpp::function::parse_leaf')");

    // Add callers for parse_core
    for (int i = 1; i <= 5; ++i) {
        std::string name = "caller_" + std::to_string(i);
        conn.exec("INSERT INTO nodes(id, node_type, file_id, kind, name, qualname, stable_key) "
                  "VALUES(" + std::to_string(100 + i) + ", 'symbol', 1, 'function', '" + name + "', '" + name + "', 'src/parse.cpp::function::" + name + "')");
        conn.exec("INSERT INTO edges(src_id, dst_id, kind, confidence) "
                  "VALUES(" + std::to_string(100 + i) + ", 10, 'calls', 1.0)");
    }

    // Insert into FTS
    conn.exec("INSERT INTO nodes_fts(rowid, name, qualname, signature, doc) VALUES(10, 'parse_core parse core', 'parse_core', '', '')");
    conn.exec("INSERT INTO nodes_fts(rowid, name, qualname, signature, doc) VALUES(20, 'parse_leaf parse leaf', 'parse_leaf', '', '')");

    compute_and_persist_pagerank(conn);

    // Run symbol_search for 'parse'
    auto params = json_parse(R"({"query": "parse"})");
    std::string response = tools::symbol_search(params.root(), conn, cache, temp.dir.string());

    auto doc = json_parse(response);
    REQUIRE(doc);
    auto* results = yyjson_obj_get(doc.root(), "results");
    REQUIRE(results);
    REQUIRE(yyjson_is_arr(results));
    REQUIRE(yyjson_arr_size(results) >= 2);

    auto* first = yyjson_arr_get(results, 0);
    auto* first_name = yyjson_obj_get(first, "name");
    REQUIRE(first_name);
    CHECK(std::string(yyjson_get_str(first_name)) == "parse_core");

    auto* first_rank = yyjson_obj_get(first, "rank");
    REQUIRE(first_rank);
    CHECK(yyjson_get_real(first_rank) > 0.0);
}
