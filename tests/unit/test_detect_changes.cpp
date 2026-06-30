// Unit tests for detect_changes MCP blast-radius analysis.

#include <catch2/catch_test_macros.hpp>
#include "db/connection.h"
#include "db/schema.h"
#include "db/queries.h"
#include "mcp/tools.h"
#include "util/json.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace codetopo;

namespace {

void cleanup(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
}

struct DetectChangesDb {
    fs::path root;
    fs::path db_path;
    int64_t foo = 0;
    int64_t bar = 0;
    int64_t baz = 0;
    int64_t low = 0;

    ~DetectChangesDb() { cleanup(root); }
};

static void step_done(Connection& conn, sqlite3_stmt* stmt) {
    int rc = sqlite3_step(stmt);
    INFO(sqlite3_errmsg(conn.raw()));
    REQUIRE(rc == SQLITE_DONE);
    sqlite3_finalize(stmt);
}

static int64_t insert_file(Connection& conn, const char* path) {
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(conn.raw(),
        "INSERT INTO files(path, language, size_bytes, mtime_ns, content_hash, parse_status) "
        "VALUES(?, 'cpp', 100, 1000000, ?, 'ok')",
        -1, &stmt, nullptr) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
    std::string hash = std::string("hash:") + path;
    sqlite3_bind_text(stmt, 2, hash.c_str(), -1, SQLITE_TRANSIENT);
    step_done(conn, stmt);
    return sqlite3_last_insert_rowid(conn.raw());
}

static int64_t insert_symbol(Connection& conn, int64_t file_id, const char* kind,
                             const char* name, const char* qualname,
                             int start_line, int end_line) {
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(conn.raw(),
        "INSERT INTO nodes(node_type, file_id, kind, name, qualname, start_line, end_line, stable_key) "
        "VALUES('symbol', ?, ?, ?, ?, ?, ?, ?)",
        -1, &stmt, nullptr) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, file_id);
    sqlite3_bind_text(stmt, 2, kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, qualname, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, start_line);
    sqlite3_bind_int(stmt, 6, end_line);
    std::string stable_key = std::string(qualname) + "::" + kind + "::" + name;
    sqlite3_bind_text(stmt, 7, stable_key.c_str(), -1, SQLITE_TRANSIENT);
    step_done(conn, stmt);
    return sqlite3_last_insert_rowid(conn.raw());
}

static void insert_edge(Connection& conn, int64_t src, int64_t dst,
                        const char* kind, double confidence) {
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(conn.raw(),
        "INSERT INTO edges(src_id, dst_id, kind, confidence) VALUES(?, ?, ?, ?)",
        -1, &stmt, nullptr) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, src);
    sqlite3_bind_int64(stmt, 2, dst);
    sqlite3_bind_text(stmt, 3, kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 4, confidence);
    step_done(conn, stmt);
}

static std::string quote_path(const fs::path& path) {
    return "\"" + path.string() + "\"";
}

static int run_cmd(const std::string& cmd) {
    INFO(cmd);
    return std::system(cmd.c_str());
}

static DetectChangesDb make_detect_changes_db() {
    DetectChangesDb db;
    db.root = fs::path("build") / "test_detect_changes";
    cleanup(db.root);
    fs::create_directories(db.root / "src");

    {
        std::ofstream(db.root / "src" / "a.cpp") << "int foo() { return 1; }\n";
        std::ofstream(db.root / "src" / "b.cpp") << "int bar() { return foo(); }\n";
        std::ofstream(db.root / "src" / "c.cpp") << "int baz() { return bar(); }\n";
        std::ofstream(db.root / "src" / "d.cpp") << "int low() { return foo(); }\n";
    }

    REQUIRE(run_cmd("git -C " + quote_path(db.root) + " init >/dev/null 2>/dev/null") == 0);
    REQUIRE(run_cmd("git -C " + quote_path(db.root) + " config user.email tester@example.com") == 0);
    REQUIRE(run_cmd("git -C " + quote_path(db.root) + " config user.name Tester") == 0);
    REQUIRE(run_cmd("git -C " + quote_path(db.root) + " add src && git -C " + quote_path(db.root) +
                    " commit -m base >/dev/null 2>/dev/null") == 0);

    {
        std::ofstream(db.root / "src" / "a.cpp") << "int foo() { return 2; }\n";
    }
    REQUIRE(run_cmd("git -C " + quote_path(db.root) + " add src/a.cpp && git -C " + quote_path(db.root) +
                    " commit -m change-foo >/dev/null 2>/dev/null") == 0);

    db.db_path = db.root / "index.sqlite";
    Connection conn(db.db_path);
    schema::ensure_schema(conn);

    int64_t a_file = insert_file(conn, "src/a.cpp");
    int64_t b_file = insert_file(conn, "src/b.cpp");
    int64_t c_file = insert_file(conn, "src/c.cpp");
    int64_t d_file = insert_file(conn, "src/d.cpp");

    db.foo = insert_symbol(conn, a_file, "function", "foo", "foo", 1, 1);
    db.bar = insert_symbol(conn, b_file, "function", "bar", "bar", 1, 1);
    db.baz = insert_symbol(conn, c_file, "function", "baz", "baz", 1, 1);
    db.low = insert_symbol(conn, d_file, "function", "low", "low", 1, 1);

    insert_edge(conn, db.bar, db.foo, "calls", 0.9);
    insert_edge(conn, db.baz, db.bar, "calls", 0.8);
    insert_edge(conn, db.low, db.foo, "calls", 0.3);

    schema::set_kv(conn, "repo_root", db.root.string());
    schema::set_kv(conn, "last_index_time", "2026-06-29T20:49:15-04:00");
    conn.wal_checkpoint();
    return db;
}

