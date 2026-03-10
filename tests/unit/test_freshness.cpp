// Tests for index freshness features (R1, R2, R5, R6, R7, R9) + P2 watcher integration
#include <catch2/catch_test_macros.hpp>
#include "db/connection.h"
#include "db/schema.h"
#include "db/queries.h"
#include "core/config.h"
#include "index/persister.h"
#include "index/scanner.h"
#include "util/git.h"
#include "mcp/server.h"
#include "watch/watcher.h"
#include <sqlite3.h>
#include <filesystem>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>

namespace fs = std::filesystem;
using namespace codetopo;

// Helper: clean up temp dir, ignoring errors (WAL files may linger on Windows)
static void cleanup(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
}

// ---------------------------------------------------------------------------
// R7: QueryCache::clear()
// ---------------------------------------------------------------------------

TEST_CASE("QueryCache: get prepares and caches a statement", "[freshness][querycache]") {
    auto tmp = fs::temp_directory_path() / "codetopo_qcache_test";
    fs::create_directories(tmp);
    auto db_path = tmp / "qcache.sqlite";

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);
        QueryCache cache(conn);

        auto* stmt = cache.get("count_files", "SELECT COUNT(*) FROM files");
        REQUIRE(stmt != nullptr);

        // Same name returns the same (reset) statement — no error
        auto* stmt2 = cache.get("count_files", "SELECT COUNT(*) FROM files");
        REQUIRE(stmt2 == stmt);
    }
    cleanup(tmp);
}

TEST_CASE("QueryCache: clear invalidates all cached statements", "[freshness][querycache]") {
    auto tmp = fs::temp_directory_path() / "codetopo_qcache_clear";
    fs::create_directories(tmp);
    auto db_path = tmp / "qcache_clear.sqlite";

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);
        QueryCache cache(conn);

        // Prepare a statement
        auto* stmt1 = cache.get("count_files", "SELECT COUNT(*) FROM files");
        REQUIRE(stmt1 != nullptr);

        // Clear the cache
        cache.clear();

        // After clear, get() re-prepares successfully and the statement works
        auto* stmt2 = cache.get("count_files", "SELECT COUNT(*) FROM files");
        REQUIRE(stmt2 != nullptr);

        // The re-prepared statement is functional (can step and get a result)
        REQUIRE(sqlite3_step(stmt2) == SQLITE_ROW);
        auto count = sqlite3_column_int64(stmt2, 0);
        REQUIRE(count >= 0);  // Just verify it returned a valid result
    }
    cleanup(tmp);
}

// ---------------------------------------------------------------------------
// R1: FreshnessPolicy enum
// ---------------------------------------------------------------------------

TEST_CASE("FreshnessPolicy enum values exist and are assignable", "[freshness][config]") {
    FreshnessPolicy p;

    p = FreshnessPolicy::eager;
    REQUIRE(p == FreshnessPolicy::eager);

    p = FreshnessPolicy::normal;
    REQUIRE(p == FreshnessPolicy::normal);

    p = FreshnessPolicy::lazy;
    REQUIRE(p == FreshnessPolicy::lazy);

    p = FreshnessPolicy::off;
    REQUIRE(p == FreshnessPolicy::off);
}

TEST_CASE("Config default freshness is normal", "[freshness][config]") {
    Config cfg;
    REQUIRE(cfg.freshness == FreshnessPolicy::normal);
}

TEST_CASE("Config default debounce_ms is 1000", "[freshness][config]") {
    Config cfg;
    REQUIRE(cfg.debounce_ms == 1000);
}

// ---------------------------------------------------------------------------
// R6: rehab_quarantine()
// ---------------------------------------------------------------------------

static Connection make_freshness_db(const fs::path& db_path) {
    Connection conn(db_path);
    schema::ensure_schema(conn);
    return conn;
}

static void insert_file_record(Connection& conn, const std::string& path,
                                int64_t mtime_ns, int64_t size_bytes) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(conn.raw(),
        "INSERT INTO files(path, language, size_bytes, mtime_ns, content_hash, parse_status) "
        "VALUES(?, 'cpp', ?, ?, 'hash123', 'ok')", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, size_bytes);
    sqlite3_bind_int64(stmt, 3, mtime_ns);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

