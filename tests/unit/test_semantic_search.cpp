#include <catch2/catch_test_macros.hpp>

#include "db/connection.h"
#include "db/queries.h"
#include "db/schema.h"
#include "mcp/tools.h"
#include "semantic/token_vectors.h"
#include "util/json.h"

#include <filesystem>

namespace fs = std::filesystem;
using namespace codetopo;

namespace {

fs::path repo_root_from_test_file() {
    return fs::path(__FILE__).parent_path().parent_path().parent_path();
}

int64_t insert_file(Connection& conn, const char* path) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(conn.raw(),
        "INSERT INTO files(path, language, size_bytes, mtime_ns, content_hash, parse_status) "
        "VALUES(?, 'cpp', 100, 1, 'hash', 'ok')",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return sqlite3_last_insert_rowid(conn.raw());
}

int64_t insert_symbol(Connection& conn, int64_t file_id, const char* kind,
                      const char* name, const char* qualname, int start_line) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(conn.raw(),
        "INSERT INTO nodes(node_type, file_id, kind, name, qualname, start_line, end_line, is_definition, stable_key) "
        "VALUES('symbol', ?, ?, ?, ?, ?, ?, 1, ?)",
        -1, &stmt, nullptr);
    std::string stable_key = std::string(qualname) + "::" + kind + "::" + name;
    sqlite3_bind_int64(stmt, 1, file_id);
    sqlite3_bind_text(stmt, 2, kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, qualname, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, start_line);
    sqlite3_bind_int(stmt, 6, start_line + 2);
    sqlite3_bind_text(stmt, 7, stable_key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return sqlite3_last_insert_rowid(conn.raw());
}

void insert_embedding(Connection& conn, int64_t node_id, const std::vector<int8_t>& embedding) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(conn.raw(),
        "INSERT INTO node_vectors(node_id, embedding) VALUES(?, ?)",
        -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, node_id);
    sqlite3_bind_blob(stmt, 2, embedding.data(), static_cast<int>(embedding.size()), SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

} // namespace

TEST_CASE("symbol_search_semantic returns closest semantic symbol", "[unit][semantic]") {
    const fs::path repo_root = repo_root_from_test_file();
    TokenVectorTable tvt;
    REQUIRE(tvt.load((repo_root / "tools" / "token_vectors_128d.bin").string(),
                     (repo_root / "tools" / "token_list.txt").string()));

    fs::path fixture_dir = repo_root / "build" / "semantic_search_test";
    fs::remove_all(fixture_dir);
    fs::create_directories(fixture_dir);
    fs::path db_path = fixture_dir / "semantic.sqlite";

    {
        Connection conn(db_path);
        REQUIRE(schema::ensure_schema(conn) == 0);

        const int64_t parser_file = insert_file(conn, "src/http/parser.cpp");
        const int64_t client_file = insert_file(conn, "src/http/client.cpp");

        const int64_t parse_node = insert_symbol(conn, parser_file, "function",
                                                 "ParseHttpResponse", "ParseHttpResponse", 42);
        const int64_t send_node = insert_symbol(conn, client_file, "method",
                                                "SendAsync", "HttpClient::SendAsync", 15);
        const int64_t client_node = insert_symbol(conn, client_file, "class",
                                                  "HttpClient", "HttpClient", 1);

        auto parse_emb = tvt.embed("ParseHttpResponse");
        auto send_emb = tvt.embed("SendAsync");
        auto client_emb = tvt.embed("HttpClient");
        REQUIRE(!parse_emb.empty());
        REQUIRE(!send_emb.empty());
        REQUIRE(!client_emb.empty());

        insert_embedding(conn, parse_node, parse_emb);
        insert_embedding(conn, send_node, send_emb);
        insert_embedding(conn, client_node, client_emb);
    }

    {
        Connection conn(db_path, true);
        QueryCache cache(conn);
        auto params = json_parse(R"({"query":"parse http response","limit":3,"min_similarity":0.1})");
        auto result = tools::symbol_search_semantic(params.root(), conn,
                                                    cache, repo_root.string());
        auto doc = json_parse(result);
        REQUIRE(doc);
        auto* root = doc.root();
        REQUIRE(json_get_bool(root, "semantic_ready", false));
        REQUIRE(std::string(json_get_str(root, "query")) == "parse http response");
        auto* results = yyjson_obj_get(root, "results");
        REQUIRE(results != nullptr);
        REQUIRE(yyjson_arr_size(results) >= 1);
        auto* first = yyjson_arr_get_first(results);
        REQUIRE(std::string(json_get_str(first, "name")) == "ParseHttpResponse");
        REQUIRE(json_get_double(first, "similarity", 0.0) > 0.3);
    }

    fs::remove_all(fixture_dir);
}
