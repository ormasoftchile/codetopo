#include <catch2/catch_test_macros.hpp>
#include "db/connection.h"
#include "db/schema.h"
#include "db/queries.h"
#include "mcp/tools.h"
#include "util/json.h"
#include <filesystem>

namespace fs = std::filesystem;
using namespace codetopo;

namespace {

struct ArchitectureIds {
    int64_t ianimal = -1;
    int64_t bark = -1;
    int64_t fetch = -1;
    int64_t meow = -1;
    int64_t main_fn = -1;
    int64_t helper = -1;
    int64_t log = -1;
};

static void step_and_finalize(sqlite3_stmt* stmt) {
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

static int64_t insert_file(Connection& conn, const char* path) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(conn.raw(),
        "INSERT INTO files(path, language, size_bytes, mtime_ns, content_hash, parse_status) "
        "VALUES(?, 'cpp', 100, 1000, 'hash', 'ok')",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
    step_and_finalize(stmt);
    return sqlite3_last_insert_rowid(conn.raw());
}

static int64_t insert_symbol(Connection& conn, int64_t file_id, const char* kind,
                             const char* name, const char* qualname) {
    sqlite3_stmt* stmt = nullptr;
    std::string stable_key = std::string(qualname) + "::" + kind + "::" + name;
    sqlite3_prepare_v2(conn.raw(),
        "INSERT INTO nodes(node_type, file_id, kind, name, qualname, start_line, end_line, stable_key) "
        "VALUES('symbol', ?, ?, ?, ?, 1, 5, ?)",
        -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, file_id);
    sqlite3_bind_text(stmt, 2, kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, qualname, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, stable_key.c_str(), -1, SQLITE_TRANSIENT);
    step_and_finalize(stmt);
    return sqlite3_last_insert_rowid(conn.raw());
}

static void insert_edge(Connection& conn, int64_t src_id, int64_t dst_id,
                        const char* kind, double confidence = 1.0) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(conn.raw(),
        "INSERT INTO edges(src_id, dst_id, kind, confidence) VALUES(?, ?, ?, ?)",
        -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, src_id);
    sqlite3_bind_int64(stmt, 2, dst_id);
    sqlite3_bind_text(stmt, 3, kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 4, confidence);
    step_and_finalize(stmt);
}

static std::pair<fs::path, ArchitectureIds> create_architecture_db() {
    auto tmp = fs::temp_directory_path() / "codetopo_architecture_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "app");
    fs::create_directories(tmp / "domain");
    fs::create_directories(tmp / "utils");
    fs::create_directories(tmp / "shared");

    auto db_path = tmp / "architecture.sqlite";
    Connection conn(db_path);
    schema::ensure_schema(conn);

    int64_t app_main = insert_file(conn, "app/main.cpp");
    int64_t domain_animal = insert_file(conn, "domain/animal.h");
    int64_t domain_dog = insert_file(conn, "domain/dog.cpp");
    int64_t domain_cat = insert_file(conn, "domain/cat.cpp");
    int64_t utils_helper = insert_file(conn, "utils/helper.cpp");
    int64_t shared_log = insert_file(conn, "shared/log.cpp");

    ArchitectureIds ids;
    ids.ianimal = insert_symbol(conn, domain_animal, "interface", "IAnimal", "IAnimal");
    insert_symbol(conn, domain_dog, "class", "Dog", "Dog");
    insert_symbol(conn, domain_cat, "class", "Cat", "Cat");
    ids.bark = insert_symbol(conn, domain_dog, "function", "bark", "Dog::bark");
    ids.fetch = insert_symbol(conn, domain_dog, "function", "fetch", "Dog::fetch");
    ids.meow = insert_symbol(conn, domain_cat, "function", "meow", "Cat::meow");
    ids.main_fn = insert_symbol(conn, app_main, "function", "main", "main");
    ids.helper = insert_symbol(conn, utils_helper, "function", "helper", "helper");
    ids.log = insert_symbol(conn, shared_log, "function", "log_message", "log_message");

    insert_edge(conn, ids.main_fn, ids.bark, "calls", 0.9);
    insert_edge(conn, ids.main_fn, ids.meow, "calls", 0.9);
    insert_edge(conn, ids.main_fn, ids.helper, "calls", 0.8);
    insert_edge(conn, ids.main_fn, ids.log, "calls", 0.8);
    insert_edge(conn, ids.bark, ids.fetch, "calls", 0.9);
    insert_edge(conn, ids.bark, ids.log, "calls", 0.8);
    insert_edge(conn, ids.meow, ids.log, "calls", 0.8);
    insert_edge(conn, ids.fetch, ids.log, "calls", 0.8);

    schema::set_kv(conn, "schema_version", "1");
    schema::set_kv(conn, "indexer_version", "1.0.0");
    schema::set_kv(conn, "repo_root", tmp.string());
    schema::set_kv(conn, "last_index_time", "2026-06-30T12:00:00Z");
    conn.wal_checkpoint();

    return {db_path, ids};
}

static yyjson_val* find_named_item(yyjson_val* arr, const char* key, const std::string& value) {
    yyjson_val* item = nullptr;
    size_t idx, max;
    yyjson_arr_foreach(arr, idx, max, item) {
        auto* field = json_get_str(item, key);
        if (field && value == field) return item;
    }
    return nullptr;
}

} // namespace

