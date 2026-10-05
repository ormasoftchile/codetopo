#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "db/connection.h"
#include "db/schema.h"
#include "index/quality.h"
#include "index/diff.h"
#include "mcp/tools.h"
#include "util/json.h"
#include <filesystem>
#include <fstream>

using namespace codetopo;
namespace fs = std::filesystem;

static fs::path quality_test_dir() {
    auto base = fs::current_path() / ".codetopo-quality-test";
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base);
    return base;
}

TEST_CASE("compute_graph_quality computes comprehensive metrics accurately",
          "[unit][quality]") {
    auto base = quality_test_dir();
    auto db_path = base / "index.sqlite";

    Connection conn(db_path);
    schema::ensure_schema(conn);

    // Insert files
    conn.exec("INSERT INTO files(id, path, language, size_bytes, mtime_ns, content_hash, parse_status) "
              "VALUES(1, 'src/main.cpp', 'cpp', 100, 1000, 'h1', 'ok')");
    conn.exec("INSERT INTO files(id, path, language, size_bytes, mtime_ns, content_hash, parse_status) "
              "VALUES(2, 'src/util.cpp', 'cpp', 200, 2000, 'h2', 'partial')");
    conn.exec("INSERT INTO files(id, path, language, size_bytes, mtime_ns, content_hash, parse_status) "
              "VALUES(3, 'scripts/run.py', 'python', 50, 3000, 'h3', 'failed')");

    // Insert nodes
    conn.exec("INSERT INTO nodes(id, node_type, file_id, kind, name, qualname, is_definition, stable_key, rank) "
              "VALUES(10, 'symbol', 1, 'function', 'main', 'main', 1, 'sym:main', 0.85)");
    conn.exec("INSERT INTO nodes(id, node_type, file_id, kind, name, qualname, is_definition, stable_key, rank) "
              "VALUES(11, 'symbol', 2, 'function', 'helper', 'helper', 1, 'sym:helper', 0.4)");
    conn.exec("INSERT INTO nodes(id, node_type, file_id, kind, name, qualname, is_definition, stable_key, rank) "
              "VALUES(12, 'symbol', 2, 'function', 'helper', 'alt::helper', 1, 'sym:alt_helper', 0.0)");

    // Insert call refs
    // 1 resolved exact call
    conn.exec("INSERT INTO refs(id, file_id, kind, name, resolved_node_id, evidence) "
              "VALUES(100, 1, 'call', 'helper', 11, 'ast')");
    // 1 resolved approx call (name-match)
    conn.exec("INSERT INTO refs(id, file_id, kind, name, resolved_node_id, evidence) "
              "VALUES(101, 1, 'call', 'helper', 11, 'name-match')");
    // 1 ambiguous unresolved call (matches both 11 and 12)
    conn.exec("INSERT INTO refs(id, file_id, kind, name, resolved_node_id, evidence) "
              "VALUES(102, 1, 'call', 'helper', NULL, NULL)");
    // 1 dangling unresolved call (external library 'printf')
    conn.exec("INSERT INTO refs(id, file_id, kind, name, resolved_node_id, evidence) "
              "VALUES(103, 1, 'call', 'printf', NULL, NULL)");

    // Insert edges
    conn.exec("INSERT INTO edges(id, src_id, dst_id, kind, confidence, evidence, source, observed_count) "
              "VALUES(200, 10, 11, 'calls', 1.0, 'ast', 'static', 1)");
    conn.exec("INSERT INTO edges(id, src_id, dst_id, kind, confidence, evidence, source, observed_count) "
              "VALUES(201, 10, 12, 'calls', 0.7, 'name-match', 'static', 1)");

    GraphQuality q = compute_graph_quality(conn);

    CHECK(q.total_files == 3);
    CHECK(q.files_ok == 1);
    CHECK(q.files_partial == 1);
    CHECK(q.files_failed == 1);

    CHECK(q.total_symbols == 3);
    CHECK(q.ranked_symbols == 2);

    CHECK(q.total_edges == 2);
    CHECK(q.total_call_refs == 4);
    CHECK(q.resolved_call_refs == 2);
    CHECK(q.unresolved_call_refs == 2);
    CHECK(q.ambiguous_call_refs == 1);
    CHECK(q.dangling_call_refs == 1);
    CHECK(q.call_resolution_rate == Catch::Approx(50.0));

    REQUIRE(q.languages.size() == 1);
    CHECK(q.languages[0].language == "cpp");
    CHECK(q.languages[0].total_calls == 4);
    CHECK(q.languages[0].resolved_calls == 1);
    CHECK(q.languages[0].approx_calls == 1);
    CHECK(q.languages[0].unresolved_calls == 2);

    std::string table = format_quality_table(q, false);
    CHECK(table.find("CodeTopo Graph Quality Report") != std::string::npos);
    CHECK(table.find("cpp") != std::string::npos);
    CHECK(table.find("Overall Resolution:") != std::string::npos);

    std::string json_str = format_quality_json(q);
    auto doc = json_parse(json_str);
    REQUIRE(doc);
    CHECK(json_get_int(doc.root(), "total_files") == 3);
    CHECK(json_get_int(doc.root(), "total_symbols") == 3);
    CHECK(json_get_int(doc.root(), "ambiguous_call_refs") == 1);
    CHECK(json_get_int(doc.root(), "dangling_call_refs") == 1);
}

