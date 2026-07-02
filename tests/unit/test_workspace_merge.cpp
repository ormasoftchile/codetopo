#include <catch2/catch_test_macros.hpp>

#include "core/config.h"
#include "db/connection.h"
#include "db/fts.h"
#include "db/schema.h"
#include "db/workspace.h"
#include "util/repo.h"

#include <filesystem>
#include <fstream>
#include <sqlite3.h>
#include <string>

namespace fs = std::filesystem;
using namespace codetopo;

static void cleanup_workspace_test_dir(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
}

static int64_t scalar_count(Connection& conn, const std::string& sql) {
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(conn.raw(), sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK);
    int64_t count = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

static void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.good());
    out << content;
}

static void create_empty_index(const fs::path& root) {
    fs::create_directories(root / ".codetopo");
    Connection conn(root / ".codetopo" / "index.sqlite");
    schema::ensure_schema(conn);
    fts::create_sync_triggers(conn);
}

static void create_source_index(const fs::path& root) {
    fs::create_directories(root / ".codetopo");
    write_file(root / "src" / "lib.cpp",
               "int mergedNeedleSymbol() {\n"
               "  int uniqueContentNeedle = 42;\n"
               "  return uniqueContentNeedle;\n"
               "}\n");

    Connection conn(root / ".codetopo" / "index.sqlite");
    schema::ensure_schema(conn);
    conn.exec(
        "INSERT INTO files(id, path, language, size_bytes, mtime_ns, content_hash, parse_status) "
        "VALUES(1, 'src/lib.cpp', 'cpp', 88, 1000, 'hash-lib', 'ok')");
    conn.exec(
        "INSERT INTO nodes(id, node_type, file_id, kind, name, qualname, signature, is_definition, stable_key) "
        "VALUES(1, 'file', NULL, 'file', 'src/lib.cpp', NULL, NULL, 1, 'file:src/lib.cpp')");
    conn.exec(
        "INSERT INTO nodes(id, node_type, file_id, kind, name, qualname, signature, start_line, end_line, is_definition, fingerprint, stable_key) "
        "VALUES(2, 'symbol', 1, 'function', 'mergedNeedleSymbol', 'mergedNeedleSymbol', 'mergedNeedleSymbol()', 1, 3, 1, 'abcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcd', 'sym:mergedNeedleSymbol')");
    conn.exec(
        "INSERT INTO edges(src_id, dst_id, kind, confidence, evidence) "
        "VALUES(2, 2, 'references', 1.0, 'test')");
    conn.exec(
        "INSERT INTO edges(src_id, dst_id, kind, confidence, evidence) "
        "VALUES(1, 2, 'contains', 1.0, 'file-node-src-test')");
    conn.exec(
        "INSERT INTO refs(id, file_id, kind, name, start_line, start_col, end_line, end_col, resolved_node_id, evidence, containing_node_id) "
        "VALUES(1, 1, 'call', 'mergedNeedleSymbol', 1, 1, 1, 20, 2, 'test', 2)");
    conn.exec(
        "INSERT INTO refs(id, file_id, kind, name, start_line, start_col, end_line, end_col, resolved_node_id, evidence, containing_node_id) "
        "VALUES(2, 1, 'http_call', '/api/merged-needle', 2, 1, 2, 24, NULL, 'http_client_call', 2)");
    fts::rebuild(conn);
}

TEST_CASE("Workspace add bulk-merges rows and keeps FTS/remove correct", "[unit][workspace]") {
    auto base = fs::current_path() / ".codetopo-workspace-merge-test";
    cleanup_workspace_test_dir(base);
    fs::create_directories(base);

    auto main_root = fs::canonical(base).string() + "/main";
    auto src_root = fs::canonical(base).string() + "/srcroot";
    fs::create_directories(main_root);
    fs::create_directories(src_root);
    create_empty_index(main_root);
    create_source_index(src_root);

    Config cfg;
    WorkspaceDB ws(default_db(main_root));
    auto result = ws.add_root(src_root, cfg);
    REQUIRE(result.root_id > 0);
    REQUIRE(result.files == 1);
    REQUIRE(result.symbols == 1);
    REQUIRE(result.edges == 2);
    REQUIRE(result.http_call_refs == 1);

    {
        Connection conn(default_db(main_root));
        auto offset = result.root_id * 1000000000LL;
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM files WHERE id = " + std::to_string(offset + 1)) == 1);
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM nodes WHERE id = " + std::to_string(offset + 2)) == 1);
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM edges WHERE src_id = " + std::to_string(offset + 1)) == 1);
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM refs WHERE id = " + std::to_string(offset + 1)) == 1);
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM refs WHERE id = " + std::to_string(offset + 2)) == 1);
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM nodes WHERE id = " + std::to_string(offset + 2) + " AND fingerprint IS NOT NULL") == 1);
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM nodes_fts WHERE nodes_fts MATCH 'mergedNeedleSymbol'") == 1);
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM content_fts WHERE content_fts MATCH 'uniqueContentNeedle'") == 0);
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM kv WHERE key LIKE 'workspace_content_fts_pending:%'") == 0);
        REQUIRE(conn.foreign_key_check() == 0);
    }

    auto second = ws.add_root(src_root, cfg);
    REQUIRE(second.root_id == result.root_id);
    REQUIRE(second.files == 1);
    REQUIRE(second.symbols == 1);
    REQUIRE(second.edges == 2);
    REQUIRE(second.http_call_refs == 1);
    {
        Connection conn(default_db(main_root));
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM nodes_fts WHERE nodes_fts MATCH 'mergedNeedleSymbol'") == 1);
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM content_fts_tracker") == 0);
        REQUIRE(conn.foreign_key_check() == 0);
    }

    cfg.workspace_content_fts = true;
    auto third = ws.add_root(src_root, cfg);
    REQUIRE(third.root_id == result.root_id);
    REQUIRE(third.files == 1);
    REQUIRE(third.symbols == 1);
    REQUIRE(third.edges == 2);
    REQUIRE(third.http_call_refs == 1);
    {
        Connection conn(default_db(main_root));
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM nodes_fts WHERE nodes_fts MATCH 'mergedNeedleSymbol'") == 1);
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM content_fts WHERE content_fts MATCH 'uniqueContentNeedle'") >= 1);
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM content_fts_tracker") == 1);
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM kv WHERE key LIKE 'workspace_content_fts_pending:%'") == 0);
        REQUIRE(conn.foreign_key_check() == 0);
    }

    auto roots = ws.list_roots();
    REQUIRE(roots.size() == 1);
    REQUIRE(roots[0].edges == 2);

    auto removed = ws.remove_root(src_root);
    REQUIRE(removed.files == 1);
    REQUIRE(removed.symbols == 1);
    REQUIRE(removed.edges == 2);
    {
        Connection conn(default_db(main_root));
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM roots") == 0);
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM files") == 0);
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM nodes") == 0);
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM nodes_fts WHERE nodes_fts MATCH 'mergedNeedleSymbol'") == 0);
        REQUIRE(scalar_count(conn, "SELECT COUNT(*) FROM content_fts_tracker") == 0);
        REQUIRE(conn.foreign_key_check() == 0);
    }

    cleanup_workspace_test_dir(base);
}
