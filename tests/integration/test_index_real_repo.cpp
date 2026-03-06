// T051-T054: Integration tests for indexing a real repository.
#include <catch2/catch_test_macros.hpp>
#include "core/config.h"
#include "core/arena.h"
#include "db/connection.h"
#include "db/schema.h"
#include "db/fts.h"
#include "index/scanner.h"
#include "index/change_detector.h"
#include "index/parser.h"
#include "index/extractor.h"
#include "index/persister.h"
#include "util/hash.h"
#include "core/arena_pool.h"
#include <filesystem>
#include <fstream>
#include <sqlite3.h>

namespace fs = std::filesystem;
using namespace codetopo;

// Forward decl (defined in core/arena.cpp)
namespace codetopo {
    void set_thread_arena(Arena* arena);
    void register_arena_allocator();
}

// Helper: run a full index pass on a directory, return the db path
static fs::path index_repo(const fs::path& repo_root, const fs::path& db_path) {
    register_arena_allocator();

    Config cfg;
    cfg.repo_root = repo_root;
    cfg.db_path = db_path;
    cfg.no_gitignore = true;  // Don't filter for test reproducibility

    Connection conn(db_path);
    schema::ensure_schema(conn);
    fts::create_sync_triggers(conn);

    Scanner scanner(cfg);
    auto scanned = scanner.scan();

    ChangeDetector detector(conn);
    auto changes = detector.detect(scanned);

    Persister persister(conn);
    persister.prune_deleted(changes.deleted_paths);

    ArenaPool arena_pool(1, 64 * 1024 * 1024);  // 1 thread, 64MB arena

    auto work_list = changes.new_files;
    work_list.insert(work_list.end(), changes.changed_files.begin(), changes.changed_files.end());

    for (auto& file : work_list) {
        auto lease = ArenaLease(arena_pool);
        set_thread_arena(lease.get());

        auto content = read_file_content(file.absolute_path);
        if (content.empty()) continue;

        auto content_hash = hash_string(content);

        Parser parser;
        if (!parser.set_language(file.language)) {
            persister.persist_file(file, ExtractionResult{}, content_hash, "skipped", "grammar not available");
            continue;
        }

        auto tree = TreeGuard(parser.parse(content));
        if (!tree) {
            persister.persist_file(file, ExtractionResult{}, content_hash, "failed", "parse failed");
            continue;
        }

        Extractor extractor(cfg.max_symbols_per_file, cfg.max_ast_depth);
        auto extraction = extractor.extract(tree.tree, content, file.language, file.relative_path);

        std::string status = extraction.truncated ? "partial" : "ok";
        persister.persist_file(file, extraction, content_hash, status,
                               extraction.truncated ? extraction.truncation_reason : "");
    }

    persister.write_metadata(repo_root.string());
    conn.wal_checkpoint();
    return db_path;
}

