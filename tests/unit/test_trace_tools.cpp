#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "db/connection.h"
#include "db/schema.h"
#include "db/queries.h"
#include "mcp/tools.h"
#include "util/json.h"
#include <filesystem>

using namespace codetopo;
namespace fs = std::filesystem;

static fs::path trace_test_dir() {
    auto base = fs::current_path() / ".codetopo-trace-tool-test";
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base);
    return base;
}

TEST_CASE("ingest_traces stores runtime traces and boosts matching call edges",
          "[unit][mcp][traces]") {
    auto base = trace_test_dir();
    auto db_path = base / "index.sqlite";

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);
        conn.exec("INSERT INTO files(id, path, language, size_bytes, mtime_ns, content_hash, parse_status) "
                  "VALUES(1, 'src/http.cs', 'csharp', 100, 1000, 'hash', 'ok')");
        conn.exec("INSERT INTO nodes(id, node_type, file_id, kind, name, qualname, is_definition, stable_key) "
                  "VALUES(10, 'symbol', 1, 'method', 'SendAsync', 'HttpClient.SendAsync', 1, 'sym:send')");
        conn.exec("INSERT INTO nodes(id, node_type, file_id, kind, name, qualname, is_definition, stable_key) "
                  "VALUES(11, 'symbol', 1, 'method', 'GetAsync', 'GetAsync', 1, 'sym:get')");
        conn.exec("INSERT INTO edges(id, src_id, dst_id, kind, confidence) "
                  "VALUES(20, 10, 11, 'calls', 0.5)");
        schema::set_kv(conn, "repo_root", base.string());
    }

    {
        Connection conn(db_path, true);
        QueryCache cache(conn);
        auto params = json_parse(R"({
            "source":"otlp",
            "traces":[{
                "caller":"HttpClient.SendAsync",
                "callee":"GetAsync",
                "count":1523,
                "p50_ms":45.2,
                "p99_ms":312.0,
                "error_rate":0.002
            }]
        })");
        REQUIRE(params);

        auto result = tools::ingest_traces(params.root(), conn, cache, base.string());
        auto doc = json_parse(result);
        REQUIRE(doc);
        REQUIRE(yyjson_obj_get(doc.root(), "error") == nullptr);
        CHECK(json_get_int(doc.root(), "ingested") == 1);
        CHECK(json_get_int(doc.root(), "resolved_edges") == 1);
        CHECK(json_get_int(doc.root(), "unresolved") == 0);
    }

    {
        Connection conn(db_path);
        sqlite3_stmt* trace_stmt = nullptr;
        sqlite3_prepare_v2(conn.raw(),
            "SELECT call_count, p50_ms, p99_ms, error_rate, source FROM traces "
            "WHERE caller_name = 'HttpClient.SendAsync' AND callee_name = 'GetAsync'",
            -1, &trace_stmt, nullptr);
        REQUIRE(sqlite3_step(trace_stmt) == SQLITE_ROW);
        CHECK(sqlite3_column_int64(trace_stmt, 0) == 1523);
        CHECK(sqlite3_column_double(trace_stmt, 1) == Catch::Approx(45.2));
        CHECK(sqlite3_column_double(trace_stmt, 2) == Catch::Approx(312.0));
        CHECK(sqlite3_column_double(trace_stmt, 3) == Catch::Approx(0.002));
        CHECK(std::string(reinterpret_cast<const char*>(sqlite3_column_text(trace_stmt, 4))) == "otlp");
        sqlite3_finalize(trace_stmt);

        sqlite3_stmt* edge_stmt = nullptr;
        sqlite3_prepare_v2(conn.raw(),
            "SELECT confidence FROM edges WHERE id = 20",
            -1, &edge_stmt, nullptr);
        REQUIRE(sqlite3_step(edge_stmt) == SQLITE_ROW);
        CHECK(sqlite3_column_double(edge_stmt, 0) > 0.5);
        sqlite3_finalize(edge_stmt);
    }

    {
        Connection conn(db_path, true);
        QueryCache cache(conn);
        auto params = json_parse(R"({"callee":"GetAsync","min_count":100,"limit":10})");
        REQUIRE(params);

        auto result = tools::get_traces(params.root(), conn, cache, base.string());
        auto doc = json_parse(result);
        REQUIRE(doc);
        REQUIRE(yyjson_obj_get(doc.root(), "error") == nullptr);
        CHECK(json_get_int(doc.root(), "returned") == 1);

        auto* results = yyjson_obj_get(doc.root(), "results");
        REQUIRE(results != nullptr);
        REQUIRE(yyjson_arr_size(results) == 1);
        auto* first = yyjson_arr_get(results, 0);
        REQUIRE(first != nullptr);
        CHECK(std::string(json_get_str(first, "caller_name")) == "HttpClient.SendAsync");
        CHECK(std::string(json_get_str(first, "callee_name")) == "GetAsync");
        CHECK(json_get_bool(first, "caller_resolved", false));
        CHECK(json_get_bool(first, "callee_resolved", false));
        CHECK(json_get_bool(first, "edge_resolved", false));
        CHECK(json_get_double(first, "edge_confidence", 0.0) > 0.5);
    }

    std::error_code ec;
    fs::remove_all(base, ec);
}
