// Unit tests for Git worktree topology detection and HEAD resolution.

#include <catch2/catch_test_macros.hpp>
#include "util/git.h"
#include "db/cow_seed.h"
#include "index/worktree_reconcile.h"
#include "db/connection.h"
#include "db/schema.h"
#include "index/ownership.h"
#include "cli/cmd_index.h"
#include "cli/cmd_worktree.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <chrono>

namespace fs = std::filesystem;
using namespace codetopo;

#ifdef _WIN32
static constexpr const char* kDevNull = " >NUL 2>NUL";
#else
static constexpr const char* kDevNull = " >/dev/null 2>/dev/null";
#endif

namespace {

void cleanup(const fs::path& p) {
    std::error_code ec;
    if (fs::exists(p, ec)) {
        for (fs::recursive_directory_iterator it(p, ec), end; !ec && it != end; it.increment(ec)) {
            fs::permissions(it->path(), fs::perms::owner_write, fs::perm_options::add, ec);
            if (ec) ec.clear();
        }
    }
    fs::remove_all(p, ec);
}

std::string quote_path(const fs::path& p) {
    std::string s = p.string();
#ifdef _WIN32
    return "\"" + s + "\"";
#else
    return "'" + s + "'";
#endif
}

int run_cmd(const std::string& cmd) {
    return std::system(cmd.c_str());
}

} // namespace

TEST_CASE("resolve_worktree_info on non-git directory", "[worktree]") {
    auto tmp = fs::current_path() / "build" / "test_wt_nongit";
    cleanup(tmp);
    fs::create_directories(tmp);

    auto info = resolve_worktree_info(tmp);
    CHECK_FALSE(info.is_git_repo);
    CHECK_FALSE(info.is_worktree);
    CHECK(info.head_file_path.empty());
    CHECK(get_git_head_path(tmp).empty());

    cleanup(tmp);
}

TEST_CASE("resolve_worktree_info and HEAD resolution on standard git repo and linked worktree", "[worktree]") {
    auto base_dir = fs::current_path() / "build" / "test_wt_fixture";
    cleanup(base_dir);
    fs::create_directories(base_dir);

    auto main_repo = base_dir / "main_repo";
    auto wt_repo = base_dir / "wt_repo";
    fs::create_directories(main_repo);

    // Initialize git repo
    REQUIRE(run_cmd("git -C " + quote_path(main_repo) + " init" + kDevNull) == 0);
    REQUIRE(run_cmd("git -C " + quote_path(main_repo) + " config user.email test@example.com") == 0);
    REQUIRE(run_cmd("git -C " + quote_path(main_repo) + " config user.name Tester") == 0);

    // 1. Before any commit (unborn HEAD)
    auto main_info = resolve_worktree_info(main_repo);
    CHECK(main_info.is_git_repo);
    CHECK_FALSE(main_info.is_worktree);
    CHECK(fs::equivalent(main_info.worktree_root, main_repo));
    CHECK(fs::equivalent(main_info.primary_repo_root, main_repo));
    CHECK(fs::equivalent(main_info.git_dir, main_repo / ".git"));
    CHECK(fs::equivalent(main_info.common_git_dir, main_repo / ".git"));
    CHECK(fs::equivalent(main_info.head_file_path, main_repo / ".git" / "HEAD"));
    CHECK(fs::equivalent(get_git_head_path(main_repo), main_repo / ".git" / "HEAD"));

    // Create an initial commit
    {
        std::ofstream(main_repo / "README.md") << "# Hello\n";
    }
    REQUIRE(run_cmd("git -C " + quote_path(main_repo) + " add README.md") == 0);
    REQUIRE(run_cmd("git -C " + quote_path(main_repo) + " commit -m initial" + kDevNull) == 0);

    auto initial_head = get_git_head(main_repo.string());
    CHECK_FALSE(initial_head.empty());

    // 2. Create a linked worktree
    REQUIRE(run_cmd("git -C " + quote_path(main_repo) + " worktree add " +
                    quote_path(wt_repo) + " -b feat-test" + kDevNull) == 0);

    // In a worktree, .git must be a regular file
    CHECK(fs::is_regular_file(wt_repo / ".git"));

    auto wt_info = resolve_worktree_info(wt_repo);
    CHECK(wt_info.is_git_repo);
    CHECK(wt_info.is_worktree);
    CHECK(fs::equivalent(wt_info.worktree_root, wt_repo));
    CHECK(fs::equivalent(wt_info.primary_repo_root, main_repo));
    CHECK(fs::equivalent(wt_info.common_git_dir, main_repo / ".git"));
    CHECK_FALSE(wt_info.git_dir.empty());
    CHECK(wt_info.git_dir.generic_string().find(".git/worktrees/") != std::string::npos);
    CHECK(fs::exists(wt_info.head_file_path));
    CHECK(fs::equivalent(get_git_head_path(wt_repo), wt_info.head_file_path));

    // Verify git branch in worktree
    CHECK(get_git_branch(wt_repo.string()) == "feat-test");
    CHECK(get_git_head(wt_repo.string()) == initial_head);

    // 3. Switching branches in worktree updates head_file_path and its mtime
    auto before_mtime = fs::last_write_time(wt_info.head_file_path);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    REQUIRE(run_cmd("git -C " + quote_path(wt_repo) + " checkout -b feat-switch" + kDevNull) == 0);

    auto after_mtime = fs::last_write_time(wt_info.head_file_path);
    CHECK((after_mtime != before_mtime));
    CHECK(get_git_branch(wt_repo.string()) == "feat-switch");

    // 4. Commits in worktree update worktree head while leaving primary repo head untouched
    {
        std::ofstream(wt_repo / "feature.txt") << "new feature\n";
    }
    REQUIRE(run_cmd("git -C " + quote_path(wt_repo) + " add feature.txt") == 0);
    REQUIRE(run_cmd("git -C " + quote_path(wt_repo) + " commit -m add-feature" + kDevNull) == 0);

    auto new_head = get_git_head(wt_repo.string());
    CHECK_FALSE(new_head.empty());
    CHECK(new_head != initial_head);

    // Primary repo head remains untouched
    CHECK(get_git_head(main_repo.string()) == initial_head);

    // Cleanup worktree via git command first to leave clean git state
    run_cmd("git -C " + quote_path(main_repo) + " worktree remove --force " + quote_path(wt_repo) + kDevNull);
    cleanup(base_dir);
}