static int count_rows(Connection& conn, const char* table) {
    std::string sql = std::string("SELECT COUNT(*) FROM ") + table;
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(conn.raw(), sql.c_str(), -1, &stmt, nullptr);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

// T051: Index the codetopo repo, verify nonzero counts, spot-check a known symbol
TEST_CASE("Index real repo produces nonzero counts", "[integration][us1]") {
    auto tmp = fs::temp_directory_path() / "codetopo_test_index_real";
    fs::create_directories(tmp);
    auto db_path = tmp / "test.sqlite";
    fs::remove(db_path);

    // Index the codetopo repo root (our own source code)
    auto repo_root = fs::current_path();
    index_repo(repo_root, db_path);

    // Verify (scoped so Connection closes before cleanup)
    {
        Connection conn(db_path, true);  // read-only
        int file_count = count_rows(conn, "files");
        int node_count = count_rows(conn, "nodes");
        int edge_count = count_rows(conn, "edges");

        REQUIRE(file_count > 0);
        REQUIRE(node_count > 0);
        REQUIRE(edge_count > 0);

        // Spot-check: look for "Arena" class/struct in nodes
        {
            sqlite3_stmt* stmt = nullptr;
            sqlite3_prepare_v2(conn.raw(),
                "SELECT name, kind FROM nodes WHERE name = 'Arena' AND node_type = 'symbol'",
                -1, &stmt, nullptr);
            bool found = sqlite3_step(stmt) == SQLITE_ROW;
            sqlite3_finalize(stmt);
            REQUIRE(found);
        }

        // Spot-check: verify kv metadata exists
        {
            // cast away const for get_kv helper
            auto& mutable_conn = const_cast<Connection&>(conn);
            auto version = schema::get_kv(mutable_conn, "schema_version");
            REQUIRE(version == "1");
            auto idx_version = schema::get_kv(mutable_conn, "indexer_version");
            REQUIRE(idx_version == "1.0.0");
        }
    }

    fs::remove_all(tmp);
}

// T052: Index, modify a file, re-index, verify incremental
TEST_CASE("Incremental re-index detects changes", "[integration][us1]") {
    auto tmp = fs::temp_directory_path() / "codetopo_test_incremental";
    fs::create_directories(tmp / "src");
    auto db_path = tmp / "test.sqlite";

    // Create initial file
    {
        std::ofstream f(tmp / "src" / "test.cpp");
        f << "void foo() {}\n";
    }

    // First index
    index_repo(tmp, db_path);

    {
        Connection conn(db_path, true);
        int files = count_rows(conn, "files");
        REQUIRE(files == 1);
    }

    // Modify the file — add a function
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    {
        std::ofstream f(tmp / "src" / "test.cpp");
        f << "void foo() {}\nvoid bar() {}\n";
    }

    // Re-index
    {
        Connection conn(db_path);
        schema::ensure_schema(conn);
        fts::create_sync_triggers(conn);

        Config cfg;
        cfg.repo_root = tmp;
        cfg.db_path = db_path;
        cfg.no_gitignore = true;

        Scanner scanner(cfg);
        auto scanned = scanner.scan();

        ChangeDetector detector(conn);
        auto changes = detector.detect(scanned);

        // File should be detected as changed
        REQUIRE(changes.changed_files.size() + changes.new_files.size() >= 1);

        Persister persister(conn);
        ArenaPool pool(1, 16 * 1024 * 1024);

        auto work = changes.new_files;
        work.insert(work.end(), changes.changed_files.begin(), changes.changed_files.end());

        for (auto& file : work) {
            auto lease = ArenaLease(pool);
            set_thread_arena(lease.get());

            auto content = read_file_content(file.absolute_path);
            auto hash = hash_string(content);

            Parser parser;
            parser.set_language(file.language);
            auto tree = TreeGuard(parser.parse(content));
            if (!tree) continue;

            Extractor ext(50000, 500);
            auto result = ext.extract(tree.tree, content, file.language, file.relative_path);
            persister.persist_file(file, result, hash, "ok");
        }
    }

    // Verify bar exists in the updated index
    {
        Connection conn(db_path, true);
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(conn.raw(),
            "SELECT name FROM nodes WHERE name = 'bar' AND kind = 'function'",
            -1, &stmt, nullptr);
        bool found = sqlite3_step(stmt) == SQLITE_ROW;
        sqlite3_finalize(stmt);
        REQUIRE(found);
    }

    fs::remove_all(tmp);
}

// T053: Index, delete a file, re-index, verify pruned
TEST_CASE("Deleted file is pruned from index", "[integration][us1]") {
    auto tmp = fs::temp_directory_path() / "codetopo_test_prune";
    fs::create_directories(tmp / "src");
    auto db_path = tmp / "test.sqlite";

    // Create two files
    {
        std::ofstream(tmp / "src" / "a.cpp") << "void a_func() {}\n";
        std::ofstream(tmp / "src" / "b.cpp") << "void b_func() {}\n";
    }

    // Index both
    index_repo(tmp, db_path);

    {
        Connection conn(db_path, true);
        REQUIRE(count_rows(conn, "files") == 2);
    }

    // Delete b.cpp
    fs::remove(tmp / "src" / "b.cpp");

    // Re-index — should prune b.cpp
    index_repo(tmp, db_path);

    {
        Connection conn(db_path, true);
        REQUIRE(count_rows(conn, "files") == 1);

        // b_func should be gone
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(conn.raw(),
            "SELECT COUNT(*) FROM nodes WHERE name = 'b_func'", -1, &stmt, nullptr);
        sqlite3_step(stmt);
        int count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        REQUIRE(count == 0);
    }

    fs::remove_all(tmp);
}
