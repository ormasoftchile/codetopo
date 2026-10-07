#include <catch2/catch_test_macros.hpp>
#include "cli/cmd_index.h"
#include "core/config.h"
#include "db/connection.h"
#include "db/schema.h"
#include "index/ownership.h"
#include "mcp/reindex.h"
#include "mcp/tools.h"
#include "db/queries.h"
#include "index/persister.h"
#include "util/json.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace codetopo;

namespace {

fs::path test_root(const std::string& name) {
    auto root = fs::current_path() / "build" / ("test_root_ownership_" + name);
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    return root;
}

void cleanup(const fs::path& path) {
    std::error_code ec;
    fs::remove_all(path, ec);
}

int64_t scalar_int(Connection& conn, const std::string& sql) {
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(conn.raw(), sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK);
    int64_t value = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) value = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return value;
}

void insert_root(Connection& conn, int64_t id, const fs::path& path) {
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(conn.raw(),
        "INSERT INTO roots(id, path, added_at) VALUES(?, ?, datetime('now'))",
        -1, &stmt, nullptr) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, id);
    auto text = path.string();
    sqlite3_bind_text(stmt, 2, text.c_str(), -1, SQLITE_TRANSIENT);
    REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
}

Config index_config(const fs::path& root, const fs::path& db) {
    Config cfg;
    cfg.repo_root = root;
    cfg.db_path = db;
    cfg.thread_count = 1;
    cfg.batch_size = 1;
    cfg.arena_size_mb = 8;
    cfg.no_gitignore = true;
    cfg.parse_timeout_s = 2;
    cfg.extraction_timeout_s = 2;
    return cfg;
}

} // namespace

TEST_CASE("Primary root resolution never derives unknown ownership from client CWD",
          "[unit][ownership]") {
    auto base = test_root("resolution");
    auto primary = base / "primary";
    auto client = base / "client";
    fs::create_directories(primary / ".codetopo");
    fs::create_directories(client);
    auto db = primary / ".codetopo" / "index.sqlite";

    {
        Connection conn(db);
        REQUIRE(schema::ensure_schema(conn) == 0);

        auto explicit_resolution = index_ownership::resolve_primary_root(
            conn, primary, true, db.string());
        CHECK(index_ownership::same_root(explicit_resolution.root, primary));
        CHECK_FALSE(explicit_resolution.metadata_present);

        CHECK_THROWS(index_ownership::resolve_primary_root(
            conn, client, false, db.string()));

        schema::set_kv(conn, "repo_root", fs::canonical(primary).string());
        auto metadata_resolution = index_ownership::resolve_primary_root(
            conn, client, false, db.string());
        CHECK(index_ownership::same_root(metadata_resolution.root, primary));
        CHECK(metadata_resolution.metadata_present);

        CHECK_THROWS(index_ownership::resolve_primary_root(
            conn, client, true, db.string()));
    }
    cleanup(base);
}

#ifdef _WIN32
TEST_CASE("Windows root comparison normalizes case and separators",
          "[unit][ownership][windows]") {
    CHECK(index_ownership::comparison_key(R"(C:\One\DsMainDev\Sql\)") ==
          index_ownership::comparison_key("c:/one/dsmaindev/sql"));
}
#endif

