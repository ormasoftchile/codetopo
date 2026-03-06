// T057a: Unit test for FTS5 search quality
#include <catch2/catch_test_macros.hpp>
#include "db/connection.h"
#include "db/schema.h"
#include "db/fts.h"
#include <sqlite3.h>
#include <filesystem>

namespace fs = std::filesystem;
using namespace codetopo;

TEST_CASE("FTS5 search finds symbols by partial name", "[unit][us1]") {
    auto tmp = fs::temp_directory_path() / "codetopo_fts_test";
    fs::create_directories(tmp);
    auto db_path = tmp / "fts_test.sqlite";

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);
        fts::create_sync_triggers(conn);

        // Insert a file
        conn.exec("INSERT INTO files(path, language, size_bytes, mtime_ns, content_hash, parse_status) "
                  "VALUES('test.cpp', 'cpp', 100, 1000, 'abc', 'ok')");
        int64_t file_id = sqlite3_last_insert_rowid(conn.raw());

        // Insert symbols with known names
        auto insert = [&](const char* name, const char* qualname, const char* kind) {
            sqlite3_stmt* stmt = nullptr;
            sqlite3_prepare_v2(conn.raw(),
                "INSERT INTO nodes(node_type, file_id, kind, name, qualname, stable_key) "
                "VALUES('symbol', ?, ?, ?, ?, ?)", -1, &stmt, nullptr);
            sqlite3_bind_int64(stmt, 1, file_id);
            sqlite3_bind_text(stmt, 2, kind, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, name, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 4, qualname, -1, SQLITE_STATIC);
            std::string key = std::string("test.cpp::") + kind + "::" + name;
            sqlite3_bind_text(stmt, 5, key.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        };

        insert("calculateDistance", "math::calculateDistance", "function");
        insert("calculateArea", "math::calculateArea", "function");
        insert("Point", "math::Point", "struct");
        insert("Line", "math::Line", "class");

        // Rebuild FTS index
        fts::rebuild(conn);
    }

    // Query FTS
    {
        Connection conn(db_path, true);

        // Search for "calculate" — should find both calculate* functions
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(conn.raw(),
            "SELECT name FROM nodes_fts WHERE nodes_fts MATCH 'calculate*'",
            -1, &stmt, nullptr);

        int count = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            count++;
        }
        sqlite3_finalize(stmt);
        REQUIRE(count >= 2);

        // Search for "Point" — should find the struct
        sqlite3_prepare_v2(conn.raw(),
            "SELECT name FROM nodes_fts WHERE nodes_fts MATCH 'Point'",
            -1, &stmt, nullptr);
        bool found = sqlite3_step(stmt) == SQLITE_ROW;
        sqlite3_finalize(stmt);
        REQUIRE(found);

        // Search for "math" in qualname — should find symbols in math namespace
        sqlite3_prepare_v2(conn.raw(),
            "SELECT name FROM nodes_fts WHERE nodes_fts MATCH 'math*'",
            -1, &stmt, nullptr);
        count = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW) { count++; }
        sqlite3_finalize(stmt);
        REQUIRE(count >= 2);
    }

    fs::remove_all(tmp);
}
