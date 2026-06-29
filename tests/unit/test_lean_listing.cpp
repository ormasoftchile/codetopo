// Regression tests for lean symbol listing output and min-span filtering.

#include <catch2/catch_test_macros.hpp>
#include "db/connection.h"
#include "db/schema.h"
#include "db/queries.h"
#include "mcp/tools.h"
#include "util/json.h"
#include <filesystem>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace codetopo;

namespace {

void cleanup(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
}

struct TestDb {
    fs::path root;
    fs::path db_path;

    TestDb() = default;
    TestDb(const TestDb&) = delete;
    TestDb& operator=(const TestDb&) = delete;
    TestDb(TestDb&& other) noexcept
        : root(std::move(other.root)), db_path(std::move(other.db_path)) {
        other.root.clear();
    }
    TestDb& operator=(TestDb&& other) noexcept {
        if (this != &other) {
            cleanup(root);
            root = std::move(other.root);
            db_path = std::move(other.db_path);
            other.root.clear();
        }
        return *this;
    }
    ~TestDb() { cleanup(root); }
};

static TestDb make_lean_db(const std::string& name, int function_count = 0) {
    TestDb db;
    db.root = fs::path("build") / "test_lean_listing" / name;
    cleanup(db.root);
    fs::create_directories(db.root / "src");
    db.db_path = db.root / "codetopo.sqlite";

    Connection conn(db.db_path);
    schema::ensure_schema(conn);
    conn.exec("INSERT INTO files(path, language, size_bytes, mtime_ns, content_hash, parse_status) "
              "VALUES('src/types.ts', 'typescript', 1000, 1000000, 'types-hash', 'ok')");
    int64_t file_id = sqlite3_last_insert_rowid(conn.raw());

    auto insert_symbol = [&](const std::string& kind, const std::string& name,
                             int start_line, int end_line, const char* visibility) {
        sqlite3_stmt* stmt = nullptr;
        REQUIRE(sqlite3_prepare_v2(conn.raw(),
            "INSERT INTO nodes(node_type, file_id, kind, name, qualname, start_line, end_line, "
            "visibility, stable_key) VALUES('symbol', ?, ?, ?, ?, ?, ?, ?, ?)",
            -1, &stmt, nullptr) == SQLITE_OK);
        sqlite3_bind_int64(stmt, 1, file_id);
        sqlite3_bind_text(stmt, 2, kind.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 5, start_line);
        sqlite3_bind_int(stmt, 6, end_line);
        if (visibility) {
            sqlite3_bind_text(stmt, 7, visibility, -1, SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_null(stmt, 7);
        }
        std::string stable_key = "src/types.ts::" + kind + "::" + name;
        sqlite3_bind_text(stmt, 8, stable_key.c_str(), -1, SQLITE_TRANSIENT);
        REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
    };

    insert_symbol("type_alias", "ID", 1, 1, "public");
    insert_symbol("type_alias", "Props", 2, 2, "public");
    insert_symbol("type_alias", "LocalPrivate", 3, 3, "private");
    insert_symbol("type_alias", "LongOptions", 10, 15, "public");

    for (int i = 0; i < function_count; ++i) {
        std::ostringstream name_stream;
        name_stream << "fn" << i;
        insert_symbol("function", name_stream.str(), 100 + i, 104 + i, "public");
    }

    schema::set_kv(conn, "schema_version", "1");
    schema::set_kv(conn, "indexer_version", "test");
    schema::set_kv(conn, "repo_root", db.root.string());
    schema::set_kv(conn, "last_index_time", "2026-06-28T22:39:24-04:00");
    return db;
}

static std::string call_symbol_list(const TestDb& db, const std::string& params) {
    Connection conn(db.db_path, true);
    QueryCache cache(conn);
    auto params_doc = json_parse(params);
    REQUIRE(params_doc);
    return tools::symbol_list(params_doc.root(), conn, cache, db.root.string());
}

static std::string call_symbols_in_path(const TestDb& db, const std::string& params) {
    Connection conn(db.db_path, true);
    QueryCache cache(conn);
    auto params_doc = json_parse(params);
    REQUIRE(params_doc);
    return tools::symbols_in_path(params_doc.root(), conn, cache, db.root.string());
}

static yyjson_val* require_results(yyjson_val* root) {
    auto* results = yyjson_obj_get(root, "results");
    REQUIRE(results != nullptr);
    REQUIRE(yyjson_is_arr(results));
    return results;
}

static yyjson_val* require_first_result(yyjson_val* root) {
    auto* results = require_results(root);
    REQUIRE(yyjson_arr_size(results) >= 1);
    return yyjson_arr_get(results, 0);
}

static bool has_field(yyjson_val* obj, const char* key) {
    return yyjson_obj_get(obj, key) != nullptr;
}

static bool results_have_name(yyjson_val* results, const std::string& target) {
    yyjson_val* item = nullptr;
    yyjson_arr_iter iter;
    yyjson_arr_iter_init(results, &iter);
    while ((item = yyjson_arr_iter_next(&iter))) {
        const char* name = json_get_str(item, "name");
        if (name && target == name) return true;
    }
    return false;
}

} // namespace

TEST_CASE("symbol_list keeps short exported TS types visible under min_span",
          "[unit][lean-listing]") {
    auto db = make_lean_db("symbol-list-hidden");

    auto result = call_symbol_list(db,
        R"({"kind":"type_alias","min_span_lines":3,"limit":20})");
    auto doc = json_parse(result);
    REQUIRE(doc);
    auto* root = doc.root();

    // TODO(lean-listing): keep public/API-like TS type aliases visible even
    // when min_span_lines would otherwise hide one-line declarations.
    CHECK(has_field(root, "total_candidates"));
    CHECK(has_field(root, "filtered_hidden"));
    CHECK(has_field(root, "hidden_public_count"));
    CHECK(json_get_int(root, "total_candidates", -1) == 4);
    CHECK(json_get_int(root, "filtered_hidden", -1) == 1);
    CHECK(json_get_int(root, "hidden_public_count", -1) == 0);

    auto* results = require_results(root);
    REQUIRE(yyjson_arr_size(results) == 3);
    CHECK(results_have_name(results, "ID"));
    CHECK(results_have_name(results, "Props"));
    CHECK(results_have_name(results, "LongOptions"));
    CHECK_FALSE(results_have_name(results, "LocalPrivate"));
}

TEST_CASE("symbols_in_path keeps short exported TS types visible under min_span",
          "[unit][lean-listing]") {
    auto db = make_lean_db("symbols-in-path-hidden");

    auto result = call_symbols_in_path(db,
        R"({"path":".","kind":["type_alias"],"min_span_lines":3,"limit":20})");
    auto doc = json_parse(result);
    REQUIRE(doc);
    auto* root = doc.root();

    // TODO(lean-listing): symbols_in_path must match symbol_list behavior for
    // one-line exported TypeScript types under min_span_lines.
    CHECK(has_field(root, "total_candidates"));
    CHECK(has_field(root, "filtered_hidden"));
    CHECK(has_field(root, "hidden_public_count"));
    CHECK(json_get_int(root, "total_candidates", -1) == 4);
    CHECK(json_get_int(root, "filtered_hidden", -1) == 1);
    CHECK(json_get_int(root, "hidden_public_count", -1) == 0);

    auto* results = require_results(root);
    REQUIRE(yyjson_arr_size(results) == 3);
    CHECK(results_have_name(results, "ID"));
    CHECK(results_have_name(results, "Props"));
    CHECK(results_have_name(results, "LongOptions"));
    CHECK_FALSE(results_have_name(results, "LocalPrivate"));
}

TEST_CASE("symbol_list reports candidate counters when kind filters hide public API",
          "[unit][lean-listing]") {
    auto db = make_lean_db("hidden-public-counters");

    auto result = call_symbol_list(db, R"({"kind":"function","limit":20})");
    auto doc = json_parse(result);
    REQUIRE(doc);
    auto* root = doc.root();

    CHECK(has_field(root, "total_candidates"));
    CHECK(has_field(root, "filtered_hidden"));
    CHECK(has_field(root, "hidden_public_count"));
    CHECK(json_get_int(root, "total_candidates", -1) == 4);
    CHECK(json_get_int(root, "filtered_hidden", -1) == 4);
    CHECK(json_get_int(root, "hidden_public_count", -1) == 3);
    CHECK(yyjson_arr_size(require_results(root)) == 0);
}

TEST_CASE("lean symbol_list omits handles by default and include_handles opts in",
          "[unit][lean-listing]") {
    auto db = make_lean_db("include-handles");

    auto lean_result = call_symbol_list(db,
        R"({"kind":"type_alias","name_glob":"ID","limit":1})");
    auto lean_doc = json_parse(lean_result);
    REQUIRE(lean_doc);
    auto* lean_item = require_first_result(lean_doc.root());
    CHECK_FALSE(has_field(lean_item, "node_id"));
    CHECK_FALSE(has_field(lean_item, "span"));

    auto handles_result = call_symbol_list(db,
        R"({"kind":"type_alias","name_glob":"ID","include_handles":true,"limit":1})");
    auto handles_doc = json_parse(handles_result);
    REQUIRE(handles_doc);
    auto* handles_item = require_first_result(handles_doc.root());
    CHECK(has_field(handles_item, "node_id"));
    CHECK(has_field(handles_item, "span"));

    auto path_lean_result = call_symbols_in_path(db,
        R"({"path":".","kind":["type_alias"],"limit":1})");
    auto path_lean_doc = json_parse(path_lean_result);
    REQUIRE(path_lean_doc);
    auto* path_lean_item = require_first_result(path_lean_doc.root());
    CHECK_FALSE(has_field(path_lean_item, "node_id"));
    CHECK_FALSE(has_field(path_lean_item, "span"));

    auto path_handles_result = call_symbols_in_path(db,
        R"({"path":".","kind":["type_alias"],"include_handles":true,"limit":1})");
    auto path_handles_doc = json_parse(path_handles_result);
    REQUIRE(path_handles_doc);
    auto* path_handles_item = require_first_result(path_handles_doc.root());
    CHECK(has_field(path_handles_item, "node_id"));
    CHECK(has_field(path_handles_item, "span"));
}

TEST_CASE("flat lean symbol_list stays below overflow threshold",
          "[unit][lean-listing]") {
    auto db = make_lean_db("flat-small", 2000);

    auto result = call_symbol_list(db, R"({"kind":"function","limit":2000,"max_bytes":8000})");
    INFO("flat symbol_list response bytes: " << result.size());
    CHECK(result.size() < 9 * 1024);

    auto doc = json_parse(result);
    REQUIRE(doc);
    auto* root = doc.root();
    CHECK_FALSE(has_field(root, "_truncated"));
    CHECK_FALSE(has_field(root, "overflow"));
    CHECK_FALSE(has_field(root, "spill"));
    CHECK(json_get_int(root, "total", -1) >= 500);
    CHECK(json_get_int(root, "max_bytes", -1) == 8000);

    auto* results = require_results(root);
    CHECK(yyjson_arr_size(results) > 0);
    CHECK(yyjson_arr_size(results) < static_cast<size_t>(json_get_int(root, "total", 0)));
    CHECK(json_get_bool(root, "has_more", false));
    CHECK(json_get_bool(root, "budget_exceeded", false));
}
