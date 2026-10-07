#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "db/connection.h"
#include "db/schema.h"
#include <memory>
#include <string>

using namespace codetopo;

namespace {

void remove_provenance_columns(Connection& conn) {
    conn.exec("DROP INDEX idx_edges_source");
    conn.exec("ALTER TABLE edges DROP COLUMN source");
    conn.exec("ALTER TABLE edges DROP COLUMN observed_count");
    conn.exec("ALTER TABLE edges DROP COLUMN first_seen");
    conn.exec("ALTER TABLE edges DROP COLUMN last_seen");
}

}

TEST_CASE("Legacy schema migration adds columns before creating current indexes",
          "[unit][schema][migration]") {
    const int version = GENERATE(13, 14);
    CAPTURE(version);
    Connection conn(":memory:");
    REQUIRE(schema::ensure_schema(conn) == 0);
    conn.exec("INSERT INTO files(id, path, language, size_bytes, mtime_ns, content_hash, parse_status) "
              "VALUES(1, 'legacy.cpp', 'cpp', 100, 1, 'legacy-hash', 'ok')");
    conn.exec("INSERT INTO nodes(id, node_type, file_id, kind, name, stable_key, rank) "
              "VALUES(10, 'symbol', 1, 'function', 'caller', 'legacy::caller', 0.75), "
              "(11, 'symbol', 1, 'function', 'callee', 'legacy::callee', 0.5)");
    conn.exec("INSERT INTO edges(id, src_id, dst_id, kind, confidence, evidence) "
              "VALUES(20, 10, 11, 'calls', 0.9, 'ast')");
    conn.exec("INSERT INTO nodes_fts(rowid, name) VALUES(10, 'caller'), (11, 'callee')");
    schema::set_kv(conn, "repo_root", "legacy-root");
    if (version == 13) {
        conn.exec("DROP INDEX idx_nodes_rank");
        conn.exec("ALTER TABLE nodes DROP COLUMN rank");
    }
    remove_provenance_columns(conn);
    schema::set_kv(conn, "schema_version", std::to_string(version));

    REQUIRE(schema::table_has_column(conn, "nodes", "rank") == (version == 14));
    REQUIRE_FALSE(schema::table_has_column(conn, "edges", "source"));
    REQUIRE(schema::ensure_schema(conn) == 0);
    REQUIRE(schema::get_schema_version(conn) == CURRENT_SCHEMA_VERSION);
    CHECK(schema::get_kv(conn, "repo_root") == "legacy-root");
    REQUIRE(schema::table_has_column(conn, "nodes", "rank"));
    REQUIRE(schema::table_has_column(conn, "edges", "source"));
    REQUIRE(schema::table_has_column(conn, "edges", "observed_count"));
    REQUIRE(schema::table_has_column(conn, "edges", "first_seen"));
    REQUIRE(schema::table_has_column(conn, "edges", "last_seen"));

    sqlite3_stmt* raw = nullptr;
    REQUIRE(sqlite3_prepare_v2(conn.raw(),
        "SELECT n.stable_key, n.file_id, n.rank, e.src_id, e.dst_id, e.evidence, "
        "e.source, e.observed_count, e.first_seen, e.last_seen "
        "FROM nodes n CROSS JOIN edges e WHERE n.id=10 AND e.id=20",
        -1, &raw, nullptr) == SQLITE_OK);
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> row(raw, sqlite3_finalize);
    REQUIRE(sqlite3_step(row.get()) == SQLITE_ROW);
    CHECK(std::string(reinterpret_cast<const char*>(sqlite3_column_text(row.get(), 0))) == "legacy::caller");
    CHECK(sqlite3_column_int(row.get(), 1) == 1);
    CHECK(sqlite3_column_double(row.get(), 2) == (version == 13 ? 0.0 : 0.75));
    CHECK(sqlite3_column_int(row.get(), 3) == 10);
    CHECK(sqlite3_column_int(row.get(), 4) == 11);
    CHECK(std::string(reinterpret_cast<const char*>(sqlite3_column_text(row.get(), 5))) == "ast");
    CHECK(std::string(reinterpret_cast<const char*>(sqlite3_column_text(row.get(), 6))) == "static");
    CHECK(sqlite3_column_int(row.get(), 7) == 1);
    CHECK(sqlite3_column_type(row.get(), 8) == SQLITE_NULL);
    CHECK(sqlite3_column_type(row.get(), 9) == SQLITE_NULL);
    row.reset();

    REQUIRE(sqlite3_prepare_v2(conn.raw(),
        "SELECT rowid FROM nodes_fts WHERE nodes_fts MATCH 'caller' LIMIT 1",
        -1, &raw, nullptr) == SQLITE_OK);
    row.reset(raw);
    REQUIRE(sqlite3_step(row.get()) == SQLITE_ROW);
    CHECK(sqlite3_column_int(row.get(), 0) == 10);
    row.reset();
    REQUIRE(schema::ensure_schema(conn) == 0);
}

TEST_CASE("Unsupported newer schema is rejected without modifying tables",
          "[unit][schema][migration]") {
    Connection conn(":memory:");
    REQUIRE(schema::ensure_schema(conn) == 0);
    conn.exec("DROP INDEX idx_nodes_rank");
    conn.exec("ALTER TABLE nodes DROP COLUMN rank");
    schema::set_kv(conn, "schema_version", std::to_string(CURRENT_SCHEMA_VERSION + 1));
    REQUIRE(schema::ensure_schema(conn) == 3);
    CHECK(schema::get_schema_version(conn) == CURRENT_SCHEMA_VERSION + 1);
    CHECK_FALSE(schema::table_has_column(conn, "nodes", "rank"));
}

TEST_CASE("Schema migration preserves missing ownership and interrupted index state",
          "[unit][schema][migration][ownership]") {
    Connection conn(":memory:");
    REQUIRE(schema::ensure_schema(conn) == 0);
    schema::delete_kv(conn, "repo_root");
    schema::delete_kv(conn, "last_index_time");
    schema::set_kv(conn, "index_state", "indexing");
    remove_provenance_columns(conn);
    schema::set_kv(conn, "schema_version", "14");

    REQUIRE(schema::ensure_schema(conn) == 0);
    CHECK(schema::get_schema_version(conn) == CURRENT_SCHEMA_VERSION);
    CHECK(schema::get_kv(conn, "repo_root", "").empty());
    CHECK(schema::get_kv(conn, "last_index_time", "").empty());
    CHECK(schema::get_kv(conn, "index_state", "") == "indexing");
}
