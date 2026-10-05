#include <catch2/catch_test_macros.hpp>
#include "index/stable_key.h"
#include "db/connection.h"
#include "db/schema.h"
#include "index/persister.h"
#include <filesystem>

using namespace codetopo;

TEST_CASE("make_stable_key basic format", "[stable_key]") {
    auto key = make_stable_key("src/foo.cpp", "function", "ns::Foo::Bar");
    REQUIRE(key == "src/foo.cpp::function::ns::Foo::Bar");
}

TEST_CASE("make_file_stable_key", "[stable_key]") {
    auto key = make_file_stable_key("src/main.cpp");
    REQUIRE(key == "src/main.cpp::file");
}

TEST_CASE("resolve_collisions no duplicates", "[stable_key]") {
    std::vector<KeyCandidate> candidates = {
        {"src/foo.cpp::function::bar", 10},
        {"src/foo.cpp::function::baz", 20},
    };
    auto result = resolve_collisions(candidates);
    REQUIRE(result.size() == 2);
    REQUIRE(result[0] == "src/foo.cpp::function::bar");
    REQUIRE(result[1] == "src/foo.cpp::function::baz");
}

TEST_CASE("resolve_collisions with overloads", "[stable_key]") {
    std::vector<KeyCandidate> candidates = {
        {"src/foo.cpp::function::overloaded", 10},
        {"src/foo.cpp::function::overloaded", 30},
        {"src/foo.cpp::function::overloaded", 20},
    };
    auto result = resolve_collisions(candidates);
    REQUIRE(result.size() == 3);
    // Sorted by start_line: 10, 20, 30
    REQUIRE(result[0] == "src/foo.cpp::function::overloaded");
    REQUIRE(result[1] == "src/foo.cpp::function::overloaded#2");
    REQUIRE(result[2] == "src/foo.cpp::function::overloaded#3");
}

TEST_CASE("resolve_collisions mixed unique and duplicate", "[stable_key]") {
    std::vector<KeyCandidate> candidates = {
        {"a::function::foo", 5},
        {"a::function::bar", 10},
        {"a::function::foo", 15},
    };
    auto result = resolve_collisions(candidates);
    REQUIRE(result.size() == 3);
    REQUIRE(result[0] == "a::function::foo");
    REQUIRE(result[1] == "a::function::bar");
    REQUIRE(result[2] == "a::function::foo#2");
}