TEST_CASE("get_edge_evidence reports provenance and tracks runtime trace observations",
          "[unit][quality][provenance]") {
    auto base = quality_test_dir();
    auto db_path = base / "index.sqlite";

    Connection conn(db_path);
    schema::ensure_schema(conn);

    conn.exec("INSERT INTO files(id, path, language, size_bytes, mtime_ns, content_hash, parse_status) "
              "VALUES(1, 'src/storage.cpp', 'cpp', 100, 1000, 'h1', 'ok')");
    conn.exec("INSERT INTO nodes(id, node_type, file_id, kind, name, qualname, is_definition, stable_key) "
              "VALUES(10, 'symbol', 1, 'method', 'Alloc', 'BufferPool::Alloc', 1, 'sym:alloc')");
    conn.exec("INSERT INTO nodes(id, node_type, file_id, kind, name, qualname, is_definition, stable_key) "
              "VALUES(11, 'symbol', 1, 'method', 'AllocatePage', 'Storage::AllocatePage', 1, 'sym:alloc_page')");
    conn.exec("INSERT INTO nodes(id, node_type, file_id, kind, name, qualname, is_definition, stable_key) "
              "VALUES(12, 'symbol', 1, 'function', 'FastAlloc', 'Arena::FastAlloc', 1, 'sym:fast_alloc')");

    // Static call edge
    conn.exec("INSERT INTO edges(id, src_id, dst_id, kind, confidence, evidence, source, observed_count) "
              "VALUES(50, 10, 11, 'calls', 0.6, 'ast', 'static', 1)");
    schema::set_kv(conn, "repo_root", base.string());

    QueryCache cache(conn);

    // 1. Query unobserved static edge
    {
        auto params = json_parse(R"({"mode":"unobserved_static"})");
        auto res = tools::get_edge_evidence(params.root(), conn, cache, base.string());
        auto doc = json_parse(res);
        REQUIRE(doc);
        CHECK(json_get_int(doc.root(), "total") == 1);
        auto* edges = yyjson_obj_get(doc.root(), "edges");
        REQUIRE(edges);
        REQUIRE(yyjson_mut_arr_size(reinterpret_cast<yyjson_mut_val*>(edges)) == 1);
    }

    // 2. Ingest trace matching existing static edge (10 -> 11)
    {
        auto params = json_parse(R"({
            "source":"runtime_agent",
            "traces":[{
                "caller":"BufferPool::Alloc",
                "callee":"Storage::AllocatePage",
                "count":500,
                "p50_ms":1.2,
                "p99_ms":5.4,
                "error_rate":0.0
            }]
        })");
        auto res = tools::ingest_traces(params.root(), conn, cache, base.string());
        auto doc = json_parse(res);
        REQUIRE(doc);
        CHECK(json_get_int(doc.root(), "resolved_edges") == 1);
    }

    // 3. Ingest trace for a call NOT predicted statically (10 -> 12) -> inserts runtime edge
    {
        auto params = json_parse(R"({
            "source":"runtime_agent",
            "traces":[{
                "caller":"BufferPool::Alloc",
                "callee":"Arena::FastAlloc",
                "count":120,
                "p50_ms":0.3,
                "p99_ms":1.1,
                "error_rate":0.0
            }]
        })");
        auto res = tools::ingest_traces(params.root(), conn, cache, base.string());
        auto doc = json_parse(res);
        REQUIRE(doc);
        CHECK(json_get_int(doc.root(), "resolved_edges") == 1);
    }

    // 4. Query runtime_only edges
    {
        auto params = json_parse(R"({"mode":"runtime_only"})");
        auto res = tools::get_edge_evidence(params.root(), conn, cache, base.string());
        auto doc = json_parse(res);
        REQUIRE(doc);
        CHECK(json_get_int(doc.root(), "total") == 1);
    }

    // 5. Query observed edges (should return both the boosted static edge and runtime-only edge)
    {
        auto params = json_parse(R"({"mode":"observed"})");
        auto res = tools::get_edge_evidence(params.root(), conn, cache, base.string());
        auto doc = json_parse(res);
        REQUIRE(doc);
        CHECK(json_get_int(doc.root(), "total") == 2);
    }

    // 6. Query MCP graph_quality tool directly
    {
        auto res = tools::graph_quality(nullptr, conn, cache, base.string());
        auto doc = json_parse(res);
        REQUIRE(doc);
        CHECK(json_get_int(doc.root(), "total_files") == 1);
        CHECK(json_get_int(doc.root(), "total_edges") == 2);
        auto* prov = yyjson_obj_get(doc.root(), "provenance");
        REQUIRE(prov);
        CHECK(json_get_int(prov, "static") == 1);
        CHECK(json_get_int(prov, "runtime") == 1);
        CHECK(json_get_int(prov, "total_traces") == 2);
    }
}