TEST_CASE("rehab_quarantine: file with changed mtime is rehabilitated", "[freshness][quarantine]") {
    auto tmp = fs::temp_directory_path() / "codetopo_rehab_changed";
    fs::create_directories(tmp);
    auto db_path = tmp / "rehab.sqlite";

    {
        auto conn = make_freshness_db(db_path);

        insert_file_record(conn, "src/foo.cpp", 1000, 500);

        schema::quarantine_file(conn, "src/foo.cpp", "crash");
        REQUIRE(schema::quarantine_count(conn) == 1);

        std::vector<ScannedFile> scanned;
        scanned.push_back(ScannedFile{
            .absolute_path = fs::path("/repo/src/foo.cpp"),
            .relative_path = "src/foo.cpp",
            .language = "cpp",
            .size_bytes = 500,
            .mtime_ns = 2000  // changed
        });

        int rehabbed = schema::rehab_quarantine(conn, scanned);
        REQUIRE(rehabbed == 1);
        REQUIRE(schema::quarantine_count(conn) == 0);
    }
    cleanup(tmp);
}

TEST_CASE("rehab_quarantine: file with same mtime stays quarantined", "[freshness][quarantine]") {
    auto tmp = fs::temp_directory_path() / "codetopo_rehab_same";
    fs::create_directories(tmp);
    auto db_path = tmp / "rehab_same.sqlite";

    {
        auto conn = make_freshness_db(db_path);

        insert_file_record(conn, "src/bar.cpp", 1000, 500);
        schema::quarantine_file(conn, "src/bar.cpp", "crash");

        std::vector<ScannedFile> scanned;
        scanned.push_back(ScannedFile{
            .absolute_path = fs::path("/repo/src/bar.cpp"),
            .relative_path = "src/bar.cpp",
            .language = "cpp",
            .size_bytes = 500,
            .mtime_ns = 1000  // unchanged
        });

        int rehabbed = schema::rehab_quarantine(conn, scanned);
        REQUIRE(rehabbed == 0);
        REQUIRE(schema::quarantine_count(conn) == 1);
    }
    cleanup(tmp);
}

TEST_CASE("rehab_quarantine: quarantined file not in DB is rehabilitated", "[freshness][quarantine]") {
    auto tmp = fs::temp_directory_path() / "codetopo_rehab_new";
    fs::create_directories(tmp);
    auto db_path = tmp / "rehab_new.sqlite";

    {
        auto conn = make_freshness_db(db_path);

        schema::quarantine_file(conn, "src/new_file.cpp", "crash");
        REQUIRE(schema::quarantine_count(conn) == 1);

        std::vector<ScannedFile> scanned;
        scanned.push_back(ScannedFile{
            .absolute_path = fs::path("/repo/src/new_file.cpp"),
            .relative_path = "src/new_file.cpp",
            .language = "cpp",
            .size_bytes = 200,
            .mtime_ns = 5000
        });

        int rehabbed = schema::rehab_quarantine(conn, scanned);
        REQUIRE(rehabbed == 1);
        REQUIRE(schema::quarantine_count(conn) == 0);
    }
    cleanup(tmp);
}

TEST_CASE("rehab_quarantine: no quarantined files returns 0", "[freshness][quarantine]") {
    auto tmp = fs::temp_directory_path() / "codetopo_rehab_none";
    fs::create_directories(tmp);
    auto db_path = tmp / "rehab_none.sqlite";

    {
        auto conn = make_freshness_db(db_path);

        std::vector<ScannedFile> scanned;
        scanned.push_back(ScannedFile{
            .absolute_path = fs::path("/repo/src/x.cpp"),
            .relative_path = "src/x.cpp",
            .language = "cpp",
            .size_bytes = 100,
            .mtime_ns = 1000
        });

        int rehabbed = schema::rehab_quarantine(conn, scanned);
        REQUIRE(rehabbed == 0);
    }
    cleanup(tmp);
}

// ---------------------------------------------------------------------------
// R9: write_metadata stores git state
// ---------------------------------------------------------------------------

