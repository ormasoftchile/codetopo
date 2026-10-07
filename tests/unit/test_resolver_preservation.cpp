#include <catch2/catch_test_macros.hpp>
#include "db/connection.h"
#include "db/schema.h"
#include "index/persister.h"
#include "index/change_detector.h"
#include "index/pagerank.h"
#include <set>
#include <string>

using namespace codetopo;

namespace {

int64_t scalar(Connection& conn, const std::string& sql) {
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(conn.raw(), sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    auto value = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return value;
}

int capture_queries(unsigned, void* context, void* statement, void*) {
    const char* sql = sqlite3_sql(static_cast<sqlite3_stmt*>(statement));
    if (sql) {
        std::string query(sql);
        if (query.starts_with("SELECT ") || query.starts_with("WITH ") ||
            query.starts_with("DELETE FROM edges") ||
            query.starts_with("UPDATE nodes SET rank") ||
            query.find("INSERT OR IGNORE INTO temp.__ct_pagerank_scope") != std::string::npos) {
            static_cast<std::set<std::string>*>(context)->insert(query);
        }
    }
    return 0;
}

void assert_scoped_plans(Connection& conn, const std::set<std::string>& queries) {
    REQUIRE_FALSE(queries.empty());
    for (const auto& query : queries) {
        CAPTURE(query);
        sqlite3_stmt* stmt = nullptr;
        auto explain = "EXPLAIN QUERY PLAN " + query;
        REQUIRE(sqlite3_prepare_v2(conn.raw(), explain.c_str(), -1, &stmt, nullptr) == SQLITE_OK);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            auto text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            REQUIRE(text != nullptr);
            std::string plan(text);
            CAPTURE(plan);
            CHECK(plan.find("SCAN n") == std::string::npos);
            CHECK(plan.find("SCAN e") == std::string::npos);
            CHECK(plan.find("SCAN dst") == std::string::npos);
            CHECK(plan.find("SCAN nodes") == std::string::npos);
            CHECK(plan.find("SCAN edges") == std::string::npos);
        }
        sqlite3_finalize(stmt);
    }
}

} // namespace

TEST_CASE("Full resolver preserves resolved primary workspace and rootless relationships",
          "[unit][ownership][resolve_refs][graph-preservation]") {
    Connection conn(":memory:");
    REQUIRE(schema::ensure_schema(conn) == 0);
    conn.exec("INSERT INTO roots(id,path,added_at) VALUES(1,'C:\\extra',datetime('now'))");
    conn.exec(
        "INSERT INTO files(id,path,language,size_bytes,mtime_ns,content_hash,parse_status,root_id) VALUES"
        "(1,'target.cpp','cpp',10,1,'target','ok',NULL),"
        "(2,'caller.cpp','cpp',10,1,'caller','ok',NULL),"
        "(3,'C:\\extra/extra.cpp','cpp',10,1,'extra','ok',1)");
    conn.exec(
        "INSERT INTO nodes(id,node_type,file_id,kind,name,stable_key) VALUES"
        "(1,'file',NULL,'file','target.cpp','target.cpp::file'),"
        "(2,'file',NULL,'file','caller.cpp','caller.cpp::file'),"
        "(3,'file',NULL,'file','extra.cpp','1:extra.cpp::file'),"
        "(10,'symbol',1,'function','helper','target.cpp::function::helper'),"
        "(11,'symbol',1,'class','Base','target.cpp::class::Base'),"
        "(20,'symbol',2,'function','caller','caller.cpp::function::caller'),"
        "(30,'symbol',3,'function','extra','1:extra.cpp::function::extra'),"
        "(90,'file',NULL,'file','missing.cpp','missing.cpp::file')");
    conn.exec(
        "INSERT INTO refs(id,file_id,kind,name,containing_node_id,resolved_node_id) VALUES"
        "(100,2,'call','helper',20,NULL),"
        "(101,2,'call','helper',20,10),"
        "(102,2,'include','target.cpp',NULL,1),"
        "(103,2,'inherit','Base',20,11),"
        "(104,3,'call','helper',30,NULL)");
    conn.exec(
        "INSERT INTO edges(id,src_id,dst_id,kind,confidence,evidence,source,observed_count) VALUES"
        "(200,20,10,'calls',0.91,'name-match','runtime',42),"
        "(201,2,1,'includes',0.7,'name-match','static',1),"
        "(202,3,30,'calls',0.75,'name-match','static',1),"
        "(203,90,30,'references',0.7,'name-match','static',1)");

    std::set<std::string> queries;
    sqlite3_trace_v2(conn.raw(), SQLITE_TRACE_STMT, capture_queries, &queries);
    Persister persister(conn);
    auto first = persister.resolve_references();
    sqlite3_trace_v2(conn.raw(), 0, nullptr, nullptr);
    CHECK(first.first == 1);
    CHECK(first.second == 1);
    CHECK(scalar(conn, "SELECT COUNT(*) FROM edges WHERE id IN (200,201,202,203)") == 4);
    CHECK(scalar(conn,
        "SELECT COUNT(*) FROM edges WHERE id=200 AND confidence=0.91 "
        "AND source='runtime' AND observed_count=42") == 1);
    CHECK(scalar(conn,
        "SELECT COUNT(*) FROM edges WHERE src_id=20 AND dst_id=11 AND kind='inherits'") == 1);
    CHECK(scalar(conn,
        "SELECT COUNT(*) FROM refs WHERE id=104 AND resolved_node_id IS NULL") == 1);
    CHECK(scalar(conn,
        "SELECT COUNT(*) FROM edges WHERE src_id=20 AND dst_id=10 AND kind='calls'") == 1);
    assert_scoped_plans(conn, queries);

    auto second = persister.resolve_references();
    CHECK(second.first == 0);
    CHECK(second.second == 0);
    CHECK(scalar(conn, "SELECT COUNT(*) FROM edges WHERE src_id=20 AND kind='inherits'") == 1);
    CHECK(conn.foreign_key_check() == 0);
}

