#include <catch2/catch_test_macros.hpp>
#include "cli/cmd_workspace.h"
#include "db/queries.h"
#include "db/schema.h"
#include "mcp/tools.h"
#include "util/json.h"
#include <chrono>
#include <filesystem>
#include <sstream>
#include <string>

using namespace codetopo;
namespace fs = std::filesystem;

namespace {

struct ListingFixture {
    fs::path base = fs::current_path() / "build" / "workspace-listing-fixture";
    fs::path primary = base / "primary";
    fs::path extra = base / "extra";
    fs::path database = primary / ".codetopo" / "index.sqlite";

    ListingFixture() {
        REQUIRE_FALSE(fs::exists(base));
        fs::create_directories(primary / ".codetopo");
        fs::create_directories(extra);
        Connection conn(database);
        REQUIRE(schema::ensure_schema(conn) == 0);
        schema::set_kv(conn, "repo_root", fs::canonical(primary).string());
        schema::set_kv(conn, "last_index_time", "2026-10-07T00:00:00Z");
        schema::set_kv(conn, "index_state", "current");
        sqlite3_stmt* stmt = nullptr;
        REQUIRE(sqlite3_prepare_v2(conn.raw(),
            "INSERT INTO roots(id,path,added_at) VALUES(1,?,datetime('now'))",
            -1, &stmt, nullptr) == SQLITE_OK);
        auto path = fs::canonical(extra).string();
        sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
        REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
        conn.exec(
            "INSERT INTO files(id,path,language,size_bytes,mtime_ns,content_hash,parse_status,root_id) "
            "VALUES(1,'extra.cpp','cpp',10,1,'hash','ok',1)");
        conn.exec(
            "INSERT INTO files(id,path,language,size_bytes,mtime_ns,content_hash,parse_status,root_id) "
            "VALUES(2,'primary.cpp','cpp',10,1,'primary-hash','ok',NULL)");
        conn.exec(
            "INSERT INTO nodes(id,node_type,file_id,kind,name,stable_key) VALUES"
            "(10,'file',NULL,'file','extra.cpp','1:extra.cpp::file'),"
            "(11,'symbol',1,'function','extra','1:extra.cpp::function::extra')");
        conn.exec(
            "INSERT INTO edges(src_id,dst_id,kind,confidence) VALUES(10,11,'contains',1)");
    }

    ~ListingFixture() {
        std::error_code ec;
        fs::remove_all(base, ec);
    }
};

struct CaptureOutput {
    std::ostringstream output;
    std::streambuf* previous = std::cout.rdbuf(output.rdbuf());
    ~CaptureOutput() { std::cout.rdbuf(previous); }
};

} // namespace

TEST_CASE("CLI workspace listing reads committed roots while writer admission is held",
          "[unit][workspace][workspace-list][integration]") {
    ListingFixture fixture;
    FileLock writer_lock(fixture.database.string() + ".lock");
    REQUIRE(writer_lock.acquire());
    Connection writer(fixture.database);
    writer.exec("BEGIN IMMEDIATE");
    writer.exec("UPDATE roots SET path='uncommitted-root' WHERE id=1");
    writer.exec("INSERT INTO kv(key,value) VALUES('workspace_content_fts_pending:1','1')");

    CaptureOutput capture;
    auto started = std::chrono::steady_clock::now();
    CHECK(run_workspace_list(fixture.primary.string()) == 0);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
    CHECK(capture.output.str().find(fs::canonical(fixture.extra).string()) != std::string::npos);
    CHECK(capture.output.str().find(fs::canonical(fixture.primary).string()) != std::string::npos);
    CHECK(capture.output.str().find("Workspace roots (2)") != std::string::npos);
    CHECK(capture.output.str().find("[primary] (1 files; graph totals not computed)") != std::string::npos);
    CHECK(capture.output.str().find("1 files, 1 symbols, 1 edges") != std::string::npos);
    CHECK(capture.output.str().find("uncommitted-root") == std::string::npos);
    CHECK(writer_lock.held_by_live_process());
    writer.exec("ROLLBACK");
}

TEST_CASE("Read-only workspace reader and MCP return consistent root counts",
          "[unit][workspace][workspace-list]") {
    ListingFixture fixture;
    Connection conn(fixture.database, true);
    CHECK(sqlite3_db_readonly(conn.raw(), "main") == 1);
    ReadSnapshot snapshot(conn);
    auto roots = read_workspace_roots(conn);
    REQUIRE(roots.size() == 1);
    CHECK(roots[0].files == 1);
    CHECK(roots[0].symbols == 1);
    CHECK(roots[0].edges == 1);
    CHECK(sqlite3_total_changes(conn.raw()) == 0);

    QueryCache cache(conn);
    auto result = json_parse(tools::workspace_list(nullptr, conn, cache, fixture.primary.string()));
    REQUIRE(result);
    auto* entries = yyjson_obj_get(result.root(), "roots");
    REQUIRE(yyjson_arr_size(entries) == 2);
    auto* primary = yyjson_arr_get_first(entries);
    CHECK(json_get_int(primary, "root_id") == 0);
    CHECK(std::string(json_get_str(primary, "role")) == "primary");
    CHECK(json_get_int(primary, "file_count") == 1);
    CHECK(yyjson_is_null(yyjson_obj_get(primary, "symbol_count")));
    CHECK(yyjson_is_null(yyjson_obj_get(primary, "edge_count")));
    CHECK_FALSE(json_get_bool(primary, "graph_counts_checked"));
    auto* entry = yyjson_arr_get(entries, 1);
    CHECK(std::string(json_get_str(entry, "role")) == "additional");
    CHECK(json_get_int(entry, "file_count") == 1);
    CHECK(json_get_int(entry, "symbol_count") == 1);
    CHECK(json_get_int(entry, "edge_count") == 1);
    auto stats = json_parse(tools::repo_stats(nullptr, conn, cache, fixture.primary.string()));
    REQUIRE(stats);
    auto* stats_roots = yyjson_obj_get(stats.root(), "roots");
    REQUIRE(yyjson_arr_size(stats_roots) == 2);
    CHECK(json_get_int(yyjson_arr_get_first(stats_roots), "root_id") == 0);
    CHECK(json_get_int(yyjson_arr_get_first(stats_roots), "files") == 1);
    CHECK(std::string(json_get_str(yyjson_arr_get_first(stats_roots), "path")) ==
          fixture.primary.string());
}

TEST_CASE("Single-root workspace listing still includes its primary repository",
          "[unit][workspace][workspace-list]") {
    ListingFixture fixture;
    {
        Connection conn(fixture.database);
        conn.exec("DELETE FROM roots WHERE id=1");
    }
    CaptureOutput capture;
    CHECK(run_workspace_list(fixture.primary.string()) == 0);
    CHECK(capture.output.str().find("Workspace roots (1)") != std::string::npos);
    CHECK(capture.output.str().find(fs::canonical(fixture.primary).string()) != std::string::npos);
    CHECK(capture.output.str().find("[primary]") != std::string::npos);
}

TEST_CASE("Workspace listing does not create a missing index",
          "[unit][workspace][workspace-list]") {
    auto root = fs::current_path() / "build" / "workspace-listing-missing";
    REQUIRE_FALSE(fs::exists(root));
    fs::create_directories(root);
    {
        CaptureOutput capture;
        CHECK(run_workspace_list(root.string()) == 0);
        CHECK(capture.output.str().find("No index found") != std::string::npos);
        CHECK_FALSE(fs::exists(root / ".codetopo"));
    }
    fs::remove(root);
}