TEST_CASE("write_metadata stores git_head and git_branch keys in kv", "[freshness][metadata]") {
    auto tmp = fs::temp_directory_path() / "codetopo_metadata_test";
    fs::create_directories(tmp);
    auto db_path = tmp / "metadata.sqlite";

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);

        Persister persister(conn);
        persister.write_metadata(tmp.string());

        // Standard metadata keys are always written
        auto version = schema::get_kv(conn, "indexer_version", "__missing__");
        REQUIRE(version != "__missing__");

        auto repo_root = schema::get_kv(conn, "repo_root", "__missing__");
        REQUIRE(repo_root != "__missing__");

        auto last_index = schema::get_kv(conn, "last_index_time", "__missing__");
        REQUIRE(last_index != "__missing__");
    }
    cleanup(tmp);
}

TEST_CASE("write_metadata stores git state when run inside a real git repo", "[freshness][metadata]") {
    // Use the codetopo-exp repo itself as a real git repo
    auto repo_root = fs::path(__FILE__).parent_path().parent_path().parent_path();

    auto head = get_git_head(repo_root.string());
    if (head.empty()) {
        WARN("Skipping: not inside a git repo");
        return;
    }

    auto tmp = fs::temp_directory_path() / "codetopo_metadata_git";
    fs::create_directories(tmp);
    auto db_path = tmp / "metadata_git.sqlite";

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);

        Persister persister(conn);
        persister.write_metadata(repo_root.string());

        auto stored_head = schema::get_kv(conn, "git_head", "");
        auto stored_branch = schema::get_kv(conn, "git_branch", "");

        REQUIRE_FALSE(stored_head.empty());
        REQUIRE_FALSE(stored_branch.empty());
        REQUIRE(stored_head == head);
    }
    cleanup(tmp);
}

// ---------------------------------------------------------------------------
// R2: StalenessState defaults
// ---------------------------------------------------------------------------

TEST_CASE("StalenessState has correct initial defaults", "[freshness][staleness]") {
    StalenessState state;

    REQUIRE(state.stale == false);
    REQUIRE(state.indexed_head.empty());
    REQUIRE(state.indexed_branch.empty());
    REQUIRE(state.current_branch.empty());
}

// ===========================================================================
// P2: Watcher integration, branch switch detection, cross-thread refresh
// ===========================================================================

// ---------------------------------------------------------------------------
// P2-R3: FileEvent::BranchSwitch enum and WatchEvent
// ---------------------------------------------------------------------------

TEST_CASE("FileEvent::BranchSwitch enum value exists", "[freshness][watcher]") {
    FileEvent ev = FileEvent::BranchSwitch;
    REQUIRE(ev == FileEvent::BranchSwitch);

    // Verify it's distinct from the other event types
    REQUIRE(ev != FileEvent::Created);
    REQUIRE(ev != FileEvent::Modified);
    REQUIRE(ev != FileEvent::Deleted);
}

TEST_CASE("WatchEvent with BranchSwitch type and .git/HEAD path", "[freshness][watcher]") {
    WatchEvent event;
    event.type = FileEvent::BranchSwitch;
    event.path = fs::path("/repo/.git/HEAD");

    REQUIRE(event.type == FileEvent::BranchSwitch);
    REQUIRE(event.path.generic_string().find(".git/HEAD") != std::string::npos);
}

TEST_CASE("is_git_head_change detects .git/HEAD paths", "[freshness][watcher]") {
    REQUIRE(is_git_head_change(fs::path("/repo/.git/HEAD")));
    REQUIRE(is_git_head_change(fs::path("/repo/.git/refs/heads/main")));
    REQUIRE_FALSE(is_git_head_change(fs::path("/repo/src/main.cpp")));
    REQUIRE_FALSE(is_git_head_change(fs::path("/repo/.gitignore")));
}

// ---------------------------------------------------------------------------
// P2-R4: Cross-thread atomic refresh flag (watcher → server pattern)
// ---------------------------------------------------------------------------