TEST_CASE("Full resolver bulk insert deduplicates existing fanout without dropping indexes",
          "[unit][ownership][resolve_refs][graph-preservation]") {
    Connection conn(":memory:");
    REQUIRE(schema::ensure_schema(conn) == 0);
    conn.exec(
        "INSERT INTO files(id,path,language,size_bytes,mtime_ns,content_hash,parse_status) VALUES"
        "(1,'caller.cpp','cpp',10,1,'caller','ok'),"
        "(2,'targets.cpp','cpp',10,1,'targets','ok')");
    conn.exec(
        "INSERT INTO nodes(id,node_type,file_id,kind,name,stable_key) VALUES"
        "(1,'file',NULL,'file','caller.cpp','caller.cpp::file'),"
        "(2,'file',NULL,'file','targets.cpp','targets.cpp::file'),"
        "(10,'symbol',1,'function','caller','caller.cpp::function::caller')");
    for (int i = 0; i < 401; ++i) {
        auto id = std::to_string(1000 + i);
        auto name = "target" + std::to_string(i);
        conn.exec(
            "INSERT INTO nodes(id,node_type,file_id,kind,name,stable_key) VALUES(" +
            id + ",'symbol',2,'function','" + name + "','targets::" + name + "')");
        conn.exec(
            "INSERT INTO refs(file_id,kind,name,containing_node_id) VALUES"
            "(1,'call','" + name + "',10)");
        if (i < 200) {
            conn.exec(
                "INSERT INTO edges(src_id,dst_id,kind,confidence,evidence) VALUES"
                "(10," + id + ",'calls',0.75,'name-match')");
        }
    }
    Persister persister(conn);
    auto first = persister.resolve_references();
    CHECK(first.first == 401);
    CHECK(first.second == 201);
    CHECK(scalar(conn, "SELECT COUNT(*) FROM edges WHERE src_id=10 AND kind='calls'") == 401);
    CHECK(scalar(conn,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name IN "
        "('idx_edges_src','idx_edges_dst','idx_edges_dst_conf','idx_edges_source')") == 4);
    auto second = persister.resolve_references();
    CHECK(second.first == 0);
    CHECK(second.second == 0);
    CHECK(scalar(conn, "SELECT COUNT(*) FROM edges WHERE src_id=10 AND kind='calls'") == 401);
}