TEST_CASE("Missing ownership metadata makes the first full index non-destructive",
          "[unit][ownership][index]") {
    auto base = test_root("non_destructive");
    auto primary = base / "primary";
    auto extra = base / "extra";
    fs::create_directories(primary / ".codetopo");
    fs::create_directories(primary / "src");
    fs::create_directories(extra);
    std::ofstream(primary / "src" / "live.cpp") << "int live_symbol() { return 1; }\n";
    auto db = primary / ".codetopo" / "index.sqlite";

    {
        Connection conn(db);
        REQUIRE(schema::ensure_schema(conn) == 0);
        insert_root(conn, 1, fs::canonical(extra));
        conn.exec(
            "INSERT INTO files(id,path,language,size_bytes,mtime_ns,content_hash,parse_status,root_id) VALUES"
            "(1,'stale-primary.cpp','cpp',1,1,'stale','ok',NULL),"
            "(2,'extra-a.cpp','cpp',1,1,'extra-a','ok',1),"
            "(3,'extra-b.cpp','cpp',1,1,'extra-b','ok',1)");
        conn.exec(
            "INSERT INTO nodes(id,node_type,file_id,kind,name,stable_key) VALUES"
            "(10,'symbol',1,'function','stale_primary','stale-primary.cpp::function::stale_primary'),"
            "(20,'symbol',2,'function','extra_a','extra-a.cpp::function::extra_a'),"
            "(30,'symbol',3,'function','extra_b','extra-b.cpp::function::extra_b')");
        conn.exec(
            "INSERT INTO edges(id,src_id,dst_id,kind,confidence,evidence) "
            "VALUES(100,20,30,'calls',0.75,'name-match')");
    }

    auto cfg = index_config(primary, db);
    REQUIRE(run_index(cfg) == 0);
    {
        Connection conn(db);
        CHECK(scalar_int(conn,
            "SELECT COUNT(*) FROM files WHERE path='stale-primary.cpp' AND root_id IS NULL") == 1);
        CHECK(scalar_int(conn, "SELECT COUNT(*) FROM files WHERE root_id=1") == 2);
        CHECK(scalar_int(conn, "SELECT COUNT(*) FROM edges WHERE id=100") == 1);
        CHECK(schema::get_kv(conn, "repo_root", "") == fs::canonical(primary).string());
        CHECK(schema::get_kv(conn, "index_state", "") == "needs_reconciliation");
        CHECK(schema::get_kv(conn, "last_index_time", "").empty());
    }

    REQUIRE(run_index(cfg) == 0);
    {
        Connection conn(db);
        CHECK(scalar_int(conn,
            "SELECT COUNT(*) FROM files WHERE path='stale-primary.cpp' AND root_id IS NULL") == 0);
        CHECK(scalar_int(conn, "SELECT COUNT(*) FROM files WHERE root_id=1") == 2);
        CHECK(scalar_int(conn, "SELECT COUNT(*) FROM edges WHERE id=100") == 1);
        CHECK(schema::get_kv(conn, "index_state", "") == "current");
        CHECK_FALSE(schema::get_kv(conn, "last_index_time", "").empty());
    }

    cleanup(base);
}

TEST_CASE("Conflicting explicit root is rejected before index mutation",
          "[unit][ownership][index]") {
    auto base = test_root("conflict");
    auto primary = base / "primary";
    auto other = base / "other";
    fs::create_directories(primary / ".codetopo");
    fs::create_directories(other);
    auto db = primary / ".codetopo" / "index.sqlite";

    std::string generation;
    {
        Connection conn(db);
        REQUIRE(schema::ensure_schema(conn) == 0);
        schema::set_kv(conn, "repo_root", fs::canonical(primary).string());
        schema::set_kv(conn, "index_state", "current");
        schema::set_kv(conn, "last_index_time", "2026-10-06T00:00:00Z");
        schema::set_kv(conn, "index_generation", "before-conflict");
        conn.exec(
            "INSERT INTO files(path,language,size_bytes,mtime_ns,content_hash,parse_status) "
            "VALUES('owned.cpp','cpp',1,1,'owned','ok')");
        generation = schema::get_kv(conn, "index_generation", "");
    }

    auto cfg = index_config(other, db);
    REQUIRE(run_index(cfg) == 1);
    {
        Connection conn(db);
        CHECK(scalar_int(conn, "SELECT COUNT(*) FROM files WHERE path='owned.cpp'") == 1);
        CHECK(schema::get_kv(conn, "index_generation", "") == generation);
        CHECK(schema::get_kv(conn, "repo_root", "") == fs::canonical(primary).string());
    }
    cleanup(base);
}

TEST_CASE("Index metadata reports missing active interrupted and current states",
          "[unit][ownership][freshness]") {
    auto base = test_root("status");
    auto db = base / "index.sqlite";
    {
        Connection conn(db);
        REQUIRE(schema::ensure_schema(conn) == 0);
        CHECK(index_ownership::inspect_metadata(conn, false).index == "missing_metadata");

        schema::set_kv(conn, "index_state", "indexing");
        CHECK(index_ownership::inspect_metadata(conn, true).index == "indexing");
        CHECK(index_ownership::inspect_metadata(conn, false).index == "interrupted");

        schema::set_kv(conn, "repo_root", fs::canonical(base).string());
        schema::set_kv(conn, "index_state", "current");
        CHECK(index_ownership::inspect_metadata(conn, false).index == "incomplete");

        schema::set_kv(conn, "last_index_time", "2026-10-06T00:00:00Z");
        CHECK(index_ownership::inspect_metadata(conn, false).index == "current");
    }
    cleanup(base);
}

TEST_CASE("Completed index UTC timestamp reports a nonnegative fresh age",
          "[unit][ownership][freshness]") {
    auto base = test_root("utc_age");
    {
        Connection conn(base / "index.sqlite");
        REQUIRE(schema::ensure_schema(conn) == 0);
        Persister persister(conn);
        persister.write_metadata(fs::canonical(base).string());
        QueryCache cache(conn);
        auto result = json_parse(tools::server_info(nullptr, conn, cache, base.string()));
        REQUIRE(result);
        auto age = json_get_int(result.root(), "index_age_seconds", -1);
        CHECK(age >= 0);
        CHECK(age < 5);
    }
    cleanup(base);
}

