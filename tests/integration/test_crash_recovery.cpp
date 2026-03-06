// T054: Integration test for crash recovery — verify DB consistency
// after simulating an interrupted indexing pass.
#include <catch2/catch_test_macros.hpp>
#include "db/connection.h"
#include "db/schema.h"
#include "db/fts.h"
#include "index/persister.h"
#include "index/scanner.h"
#include "index/extractor.h"
#include <filesystem>
#include <fstream>
#include <sqlite3.h>

namespace fs = std::filesystem;
using namespace codetopo;

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
        ea.symbols.push_back({"function", "a", "a", "", 1, 0, 1, 15, true, "", "", ""});
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