TEST_CASE("durable symbol identity in-place UPSERT preserves node id, rank, incoming edges, and runtime provenance",
          "[stable_key][persister]") {
    namespace fs = std::filesystem;
    auto base = fs::current_path() / "build" / "test_durable_identity";
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base);

    auto db_path = base / "index.sqlite";
    Connection conn(db_path);
    schema::ensure_schema(conn);

    Persister persister(conn);

    // Initial extraction of src/service.cpp
    ScannedFile file;
    file.relative_path = "src/service.cpp";
    file.language = "cpp";
    file.size_bytes = 200;
    file.mtime_ns = 1000;

    ExtractionResult ext1;
    ExtractedSymbol s_start;
    s_start.kind = "function";
    s_start.name = "start";
    s_start.qualname = "Service::start";
    s_start.signature = "void start()";
    s_start.start_line = 10;
    s_start.end_line = 15;
    s_start.stable_key = make_stable_key(file.relative_path, s_start.kind, s_start.qualname);
    ext1.symbols.push_back(s_start);

    ExtractedSymbol s_stop;
    s_stop.kind = "function";
    s_stop.name = "stop";
    s_stop.qualname = "Service::stop";
    s_stop.signature = "void stop()";
    s_stop.start_line = 20;
    s_stop.end_line = 25;
    s_stop.stable_key = make_stable_key(file.relative_path, s_stop.kind, s_stop.qualname);
    ext1.symbols.push_back(s_stop);

    ExtractedSymbol s_helper;
    s_helper.kind = "function";
    s_helper.name = "helper";
    s_helper.qualname = "Service::helper";
    s_helper.signature = "void helper()";
    s_helper.start_line = 30;
    s_helper.end_line = 35;
    s_helper.stable_key = make_stable_key(file.relative_path, s_helper.kind, s_helper.qualname);
    ext1.symbols.push_back(s_helper);

    REQUIRE(persister.persist_file(file, ext1, "hash_v1", "ok"));

    // Query node IDs assigned
    int64_t start_id = 0;
    int64_t stop_id = 0;
    int64_t helper_id = 0;
    {
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(conn.raw(), "SELECT id, name FROM nodes WHERE node_type = 'symbol'", -1, &stmt, nullptr);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            int64_t id = sqlite3_column_int64(stmt, 0);
            std::string name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            if (name == "start") start_id = id;
            else if (name == "stop") stop_id = id;
            else if (name == "helper") helper_id = id;
        }
        sqlite3_finalize(stmt);
    }
    REQUIRE(start_id > 0);
    REQUIRE(stop_id > 0);
    REQUIRE(helper_id > 0);

    // Set PageRank rank and insert an incoming runtime edge from an external node (e.g. main() in another file)
    conn.exec("UPDATE nodes SET rank = 0.85 WHERE id = " + std::to_string(start_id));
    conn.exec("INSERT INTO nodes(id, node_type, kind, name, stable_key) VALUES(999, 'symbol', 'function', 'main', 'src/main.cpp::function::main')");
    conn.exec("INSERT INTO edges(src_id, dst_id, kind, confidence, source, observed_count) "
              "VALUES(999, " + std::to_string(start_id) + ", 'calls', 0.95, 'runtime', 5)");

    // Re-indexing pass: file is modified
    // start() moved to line 40, signature changed
    // stop() unchanged
    // helper() removed
    // restart() added
    file.mtime_ns = 2000;
    file.size_bytes = 250;

    ExtractionResult ext2;
    ExtractedSymbol s_start2 = s_start;
    s_start2.start_line = 40;
    s_start2.end_line = 45;
    s_start2.signature = "void start(bool fast)";
    ext2.symbols.push_back(s_start2);

    ExtractedSymbol s_stop2 = s_stop;
    ext2.symbols.push_back(s_stop2);

    ExtractedSymbol s_restart;
    s_restart.kind = "function";
    s_restart.name = "restart";
    s_restart.qualname = "Service::restart";
    s_restart.signature = "void restart()";
    s_restart.start_line = 50;
    s_restart.end_line = 55;
    s_restart.stable_key = make_stable_key(file.relative_path, s_restart.kind, s_restart.qualname);
    ext2.symbols.push_back(s_restart);

    REQUIRE(persister.persist_file(file, ext2, "hash_v2", "ok"));

    // Verify durable identity preservation:
    // 1. start() must have the exact same node_id
    int64_t start_id_after = 0;
    int start_line_after = 0;
    double start_rank_after = 0.0;
    std::string start_sig_after;
    {
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(conn.raw(), "SELECT id, start_line, rank, signature FROM nodes WHERE stable_key = ?", -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, s_start.stable_key.c_str(), -1, SQLITE_STATIC);
        REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
        start_id_after = sqlite3_column_int64(stmt, 0);
        start_line_after = sqlite3_column_int(stmt, 1);
        start_rank_after = sqlite3_column_double(stmt, 2);
        start_sig_after = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        sqlite3_finalize(stmt);
    }
    CHECK(start_id_after == start_id); // DURABLE NODE ID!
    CHECK(start_line_after == 40);     // In-place updated line!
    CHECK(start_rank_after == 0.85);   // Centrality preserved!
    CHECK(start_sig_after == "void start(bool fast)");

    // 2. Incoming external edge with runtime provenance and observed_count MUST BE INTACT!
    {
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(conn.raw(), "SELECT source, observed_count FROM edges WHERE src_id = 999 AND dst_id = ?", -1, &stmt, nullptr);
        sqlite3_bind_int64(stmt, 1, start_id);
        REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
        std::string src = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        int count = sqlite3_column_int(stmt, 1);
        CHECK(src == "runtime");
        CHECK(count == 5);
        sqlite3_finalize(stmt);
    }

    // 3. stop() retains node_id
    {
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(conn.raw(), "SELECT id FROM nodes WHERE stable_key = ?", -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, s_stop.stable_key.c_str(), -1, SQLITE_STATIC);
        REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
        CHECK(sqlite3_column_int64(stmt, 0) == stop_id);
        sqlite3_finalize(stmt);
    }

    // 4. helper() was removed
    {
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(conn.raw(), "SELECT COUNT(*) FROM nodes WHERE id = ?", -1, &stmt, nullptr);
        sqlite3_bind_int64(stmt, 1, helper_id);
        REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
        CHECK(sqlite3_column_int(stmt, 0) == 0);
        sqlite3_finalize(stmt);
    }

    // 5. restart() was added
    {
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(conn.raw(), "SELECT COUNT(*) FROM nodes WHERE name = 'restart'", -1, &stmt, nullptr);
        REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
        CHECK(sqlite3_column_int(stmt, 0) == 1);
        sqlite3_finalize(stmt);
    }

    fs::remove_all(base, ec);
}