TEST_CASE("compute_semantic_diff detects added symbols, modified signatures, and edge topology deltas",
          "[unit][diff]") {
    auto base = fs::current_path() / "build" / "test_diff_repo";
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base / "src");

    std::string quote_base = "\"" + base.string() + "\"";
#ifdef _WIN32
    std::string dev_null = " >NUL 2>NUL";
#else
    std::string dev_null = " >/dev/null 2>/dev/null";
#endif

    // Setup git repo
    REQUIRE(std::system(("git -C " + quote_base + " init" + dev_null).c_str()) == 0);
    REQUIRE(std::system(("git -C " + quote_base + " config user.email test@example.com").c_str()) == 0);
    REQUIRE(std::system(("git -C " + quote_base + " config user.name Tester").c_str()) == 0);

    // Initial commit
    {
        std::ofstream ofs(base / "src" / "math.py");
        ofs << "def add(a, b):\n"
            << "    return a + b\n";
    }
    REQUIRE(std::system(("git -C " + quote_base + " add ." + dev_null).c_str()) == 0);
    REQUIRE(std::system(("git -C " + quote_base + " commit -m initial" + dev_null).c_str()) == 0);

    // Setup DB and insert base state
    auto db_path = base / "index.sqlite";
    Connection conn(db_path);
    schema::ensure_schema(conn);

    conn.exec("INSERT INTO files(id, path, language, size_bytes, mtime_ns, content_hash, parse_status) "
              "VALUES(1, 'src/math.py', 'python', 35, 1000, 'h1', 'ok')");
    conn.exec("INSERT INTO nodes(id, node_type, file_id, kind, name, qualname, signature, start_line, end_line, is_definition, stable_key, rank) "
              "VALUES(10, 'symbol', 1, 'function', 'add', 'add', 'def add(a, b)', 1, 2, 1, 'src/math.py::function::add', 0.5)");

    // Working tree modification: change signature of add, add multiply that calls add
    {
        std::ofstream ofs(base / "src" / "math.py");
        ofs << "def add(a, b, c=0):\n"
            << "    return a + b + c\n\n"
            << "def multiply(x, y):\n"
            << "    return add(x, y)\n";
    }

    GraphDiffReport report = compute_semantic_diff(conn, base.string(), "HEAD", "working-tree");
    CHECK(report.files_changed == 1);
    CHECK(report.symbols_added == 1);
    CHECK(report.symbols_modified == 1); // add signature changed
    CHECK(report.edges_added >= 1); // multiply -> add

    // Formatters
    std::string table = format_diff_table(report, false);
    CHECK(table.find("+ symbol multiply") != std::string::npos);
    CHECK(table.find("~ symbol add") != std::string::npos);

    std::string json_str = format_diff_json(report);
    auto doc = json_parse(json_str);
    REQUIRE(doc);
    auto* summary = yyjson_obj_get(doc.root(), "summary");
    REQUIRE(summary);
    CHECK(json_get_int(summary, "symbols_added") == 1);
    CHECK(json_get_int(summary, "symbols_modified") == 1);

    // MCP tool verification
    QueryCache cache(conn);
    auto params = json_parse(R"({"base":"HEAD","target":"working-tree"})");
    auto tool_res = tools::graph_diff(params.root(), conn, cache, base.string());
    auto tool_doc = json_parse(tool_res);
    REQUIRE(tool_doc);
    auto* tool_sum = yyjson_obj_get(tool_doc.root(), "summary");
    REQUIRE(tool_sum);
    CHECK(json_get_int(tool_sum, "symbols_added") == 1);

    fs::remove_all(base, ec);
}

