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

    // Also verify entrypoints tool returns parse_core with rank
    std::string ep_response = tools::entrypoints(nullptr, conn, cache, temp.dir.string());
    auto ep_doc = json_parse(ep_response);
    REQUIRE(ep_doc);
    auto* ep_results = yyjson_obj_get(ep_doc.root(), "results");
    REQUIRE(ep_results);
    REQUIRE(yyjson_arr_size(ep_results) >= 1);

    auto* ep_first = yyjson_arr_get(ep_results, 0);
    auto* ep_name = yyjson_obj_get(ep_first, "name");
    REQUIRE(ep_name);
    CHECK(std::string(yyjson_get_str(ep_name)) == "parse_core");
    auto* ep_rank = yyjson_obj_get(ep_first, "rank");
    REQUIRE(ep_rank);
    CHECK(yyjson_get_real(ep_rank) > 0.0);

    // Verify symbol_get returns rank
    int64_t parse_core_id = yyjson_get_sint(yyjson_obj_get(first, "node_id"));
    auto get_params = json_parse("{\"node_id\":" + std::to_string(parse_core_id) + "}");
    std::string get_resp = tools::symbol_get(get_params.root(), conn, cache, temp.dir.string());
    auto get_doc = json_parse(get_resp);
    REQUIRE(get_doc);
    auto* get_rank = yyjson_obj_get(get_doc.root(), "rank");
    REQUIRE(get_rank);
    CHECK(yyjson_get_real(get_rank) > 0.0);

    // Verify symbol_get_batch returns rank
    auto batch_params = json_parse("{\"node_ids\":[" + std::to_string(parse_core_id) + "]}");
    std::string batch_resp = tools::symbol_get_batch(batch_params.root(), conn, cache, temp.dir.string());
    auto batch_doc = json_parse(batch_resp);
    REQUIRE(batch_doc);
    auto* batch_results = yyjson_obj_get(batch_doc.root(), "results");
    REQUIRE(batch_results);
    REQUIRE(yyjson_arr_size(batch_results) == 1);
    auto* batch_first = yyjson_arr_get(batch_results, 0);
    auto* batch_rank = yyjson_obj_get(batch_first, "rank");
    REQUIRE(batch_rank);
    CHECK(yyjson_get_real(batch_rank) > 0.0);

    // Verify context_for returns rank on symbol
    auto ctx_params = json_parse("{\"node_id\":" + std::to_string(parse_core_id) + "}");
    std::string ctx_resp = tools::context_for(ctx_params.root(), conn, cache, temp.dir.string());
    auto ctx_doc = json_parse(ctx_resp);
    REQUIRE(ctx_doc);
    auto* ctx_symbol = yyjson_obj_get(ctx_doc.root(), "symbol");
    REQUIRE(ctx_symbol);
    auto* ctx_rank = yyjson_obj_get(ctx_symbol, "rank");
    REQUIRE(ctx_rank);
    CHECK(yyjson_get_real(ctx_rank) > 0.0);

    // Verify get_architecture hotspots expose rank
    std::string arch_resp = tools::get_architecture(nullptr, conn, cache, temp.dir.string());
    auto arch_doc = json_parse(arch_resp);
    REQUIRE(arch_doc);
    auto* arch_hotspots = yyjson_obj_get(arch_doc.root(), "hotspots");
    REQUIRE(arch_hotspots);
    if (yyjson_arr_size(arch_hotspots) > 0) {
        auto* arch_first = yyjson_arr_get(arch_hotspots, 0);
        auto* arch_rank = yyjson_obj_get(arch_first, "rank");
        REQUIRE(arch_rank);
        CHECK(yyjson_get_real(arch_rank) > 0.0);
    }

    // Verify impact_of returns rank on symbol
    std::string impact_resp = tools::impact_of(ctx_params.root(), conn, cache, temp.dir.string());
    auto impact_doc = json_parse(impact_resp);
    REQUIRE(impact_doc);
    auto* impact_symbol = yyjson_obj_get(impact_doc.root(), "symbol");
    REQUIRE(impact_symbol);
    auto* impact_rank = yyjson_obj_get(impact_symbol, "rank");
    REQUIRE(impact_rank);
    CHECK(yyjson_get_real(impact_rank) > 0.0);
}