TEST_CASE("Ownership bootstrap primary-row probe is index driven",
          "[unit][ownership][query-plan]") {
    auto base = test_root("query_plan");
    auto db = base / "index.sqlite";
    {
        Connection conn(db);
        REQUIRE(schema::ensure_schema(conn) == 0);
        sqlite3_stmt* stmt = nullptr;
        REQUIRE(sqlite3_prepare_v2(conn.raw(),
            "EXPLAIN QUERY PLAN "
            "SELECT 1 FROM files WHERE root_id IS NULL LIMIT 1",
            -1, &stmt, nullptr) == SQLITE_OK);
        REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
        auto* detail = sqlite3_column_text(stmt, 3);
        REQUIRE(detail != nullptr);
        CHECK(std::string(reinterpret_cast<const char*>(detail)).find(
            "idx_files_root") != std::string::npos);
        sqlite3_finalize(stmt);
    }
    cleanup(base);
}

TEST_CASE("Automatic reindex uses authoritative arguments and coalesces concurrent clients",
          "[unit][ownership][reindex]") {
    auto base = test_root("reindex");
    auto primary = base / "primary";
    auto client = base / "client";
    fs::create_directories(primary / ".codetopo");
    fs::create_directories(client);
    auto db = primary / ".codetopo" / "index.sqlite";
    {
        Connection conn(db);
        REQUIRE(schema::ensure_schema(conn) == 0);
    }

    std::vector<std::string> captured;
    std::string changed_list;
    std::promise<void> captured_promise;
    auto captured_future = captured_promise.get_future();
    ReindexState watcher;
    watcher.spawn = [&](const std::string&, const std::vector<std::string>& args) {
        captured = args;
        auto changed = std::find(args.begin(), args.end(), "--changed-file");
        if (changed != args.end() && std::next(changed) != args.end()) {
            std::ifstream in(*std::next(changed));
            std::getline(in, changed_list);
        }
        captured_promise.set_value();
        return 0;
    };
    watcher.trigger(
        fs::canonical(primary).string(), fs::canonical(db).string(), [] {},
        {"src/changed.cpp"}, false, ReindexReason::watcher);
    REQUIRE(captured_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    watcher.stop();

    REQUIRE(captured.size() >= 7);
    CHECK(captured[0] == "index");
    CHECK(captured[1] == "--root");
    CHECK(captured[2] == fs::canonical(primary).string());
    CHECK(captured[3] == "--db");
    CHECK(captured[4] == fs::canonical(db).string());
    CHECK(changed_list == "src/changed.cpp");

    std::atomic<int> spawn_count{0};
    std::promise<void> first_entered;
    auto first_entered_future = first_entered.get_future();
    std::promise<void> release_first;
    auto release_future = release_first.get_future().share();
    ReindexState first;
    ReindexState second;
    first.automatic_coordination_timeout = std::chrono::milliseconds(100);
    second.automatic_coordination_timeout = std::chrono::milliseconds(100);
    first.spawn = [&](const std::string&, const std::vector<std::string>&) {
        ++spawn_count;
        first_entered.set_value();
        release_future.wait();
        return 0;
    };
    second.spawn = [&](const std::string&, const std::vector<std::string>&) {
        ++spawn_count;
        return 0;
    };

    first.trigger(
        fs::canonical(primary).string(), fs::canonical(db).string(), [] {},
        {}, true, ReindexReason::startup);
    REQUIRE(first_entered_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    second.trigger(
        fs::canonical(primary).string(), fs::canonical(db).string(), [] {},
        {}, true, ReindexReason::startup);
    second.stop();
    release_first.set_value();
    first.stop();
    CHECK(spawn_count.load() == 1);

    cleanup(base);
}

TEST_CASE("Freshness policy retains existing startup and watcher behavior",
          "[unit][ownership][freshness]") {
    CHECK(freshness_reconciles_on_startup(FreshnessPolicy::eager));
    CHECK(freshness_reconciles_on_startup(FreshnessPolicy::normal));
    CHECK_FALSE(freshness_reconciles_on_startup(FreshnessPolicy::lazy));
    CHECK_FALSE(freshness_reconciles_on_startup(FreshnessPolicy::off));
    CHECK(freshness_allows_watching(FreshnessPolicy::eager));
    CHECK(freshness_allows_watching(FreshnessPolicy::normal));
    CHECK(freshness_allows_watching(FreshnessPolicy::lazy));
    CHECK_FALSE(freshness_allows_watching(FreshnessPolicy::off));
}