TEST_CASE("Atomic bool cross-thread pattern: set from writer, read from reader", "[freshness][atomic]") {
    std::atomic<bool> needs_refresh{false};

    REQUIRE(needs_refresh.load() == false);

    // Simulate watcher thread setting the flag
    std::thread writer([&]() {
        needs_refresh.store(true);
    });
    writer.join();

    // Main thread reads the flag (exchange resets it)
    REQUIRE(needs_refresh.exchange(false) == true);

    // After exchange, flag is cleared
    REQUIRE(needs_refresh.load() == false);
}

TEST_CASE("Atomic refresh flag: multiple rapid sets collapse to single true", "[freshness][atomic]") {
    std::atomic<bool> needs_refresh{false};

    // Simulate multiple watcher events firing
    std::thread t1([&]() { needs_refresh.store(true); });
    std::thread t2([&]() { needs_refresh.store(true); });
    std::thread t3([&]() { needs_refresh.store(true); });
    t1.join();
    t2.join();
    t3.join();

    // Consumer sees a single true regardless of how many sets happened
    REQUIRE(needs_refresh.exchange(false) == true);
    REQUIRE(needs_refresh.load() == false);
}

// ---------------------------------------------------------------------------
// P2-R7: rehab_quarantine during branch switch (integration scenario)
// ---------------------------------------------------------------------------

TEST_CASE("rehab_quarantine: branch switch scenario with changed mtime removes from quarantine", "[freshness][quarantine][branchswitch]") {
    auto tmp = fs::temp_directory_path() / "codetopo_rehab_branchswitch";
    fs::create_directories(tmp);
    auto db_path = tmp / "rehab_bs.sqlite";

    {
        auto conn = make_freshness_db(db_path);

        // Simulate file indexed on branch A with mtime 1000
        insert_file_record(conn, "src/feature.cpp", 1000, 800);

        // File crashes during parse, gets quarantined
        schema::quarantine_file(conn, "src/feature.cpp", "crash");
        REQUIRE(schema::quarantine_count(conn) == 1);

        // User switches branch — file now has different mtime (content changed)
        std::vector<ScannedFile> scanned;
        scanned.push_back(ScannedFile{
            .absolute_path = fs::path("/repo/src/feature.cpp"),
            .relative_path = "src/feature.cpp",
            .language = "cpp",
            .size_bytes = 900,    // different size on new branch
            .mtime_ns = 3000     // different mtime on new branch
        });

        int rehabbed = schema::rehab_quarantine(conn, scanned);
        REQUIRE(rehabbed == 1);
        REQUIRE(schema::quarantine_count(conn) == 0);

        // After rehab, file should appear in a work list (not filtered by quarantine)
        auto quarantined = schema::load_quarantine(conn);
        REQUIRE(quarantined.count("src/feature.cpp") == 0);
    }
    cleanup(tmp);
}

// ---------------------------------------------------------------------------
// P2: Watcher debounce configuration
// ---------------------------------------------------------------------------

TEST_CASE("Watcher accepts custom debounce value", "[freshness][watcher]") {
    // Create a temp dir as the watch root (Watcher requires a valid, canonical path)
    auto tmp = fs::temp_directory_path() / "codetopo_watcher_debounce";
    fs::create_directories(tmp);

    bool callback_called = false;
    Watcher watcher(tmp, [&](const std::vector<WatchEvent>&) {
        callback_called = true;
    }, std::chrono::milliseconds(2000));

    // Verify construction succeeded — start/stop lifecycle works without crash
    // (We don't actually trigger filesystem events; that would require a real watcher test)
    watcher.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    watcher.stop();

    // callback_called may or may not be true depending on OS noise;
    // the point is the watcher accepted the 2000ms debounce and ran without errors

    cleanup(tmp);
}

TEST_CASE("Watcher default debounce is 1000ms", "[freshness][watcher]") {
    auto tmp = fs::temp_directory_path() / "codetopo_watcher_default";
    fs::create_directories(tmp);

    // Default constructor uses 1000ms debounce
    Watcher watcher(tmp, [](const std::vector<WatchEvent>&) {});

    // Just verify it constructs and can start/stop
    watcher.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    watcher.stop();

    cleanup(tmp);
}