TEST_CASE("clone_database_cow copies database and preserves isolation", "[worktree]") {
    auto base_dir = fs::current_path() / "build" / "test_cow_fixture";
    cleanup(base_dir);
    fs::create_directories(base_dir);

    auto src_db = base_dir / "src.sqlite";
    auto dst_db = base_dir / "dst.sqlite";

    {
        Connection conn(src_db.string());
        schema::ensure_schema(conn);
        schema::set_kv(conn, "test_key", "original_value");
        conn.wal_checkpoint();
    }

    REQUIRE(fs::exists(src_db));
    REQUIRE(clone_database_cow(src_db, dst_db));
    REQUIRE(fs::exists(dst_db));

    // Verify dst has the data
    {
        Connection dst_conn(dst_db.string(), true);
        CHECK(schema::get_kv(dst_conn, "test_key", "") == "original_value");
    }

    // Mutate dst, verify src is unaffected
    {
        Connection dst_conn(dst_db.string());
        schema::set_kv(dst_conn, "test_key", "mutated_value");
    }

    {
        Connection src_conn(src_db.string(), true);
        CHECK(schema::get_kv(src_conn, "test_key", "") == "original_value");
        Connection dst_conn(dst_db.string(), true);
        CHECK(schema::get_kv(dst_conn, "test_key", "") == "mutated_value");
    }

    cleanup(base_dir);
}