static std::string invoke_detect_changes(const DetectChangesDb& db, const std::string& params) {
    Connection conn(db.db_path, true);
    QueryCache cache(conn);
    auto params_doc = json_parse(params);
    REQUIRE(params_doc);
    return tools::detect_changes(params_doc.root(), conn, cache, db.root.string());
}

static yyjson_val* require_array_field(yyjson_val* obj, const char* key) {
    auto* arr = yyjson_obj_get(obj, key);
    REQUIRE(arr != nullptr);
    REQUIRE(yyjson_is_arr(arr));
    return arr;
}

static yyjson_val* find_by_name(yyjson_val* arr, const std::string& name) {
    yyjson_arr_iter iter;
    yyjson_arr_iter_init(arr, &iter);
    while (auto* item = yyjson_arr_iter_next(&iter)) {
        const char* value = json_get_str(item, "name");
        if (value && name == value) return item;
    }
    return nullptr;
}

} // namespace

TEST_CASE("detect_changes returns changed and impacted symbols from git diff", "[unit][mcp]") {
    auto db = make_detect_changes_db();
    std::string params =
        "{\"repo_root\":\"" + db.root.string() + "\","
        "\"since\":\"HEAD~1\","
        "\"depth\":2,"
        "\"min_confidence\":0.5}";

    auto result = invoke_detect_changes(db, params);
    auto doc = json_parse(result);
    REQUIRE(doc);

    auto* root = doc.root();
    REQUIRE(std::string(json_get_str(root, "since")) == "HEAD~1");

    auto* changed_files = require_array_field(root, "changed_files");
    REQUIRE(yyjson_arr_size(changed_files) == 1);
    REQUIRE(std::string(yyjson_get_str(yyjson_arr_get(changed_files, 0))) == "src/a.cpp");

    auto* changed_symbols = require_array_field(root, "changed_symbols");
    REQUIRE(yyjson_arr_size(changed_symbols) == 1);
    REQUIRE(find_by_name(changed_symbols, "foo") != nullptr);

    auto* impacted = require_array_field(root, "impacted_symbols");
    REQUIRE(yyjson_arr_size(impacted) == 2);
    auto* bar = find_by_name(impacted, "bar");
    auto* baz = find_by_name(impacted, "baz");
    REQUIRE(bar != nullptr);
    REQUIRE(baz != nullptr);
    REQUIRE(json_get_int(bar, "distance") == 1);
    REQUIRE(json_get_int(baz, "distance") == 2);
    REQUIRE(find_by_name(impacted, "low") == nullptr);

    auto* summary = yyjson_obj_get(root, "summary");
    REQUIRE(summary != nullptr);
    REQUIRE(json_get_int(summary, "changed_files") == 1);
    REQUIRE(json_get_int(summary, "changed_symbols") == 1);
    REQUIRE(json_get_int(summary, "impacted_symbols") == 2);
    REQUIRE(json_get_int(summary, "max_depth_reached") == 2);
}

TEST_CASE("detect_changes honors file_pattern filters", "[unit][mcp]") {
    auto db = make_detect_changes_db();
    std::string params =
        "{\"repo_root\":\"" + db.root.string() + "\","
        "\"since\":\"HEAD~1\","
        "\"file_pattern\":\"src/**/*.go\"}";

    auto result = invoke_detect_changes(db, params);
    auto doc = json_parse(result);
    REQUIRE(doc);

    auto* root = doc.root();
    REQUIRE(yyjson_arr_size(require_array_field(root, "changed_files")) == 0);
    REQUIRE(yyjson_arr_size(require_array_field(root, "changed_symbols")) == 0);
    REQUIRE(yyjson_arr_size(require_array_field(root, "impacted_symbols")) == 0);
}