TEST_CASE("get_architecture returns clusters hotspots and boundaries", "[unit]") {
    auto [db_path, ids] = create_architecture_db();
    {
        Connection conn(db_path, true);
        QueryCache cache(conn);
        auto repo_root = schema::get_kv(const_cast<Connection&>(conn), "repo_root");

        auto result = tools::get_architecture(nullptr, const_cast<Connection&>(conn), cache, repo_root);
        auto doc = json_parse(result);
        REQUIRE(doc);

        auto* summary = yyjson_obj_get(doc.root(), "summary");
        REQUIRE(summary != nullptr);
        REQUIRE(json_get_int(summary, "total_files") == 6);
        REQUIRE(json_get_int(summary, "total_symbols") == 9);
        REQUIRE(json_get_int(summary, "total_edges") == 8);
        REQUIRE(json_get_int(summary, "cluster_count") == 4);

        auto* clusters = yyjson_obj_get(doc.root(), "clusters");
        REQUIRE(clusters != nullptr);
        auto* domain = find_named_item(clusters, "name", "domain");
        REQUIRE(domain != nullptr);
        REQUIRE(json_get_int(domain, "files") == 3);
        REQUIRE(json_get_int(domain, "symbols") == 6);
        REQUIRE(json_get_int(domain, "internal_edges") == 1);
        REQUIRE(json_get_int(domain, "external_edges") == 5);

        auto* hotspots = yyjson_obj_get(doc.root(), "hotspots");
        REQUIRE(hotspots != nullptr);
        auto* log = find_named_item(hotspots, "name", "log_message");
        REQUIRE(log != nullptr);
        REQUIRE(json_get_int(log, "fan_in") == 4);

        auto* boundaries = yyjson_obj_get(doc.root(), "boundaries");
        REQUIRE(boundaries != nullptr);
        auto* main_boundary = find_named_item(boundaries, "file", "app/main.cpp");
        REQUIRE(main_boundary != nullptr);
        REQUIRE(json_get_int(main_boundary, "internal_edges") == 0);
        REQUIRE(json_get_int(main_boundary, "external_edges") == 4);
    }
    fs::remove_all(db_path.parent_path());
}

TEST_CASE("get_architecture supports scope and aspect filtering", "[unit]") {
    auto [db_path, ids] = create_architecture_db();
    {
        Connection conn(db_path, true);
        QueryCache cache(conn);
        auto repo_root = schema::get_kv(const_cast<Connection&>(conn), "repo_root");

        auto params = json_parse(R"({"scope":"domain","aspects":["summary","hotspots"],"limit":5})");
        auto result = tools::get_architecture(params.root(), const_cast<Connection&>(conn), cache, repo_root);
        auto doc = json_parse(result);
        REQUIRE(doc);

        REQUIRE(yyjson_obj_get(doc.root(), "clusters") == nullptr);
        REQUIRE(yyjson_obj_get(doc.root(), "boundaries") == nullptr);

        auto* summary = yyjson_obj_get(doc.root(), "summary");
        REQUIRE(summary != nullptr);
        REQUIRE(json_get_int(summary, "total_files") == 3);
        REQUIRE(json_get_int(summary, "cluster_count") == 1);

        auto* hotspots = yyjson_obj_get(doc.root(), "hotspots");
        REQUIRE(hotspots != nullptr);
        REQUIRE(find_named_item(hotspots, "name", "log_message") == nullptr);
        REQUIRE(find_named_item(hotspots, "name", "bark") != nullptr);
    }
    fs::remove_all(db_path.parent_path());
}