TEST_CASE("bootstrap_worktree_index and fast reconciliation with divergent code", "[worktree]") {
    auto base_dir = fs::current_path() / "build" / "test_wt_reconcile_fixture";
    cleanup(base_dir);
    fs::create_directories(base_dir);

    auto main_repo = base_dir / "main_repo";
    auto wt_repo = base_dir / "wt_repo";
    fs::create_directories(main_repo / "src");

    // Initialize git repo
    REQUIRE(run_cmd("git -C " + quote_path(main_repo) + " init" + kDevNull) == 0);
    REQUIRE(run_cmd("git -C " + quote_path(main_repo) + " config user.email test@example.com") == 0);
    REQUIRE(run_cmd("git -C " + quote_path(main_repo) + " config user.name Tester") == 0);

    {
        std::ofstream(main_repo / "src" / "main.cpp") << "int main() { return 0; }\n";
        std::ofstream(main_repo / "src" / "utils.cpp") << "int helper() { return 1; }\n";
    }
    REQUIRE(run_cmd("git -C " + quote_path(main_repo) + " add .") == 0);
    REQUIRE(run_cmd("git -C " + quote_path(main_repo) + " commit -m base" + kDevNull) == 0);

    // Run index on main repo
    std::string main_db = default_db(main_repo.string());
    Config main_cfg;
    main_cfg.repo_root = main_repo;
    main_cfg.db_path = main_db;
    ensure_codetopo_dir(main_repo.string());
    REQUIRE(run_index(main_cfg) == 0);
    REQUIRE(fs::exists(main_db));

    // Create worktree
    REQUIRE(run_cmd("git -C " + quote_path(main_repo) + " worktree add " +
                    quote_path(wt_repo) + " -b feat-magic" + kDevNull) == 0);

    // Bootstrap worktree index
    auto boot_res = bootstrap_worktree_index(wt_repo);
    CHECK(boot_res.success);
    CHECK(boot_res.files_changed == 0);

    std::string wt_db = default_db(wt_repo.string());
    REQUIRE(fs::exists(wt_db));

    // Verify ownership is valid for wt_repo
    {
        Connection wt_conn(wt_db, true);
        auto ownership = index_ownership::resolve_primary_root(wt_conn, wt_repo, true, wt_db);
        CHECK(fs::equivalent(ownership.root, wt_repo));
        CHECK(ownership.metadata_present);
        CHECK(schema::get_kv(wt_conn, "worktree_parent_root", "") == fs::canonical(main_repo).string());
    }

    // Now introduce divergent code in worktree: add a new file with symbol
    {
        std::ofstream(wt_repo / "src" / "magic.cpp") << "int worktree_magic() { return 42; }\n";
    }

    // Run fast reconciliation
    auto sync_res = reconcile_worktree_index(wt_repo);
    CHECK(sync_res.success);
    CHECK(sync_res.files_changed == 1);

    // Verify symbol exists in wt_db
    {
        Connection wt_conn(wt_db, true);
        sqlite3_stmt* stmt = nullptr;
        REQUIRE(sqlite3_prepare_v2(wt_conn.raw(),
            "SELECT count(*) FROM nodes WHERE name = 'worktree_magic'", -1, &stmt, nullptr) == SQLITE_OK);
        REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
        CHECK(sqlite3_column_int(stmt, 0) == 1);
        sqlite3_finalize(stmt);
    }

    // Verify symbol DOES NOT exist in main_db (complete isolation)
    {
        Connection main_conn(main_db, true);
        sqlite3_stmt* stmt = nullptr;
        REQUIRE(sqlite3_prepare_v2(main_conn.raw(),
            "SELECT count(*) FROM nodes WHERE name = 'worktree_magic'", -1, &stmt, nullptr) == SQLITE_OK);
        REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
        CHECK(sqlite3_column_int(stmt, 0) == 0);
        sqlite3_finalize(stmt);
    }

    // Cleanup
    run_cmd("git -C " + quote_path(main_repo) + " worktree remove --force " + quote_path(wt_repo) + kDevNull);
    cleanup(base_dir);
}

TEST_CASE("run_worktree_init, sync, and status CLI commands", "[worktree]") {
    auto base_dir = fs::current_path() / "build" / "test_wt_cli_fixture";
    cleanup(base_dir);
    fs::create_directories(base_dir);

    auto main_repo = base_dir / "main_repo";
    auto wt_repo = base_dir / "wt_repo";
    fs::create_directories(main_repo / "src");

    REQUIRE(run_cmd("git -C " + quote_path(main_repo) + " init" + kDevNull) == 0);
    REQUIRE(run_cmd("git -C " + quote_path(main_repo) + " config user.email test@example.com") == 0);
    REQUIRE(run_cmd("git -C " + quote_path(main_repo) + " config user.name Tester") == 0);

    {
        std::ofstream(main_repo / "src" / "main.cpp") << "int main() { return 0; }\n";
    }
    REQUIRE(run_cmd("git -C " + quote_path(main_repo) + " add .") == 0);
    REQUIRE(run_cmd("git -C " + quote_path(main_repo) + " commit -m base" + kDevNull) == 0);

    // Index main repo
    std::string main_db = default_db(main_repo.string());
    Config main_cfg;
    main_cfg.repo_root = main_repo;
    main_cfg.db_path = main_db;
    ensure_codetopo_dir(main_repo.string());
    REQUIRE(run_index(main_cfg) == 0);

    // Create worktree
    REQUIRE(run_cmd("git -C " + quote_path(main_repo) + " worktree add " +
                    quote_path(wt_repo) + " -b feat-cli" + kDevNull) == 0);

    // 1. worktree status before init
    CHECK(run_worktree_status(wt_repo.string()) == 0);

    // 2. worktree init
    CHECK(run_worktree_init(wt_repo.string()) == 0);
    std::string wt_db = default_db(wt_repo.string());
    CHECK(fs::exists(wt_db));

    // 3. worktree status after init
    CHECK(run_worktree_status(wt_repo.string()) == 0);

    // 4. worktree sync
    CHECK(run_worktree_sync(wt_repo.string()) == 0);

    // Cleanup
    run_cmd("git -C " + quote_path(main_repo) + " worktree remove --force " + quote_path(wt_repo) + kDevNull);
    cleanup(base_dir);
}
