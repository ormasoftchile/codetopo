#include <catch2/catch_test_macros.hpp>
#include "db/connection.h"
#include "db/bind.h"
#include <sqlite3.h>

using namespace codetopo;

TEST_CASE("C++26 pack-indexed statement binder", "[db][bind]") {
    Connection conn(":memory:");
    conn.exec("CREATE TABLE test_types ("
              "  id INTEGER PRIMARY KEY,"
              "  i_val INTEGER,"
              "  i64_val INTEGER,"
              "  b_val INTEGER,"
              "  d_val REAL,"
              "  s_val TEXT,"
              "  sv_val TEXT,"
              "  opt_val TEXT,"
              "  null_val TEXT"
              ")");

    sqlite3_stmt* insert_stmt = nullptr;
    int rc = sqlite3_prepare_v2(
        conn.raw(),
        "INSERT INTO test_types (i_val, i64_val, b_val, d_val, s_val, sv_val, opt_val, null_val) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
        -1, &insert_stmt, nullptr);
    REQUIRE(rc == SQLITE_OK);

    std::string str = "hello string";
    std::string_view sv = "hello view";
    std::optional<std::string> opt_present = "present";
    std::optional<std::string> opt_missing = std::nullopt;

    // Row 1: all populated
    db::bind(insert_stmt, 42, 9876543210LL, true, 3.14159, str, sv, opt_present, nullptr);
    REQUIRE(sqlite3_step(insert_stmt) == SQLITE_DONE);

    // Row 2: optional missing and bool false
    db::bind(insert_stmt, 0, 0LL, false, 0.0, "", "", opt_missing, nullptr);
    REQUIRE(sqlite3_step(insert_stmt) == SQLITE_DONE);

    sqlite3_finalize(insert_stmt);

    // Query back Row 1
    sqlite3_stmt* query_stmt = nullptr;
    rc = sqlite3_prepare_v2(conn.raw(),
        "SELECT i_val, i64_val, b_val, d_val, s_val, sv_val, opt_val, null_val "
        "FROM test_types WHERE id = 1",
        -1, &query_stmt, nullptr);
    REQUIRE(rc == SQLITE_OK);
    REQUIRE(sqlite3_step(query_stmt) == SQLITE_ROW);

    CHECK(sqlite3_column_int(query_stmt, 0) == 42);
    CHECK(sqlite3_column_int64(query_stmt, 1) == 9876543210LL);
    CHECK(sqlite3_column_int(query_stmt, 2) == 1);
    CHECK(sqlite3_column_double(query_stmt, 3) == 3.14159);
    CHECK(std::string(reinterpret_cast<const char*>(sqlite3_column_text(query_stmt, 4))) == "hello string");
    CHECK(std::string(reinterpret_cast<const char*>(sqlite3_column_text(query_stmt, 5))) == "hello view");
    CHECK(std::string(reinterpret_cast<const char*>(sqlite3_column_text(query_stmt, 6))) == "present");
    CHECK(sqlite3_column_type(query_stmt, 7) == SQLITE_NULL);

    sqlite3_finalize(query_stmt);
}
