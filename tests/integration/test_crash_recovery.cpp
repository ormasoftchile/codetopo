// T054: Integration test for crash recovery — verify DB consistency
// after simulating an interrupted indexing pass.
#include <catch2/catch_test_macros.hpp>
#include "db/connection.h"
#include "db/schema.h"
#include "db/fts.h"
#include "index/persister.h"
#include "index/scanner.h"
#include "index/extractor.h"
#include "index/supervisor.h"
#include "util/process.h"
#include "util/seh.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sqlite3.h>

namespace fs = std::filesystem;
using namespace codetopo;

TEST_CASE("SEH translation is gated by compiler support", "[contract][crash]") {
#if defined(_WIN32) && defined(_MSC_VER)
    STATIC_REQUIRE(CODETOPO_HAS_SEH_TRANSLATOR == 1);
    REQUIRE_THROWS_AS(seh_translator(EXCEPTION_ACCESS_VIOLATION, nullptr), SehException);
#else
    STATIC_REQUIRE(CODETOPO_HAS_SEH_TRANSLATOR == 0);
#endif
}

#ifdef _WIN32
TEST_CASE("Supervisor quarantines a native worker fault and resumes real indexing",
          "[integration][supervisor][crash]") {
    auto exe_dir = fs::path(get_self_executable_path()).parent_path();
    auto child = exe_dir / "codetopo_crash_child.exe";
    REQUIRE(fs::exists(child));
    auto root = exe_dir / ("crash-recovery-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    struct Cleanup {
        fs::path root;
        ~Cleanup() { std::error_code ec; fs::remove_all(root, ec); }
    } cleanup{root};
    for (const char* name : {"committed.c", "fault.c", "inflight1.c", "inflight2.c", "remaining.c"}) {
        std::ofstream(root / name) << "int example(void) { return 0; }\n";
    }
    Config cfg;
    cfg.repo_root = root;
    cfg.db_path = root / "index.sqlite";
    cfg.thread_count = 1;
    cfg.batch_size = 1;
    cfg.arena_size_mb = 16;
    cfg.no_gitignore = true;
    REQUIRE(run_index_supervisor(cfg, child.string()) == 0);
    std::ifstream attempts(root / "attempts.log");
    std::string first, second, extra;
    REQUIRE(static_cast<bool>(std::getline(attempts, first)));
    REQUIRE(static_cast<bool>(std::getline(attempts, second)));
    REQUIRE(first == "initial");
    REQUIRE(second == "resume");
    REQUIRE_FALSE(static_cast<bool>(std::getline(attempts, extra)));
    {
        Connection conn(cfg.db_path, true);
        auto quarantined = schema::load_quarantine(conn);
        REQUIRE(quarantined.size() == 3);
        REQUIRE(quarantined.count("fault.c") == 1);
        REQUIRE(quarantined.count("inflight1.c") == 1);
        REQUIRE(quarantined.count("inflight2.c") == 1);
        sqlite3_stmt* raw = nullptr;
        REQUIRE(sqlite3_prepare_v2(conn.raw(), "SELECT parse_status FROM files WHERE path = ?",
                                  -1, &raw, nullptr) == SQLITE_OK);
        auto stmt = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>(raw, sqlite3_finalize);
        for (const char* name : {"committed.c", "remaining.c"}) {
            sqlite3_bind_text(stmt.get(), 1, name, -1, SQLITE_TRANSIENT);
            REQUIRE(sqlite3_step(stmt.get()) == SQLITE_ROW);
            REQUIRE(std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0))) == "ok");
            sqlite3_reset(stmt.get());
        }
        REQUIRE(conn.integrity_check() == "ok");
    }
    REQUIRE_FALSE(fs::exists(cfg.db_path.string() + ".worklist"));
    REQUIRE_FALSE(fs::exists(cfg.db_path.string() + ".progress"));
}
#endif

TEST_CASE("DB remains consistent after partial batch", "[integration][us1]") {
    auto tmp = fs::temp_directory_path() / "codetopo_crash_test";
    fs::create_directories(tmp / "src");
    auto db_path = tmp / "test.sqlite";

    // Create 3 files
    { std::ofstream(tmp / "src" / "a.c") << "void a() {}\n"; }
    { std::ofstream(tmp / "src" / "b.c") << "void b() {}\n"; }
    { std::ofstream(tmp / "src" / "c.c") << "void c() {}\n"; }

    // Index first file normally, then simulate crash by not committing batch
    {
        Connection conn(db_path);
        schema::ensure_schema(conn);

        // Persist file A successfully
        ScannedFile fa;
        fa.relative_path = "src/a.c";
        fa.language = "c";
        fa.size_bytes = 12;
        fa.mtime_ns = 1000;
        fa.absolute_path = tmp / "src" / "a.c";

        ExtractionResult ea;
        ea.symbols.push_back({"function", "a", "a", "", "", 1, 0, 1, 15, true, "", "", ""});
        ea.symbols[0].stable_key = "src/a.c::function::a";

        Persister persister(conn);
        REQUIRE(persister.persist_file(fa, ea, "hash_a", "ok"));

        // Now simulate a crash: start a transaction for file B but don't commit
        conn.exec("BEGIN TRANSACTION");
        conn.exec("INSERT INTO files(path, language, size_bytes, mtime_ns, content_hash, parse_status) "
                  "VALUES('src/b.c', 'c', 12, 1000, 'hash_b', 'ok')");
        // DON'T commit — simulate crash by closing connection
        // SQLite rolls back uncommitted transactions on close
        conn.exec("ROLLBACK");
    }

    // Verify DB integrity after "crash"
    {
        Connection conn(db_path, true);

        // Integrity should be ok
        REQUIRE(conn.integrity_check() == "ok");
        REQUIRE(conn.foreign_key_check() == 0);

        // File A should exist (committed)
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(conn.raw(),
            "SELECT COUNT(*) FROM files WHERE path = 'src/a.c'", -1, &stmt, nullptr);
        sqlite3_step(stmt);
        REQUIRE(sqlite3_column_int(stmt, 0) == 1);
        sqlite3_finalize(stmt);

        // File B should NOT exist (rolled back)
        sqlite3_prepare_v2(conn.raw(),
            "SELECT COUNT(*) FROM files WHERE path = 'src/b.c'", -1, &stmt, nullptr);
        sqlite3_step(stmt);
        REQUIRE(sqlite3_column_int(stmt, 0) == 0);
        sqlite3_finalize(stmt);

        // No orphan edges
        sqlite3_prepare_v2(conn.raw(),
            "SELECT COUNT(*) FROM edges e LEFT JOIN nodes n1 ON e.src_id = n1.id "
            "LEFT JOIN nodes n2 ON e.dst_id = n2.id WHERE n1.id IS NULL OR n2.id IS NULL",
            -1, &stmt, nullptr);
        sqlite3_step(stmt);
        REQUIRE(sqlite3_column_int(stmt, 0) == 0);
        sqlite3_finalize(stmt);
    }

    fs::remove_all(tmp);
}