TEST_CASE("Recovering a pruned file reuses its surviving file node and incoming workspace links",
          "[unit][ownership][persist_opt][graph-preservation]") {
    Connection conn(":memory:");
    REQUIRE(schema::ensure_schema(conn) == 0);
    conn.exec("INSERT INTO roots(id,path,added_at) VALUES(1,'C:\\extra',datetime('now'))");
    conn.exec(
        "INSERT INTO files(id,path,language,size_bytes,mtime_ns,content_hash,parse_status,root_id) "
        "VALUES(1,'C:\\extra/extra.cpp','cpp',10,1,'extra','ok',1)");
    conn.exec(
        "INSERT INTO nodes(id,node_type,file_id,kind,name,stable_key) VALUES"
        "(10,'symbol',1,'function','extra','1:extra.cpp::function::extra'),"
        "(900,'file',NULL,'file','recovered.cpp','recovered.cpp::file')");
    conn.exec(
        "INSERT INTO edges(id,src_id,dst_id,kind,confidence,evidence) "
        "VALUES(2000,10,900,'includes',0.7,'name-match')");
    conn.exec(
        "INSERT INTO refs(id,file_id,kind,name,containing_node_id,resolved_node_id) "
        "VALUES(100,1,'include','recovered.cpp',10,900)");

    Persister persister(conn);
    ScannedFile file;
    file.relative_path = "recovered.cpp";
    file.language = "cpp";
    file.size_bytes = 10;
    file.mtime_ns = 1;
    ExtractionResult extraction;
    ExtractedSymbol symbol;
    symbol.kind = "function";
    symbol.name = "recovered";
    symbol.stable_key = "recovered.cpp::function::recovered";
    symbol.is_definition = true;
    extraction.symbols.push_back(symbol);
    REQUIRE(persister.persist_file(file, extraction, "recovered-hash", "ok"));
    CHECK(scalar(conn,
        "SELECT COUNT(*) FROM nodes WHERE stable_key='recovered.cpp::file' AND id=900") == 1);
    CHECK(scalar(conn, "SELECT COUNT(*) FROM edges WHERE id=2000 AND dst_id=900") == 1);
    CHECK(scalar(conn, "SELECT resolved_node_id FROM refs WHERE id=100") == 900);
    CHECK(scalar(conn,
        "SELECT COUNT(*) FROM files WHERE path='recovered.cpp' AND root_id IS NULL") == 1);
    CHECK(conn.foreign_key_check() == 0);
}

TEST_CASE("Primary change detection does not classify extra-root files as deleted",
          "[unit][ownership][graph-preservation]") {
    Connection conn(":memory:");
    REQUIRE(schema::ensure_schema(conn) == 0);
    conn.exec("INSERT INTO roots(id,path,added_at) VALUES(1,'C:\\extra',datetime('now'))");
    conn.exec(
        "INSERT INTO files(path,language,size_bytes,mtime_ns,content_hash,parse_status,root_id) VALUES"
        "('primary.cpp','cpp',10,1,'primary','ok',NULL),"
        "('C:\\extra/extra.cpp','cpp',10,1,'extra','ok',1)");
    ChangeDetector detector(conn);
    auto result = detector.detect({});
    REQUIRE(result.deleted_paths.size() == 1);
    CHECK(result.deleted_paths.front() == "primary.cpp");
}

TEST_CASE("Indexer ranking traverses only file-owned graph endpoints through indexes",
          "[unit][ownership][graph-preservation][pagerank]") {
    Connection conn(":memory:");
    REQUIRE(schema::ensure_schema(conn) == 0);
    conn.exec(
        "INSERT INTO files(id,path,language,size_bytes,mtime_ns,content_hash,parse_status) "
        "VALUES(1,'ranked.cpp','cpp',10,1,'ranked','ok')");
    conn.exec(
        "INSERT INTO nodes(id,node_type,file_id,kind,name,stable_key,rank) VALUES"
        "(1,'file',NULL,'file','ranked.cpp','ranked.cpp::file',0),"
        "(10,'symbol',1,'function','ranked','ranked.cpp::function::ranked',0),"
        "(20,'file',NULL,'file','orphan.cpp','orphan.cpp::file',0.25)");
    conn.exec(
        "INSERT INTO edges(src_id,dst_id,kind,confidence) VALUES"
        "(1,10,'references',0.7),(20,10,'references',0.7)");
    std::set<std::string> queries;
    sqlite3_trace_v2(conn.raw(), SQLITE_TRACE_STMT, capture_queries, &queries);
    PageRankOptions options;
    options.scope_to_owned_files = true;
    CHECK(compute_and_persist_pagerank(conn, options) == 2);
    sqlite3_trace_v2(conn.raw(), 0, nullptr, nullptr);
    // The temporary scope is dropped after loading; recreate only its schema for EXPLAIN.
    conn.exec("CREATE TEMP TABLE __ct_pagerank_scope(id INTEGER PRIMARY KEY)");
    conn.exec("CREATE TEMP TABLE temp_pagerank(id INTEGER PRIMARY KEY,rank REAL)");
    assert_scoped_plans(conn, queries);
    CHECK(scalar(conn, "SELECT COUNT(*) FROM nodes WHERE id=20 AND rank=0.25") == 1);
}
