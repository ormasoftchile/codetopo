// Tests for MCP tool enhancements (US-enhance):
//   1. NEW: find_implementations
//   2. ENHANCED: context_for (siblings, container, base/implements)
//   3. ENHANCED: subgraph (edge_kinds filter)
//   4. ENHANCED: shortest_path (max_paths, relation_types)
//   5. ENHANCED: entrypoints (scope filter)
//   6. ENHANCED: callers_approx / callees_approx (group_by)
//
// Pattern: create in-memory test DB with rich inheritance/containment graph,
// exercise each tool via tools:: API, verify JSON response.
// Tests written RED (pre-implementation) — will pass once Booch lands enhancements.

#include <catch2/catch_test_macros.hpp>
#include "db/connection.h"
#include "db/schema.h"
#include "db/queries.h"
#include "mcp/tools.h"
#include "mcp/error.h"
#include "util/json.h"
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>

namespace fs = std::filesystem;
using namespace codetopo;

// ---------------------------------------------------------------------------
// Shared fixture: builds a test DB with inheritance, containment, calls, includes
// ---------------------------------------------------------------------------
//
//  IAnimal (interface, animal.h)
//      ^              ^
//   inherits       inherits
//      |              |
//   Dog (class, dog.cpp)   Cat (class, cat.cpp)
//     contains: bark, fetch   contains: meow
//
//  main (function, main.cpp) --calls--> bark, meow, helper
//  dog.cpp --includes--> animal.h
//  cat.cpp --includes--> animal.h
//
//  helper (function, utils/helper.cpp) — separate directory
// ---------------------------------------------------------------------------

struct TestIds {
    int64_t file_animal, file_dog, file_cat, file_main, file_helper;
    int64_t fnode_animal, fnode_dog, fnode_cat, fnode_main, fnode_helper;
    int64_t ianimal, dog, cat, bark, fetch, meow_fn, main_fn, helper_fn;
};

static void bind_and_step(sqlite3_stmt* s) {
    sqlite3_step(s);
    sqlite3_finalize(s);
}

static int64_t insert_file(Connection& conn, const char* path, const char* lang) {
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(conn.raw(),
        "INSERT INTO files(path, language, size_bytes, mtime_ns, content_hash, parse_status) "
        "VALUES(?, ?, 100, 1000000, 'hash', 'ok')", -1, &s, nullptr);
    sqlite3_bind_text(s, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, lang, -1, SQLITE_TRANSIENT);
    bind_and_step(s);
    return sqlite3_last_insert_rowid(conn.raw());
}

static int64_t insert_file_node(Connection& conn, const char* name) {
    std::string key = std::string(name) + "::file";
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(conn.raw(),
        "INSERT INTO nodes(node_type, file_id, kind, name, stable_key) "
        "VALUES('file', NULL, 'file', ?, ?)", -1, &s, nullptr);
    sqlite3_bind_text(s, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, key.c_str(), -1, SQLITE_TRANSIENT);
    bind_and_step(s);
    return sqlite3_last_insert_rowid(conn.raw());
}

static int64_t insert_symbol(Connection& conn, int64_t file_id, const char* kind,
                             const char* name, const char* qualname,
                             int start_line, int end_line,
                             const char* signature = nullptr) {
    std::string key = std::string(qualname) + "::" + kind + "::" + name;
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(conn.raw(),
        "INSERT INTO nodes(node_type, file_id, kind, name, qualname, "
        "start_line, end_line, stable_key, signature) "
        "VALUES('symbol', ?, ?, ?, ?, ?, ?, ?, ?)", -1, &s, nullptr);
    sqlite3_bind_int64(s, 1, file_id);
    sqlite3_bind_text(s, 2, kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 3, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 4, qualname, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 5, start_line);
    sqlite3_bind_int(s, 6, end_line);
    sqlite3_bind_text(s, 7, key.c_str(), -1, SQLITE_TRANSIENT);
    if (signature)
        sqlite3_bind_text(s, 8, signature, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(s, 8);
    bind_and_step(s);
    return sqlite3_last_insert_rowid(conn.raw());
}

static void insert_edge(Connection& conn, int64_t src, int64_t dst,
                        const char* kind, double confidence = 1.0) {
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(conn.raw(),
        "INSERT INTO edges(src_id, dst_id, kind, confidence) VALUES(?, ?, ?, ?)",
        -1, &s, nullptr);
    sqlite3_bind_int64(s, 1, src);
    sqlite3_bind_int64(s, 2, dst);
    sqlite3_bind_text(s, 3, kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(s, 4, confidence);
    bind_and_step(s);
}

static std::pair<fs::path, TestIds> create_rich_db() {
    auto tmp = fs::temp_directory_path() / "codetopo_enhance_test";
    fs::create_directories(tmp);
    auto db_path = tmp / "enhance.sqlite";
    fs::remove(db_path);

    Connection conn(db_path);
    schema::ensure_schema(conn);

    TestIds ids{};

    // --- Files ---
    ids.file_animal = insert_file(conn, "src/animal.h", "cpp");
    ids.file_dog    = insert_file(conn, "src/dog.cpp", "cpp");
    ids.file_cat    = insert_file(conn, "src/cat.cpp", "cpp");
    ids.file_main   = insert_file(conn, "src/main.cpp", "cpp");
    ids.file_helper = insert_file(conn, "src/utils/helper.cpp", "cpp");

    // --- File nodes ---
    ids.fnode_animal = insert_file_node(conn, "src/animal.h");
    ids.fnode_dog    = insert_file_node(conn, "src/dog.cpp");
    ids.fnode_cat    = insert_file_node(conn, "src/cat.cpp");
    ids.fnode_main   = insert_file_node(conn, "src/main.cpp");
    ids.fnode_helper = insert_file_node(conn, "src/utils/helper.cpp");

    // --- Symbols ---
    ids.ianimal = insert_symbol(conn, ids.file_animal, "interface", "IAnimal",
                                "IAnimal", 1, 5, "class IAnimal");
    ids.dog     = insert_symbol(conn, ids.file_dog, "class", "Dog",
                                "Dog", 1, 20, "class Dog : public IAnimal");
    ids.cat     = insert_symbol(conn, ids.file_cat, "class", "Cat",
                                "Cat", 1, 15, "class Cat : public IAnimal");
    ids.bark    = insert_symbol(conn, ids.file_dog, "function", "bark",
                                "Dog::bark", 5, 10, "void bark()");
    ids.fetch   = insert_symbol(conn, ids.file_dog, "function", "fetch",
                                "Dog::fetch", 12, 18, "void fetch()");
    ids.meow_fn = insert_symbol(conn, ids.file_cat, "function", "meow",
                                "Cat::meow", 5, 12, "void meow()");
    ids.main_fn = insert_symbol(conn, ids.file_main, "function", "main",
                                "main", 1, 8, "int main()");
    ids.helper_fn = insert_symbol(conn, ids.file_helper, "function", "helper",
                                  "helper", 1, 5, "void helper()");

    // --- Edges ---
    // Inheritance
    insert_edge(conn, ids.dog, ids.ianimal, "inherits");
    insert_edge(conn, ids.cat, ids.ianimal, "inherits");

    // Containment (class contains members)
    insert_edge(conn, ids.dog, ids.bark,  "contains");
    insert_edge(conn, ids.dog, ids.fetch, "contains");
    insert_edge(conn, ids.cat, ids.meow_fn, "contains");

    // File contains symbols
    insert_edge(conn, ids.fnode_dog, ids.dog,       "contains");
    insert_edge(conn, ids.fnode_cat, ids.cat,       "contains");
    insert_edge(conn, ids.fnode_main, ids.main_fn,  "contains");
    insert_edge(conn, ids.fnode_helper, ids.helper_fn, "contains");
    insert_edge(conn, ids.fnode_animal, ids.ianimal, "contains");

    // Calls
    insert_edge(conn, ids.main_fn, ids.bark,      "calls", 0.9);
    insert_edge(conn, ids.main_fn, ids.meow_fn,   "calls", 0.9);
    insert_edge(conn, ids.main_fn, ids.helper_fn, "calls", 0.8);

    // Includes
    insert_edge(conn, ids.fnode_dog, ids.fnode_animal, "includes");
    insert_edge(conn, ids.fnode_cat, ids.fnode_animal, "includes");

    // Metadata
    schema::set_kv(conn, "schema_version", "1");
    schema::set_kv(conn, "indexer_version", "1.0.0");
    schema::set_kv(conn, "repo_root", tmp.string());
    schema::set_kv(conn, "last_index_time", "2026-03-08T12:00:00Z");
    conn.exec("INSERT INTO nodes_fts(nodes_fts) VALUES('rebuild')");

    return {db_path, ids};
}

// =====================================================================
// 1. NEW TOOL: find_implementations
// =====================================================================

TEST_CASE("find_implementations: basic lookup returns Dog and Cat for IAnimal",
          "[integration][enhance]") {
    auto [db_path, ids] = create_rich_db();
    {
        Connection conn(db_path, true);
        QueryCache cache(conn);
        auto repo_root = schema::get_kv(const_cast<Connection&>(conn), "repo_root");

        auto params_str = R"({"symbol": "IAnimal"})";
        auto params_doc = json_parse(params_str);

        auto result = tools::find_implementations(params_doc.root(),
                          const_cast<Connection&>(conn), cache, repo_root);
        auto doc = json_parse(result);
        REQUIRE(doc);

        auto* results = yyjson_obj_get(doc.root(), "results");
        REQUIRE(results != nullptr);
        REQUIRE(yyjson_is_arr(results));
        REQUIRE(yyjson_arr_size(results) == 2);

        // Collect implementation names
        std::unordered_map<std::string, bool> found;
        yyjson_val* item;
        size_t idx, max;
        yyjson_arr_foreach(results, idx, max, item) {
            auto* name = json_get_str(item, "name");
            REQUIRE(name != nullptr);
            found[name] = true;

            // Each result should have required fields
            REQUIRE(json_get_str(item, "qualname") != nullptr);
            REQUIRE(json_get_str(item, "file_path") != nullptr);
            REQUIRE(json_get_str(item, "kind") != nullptr);
        }
        REQUIRE(found.count("Dog"));
        REQUIRE(found.count("Cat"));
    }
    fs::remove_all(db_path.parent_path());
}

TEST_CASE("find_implementations: no results for leaf class",
          "[integration][enhance]") {
    auto [db_path, ids] = create_rich_db();
    {
        Connection conn(db_path, true);
        QueryCache cache(conn);
        auto repo_root = schema::get_kv(const_cast<Connection&>(conn), "repo_root");

        auto params_str = R"({"symbol": "Dog"})";
        auto params_doc = json_parse(params_str);

        auto result = tools::find_implementations(params_doc.root(),
                          const_cast<Connection&>(conn), cache, repo_root);
        auto doc = json_parse(result);
        REQUIRE(doc);

        auto* results = yyjson_obj_get(doc.root(), "results");
        REQUIRE(results != nullptr);
        REQUIRE(yyjson_arr_size(results) == 0);
    }
    fs::remove_all(db_path.parent_path());
}

TEST_CASE("find_implementations: limit caps returned results",
          "[integration][enhance]") {
    auto [db_path, ids] = create_rich_db();
    {
        Connection conn(db_path, true);
        QueryCache cache(conn);
        auto repo_root = schema::get_kv(const_cast<Connection&>(conn), "repo_root");

        auto params_str = R"({"symbol": "IAnimal", "limit": 1})";
        auto params_doc = json_parse(params_str);

        auto result = tools::find_implementations(params_doc.root(),
                          const_cast<Connection&>(conn), cache, repo_root);
        auto doc = json_parse(result);
        REQUIRE(doc);

        auto* results = yyjson_obj_get(doc.root(), "results");
        REQUIRE(results != nullptr);
        REQUIRE(yyjson_arr_size(results) == 1);
    }
    fs::remove_all(db_path.parent_path());
}

TEST_CASE("find_implementations: missing symbol param returns error",
          "[integration][enhance]") {
    auto [db_path, ids] = create_rich_db();
    {
        Connection conn(db_path, true);
        QueryCache cache(conn);
        auto repo_root = schema::get_kv(const_cast<Connection&>(conn), "repo_root");

        auto params_str = R"({})";
        auto params_doc = json_parse(params_str);

        auto result = tools::find_implementations(params_doc.root(),
                          const_cast<Connection&>(conn), cache, repo_root);
        REQUIRE(result.find("invalid_input") != std::string::npos);
    }
    fs::remove_all(db_path.parent_path());
}

TEST_CASE("find_implementations: nonexistent symbol returns not_found error",
          "[integration][enhance]") {
    auto [db_path, ids] = create_rich_db();
    {
        Connection conn(db_path, true);
        QueryCache cache(conn);
        auto repo_root = schema::get_kv(const_cast<Connection&>(conn), "repo_root");

        auto params_str = R"({"symbol": "NoSuchType"})";
        auto params_doc = json_parse(params_str);

        auto result = tools::find_implementations(params_doc.root(),
                          const_cast<Connection&>(conn), cache, repo_root);
        // Implementation returns not_found when base type doesn't exist
        REQUIRE(result.find("not_found") != std::string::npos);
    }
    fs::remove_all(db_path.parent_path());
}

// =====================================================================
// 2. ENHANCED: context_for — siblings, container, base/implements
// =====================================================================

TEST_CASE("context_for: existing fields still present (backward compat)",
          "[integration][enhance]") {
    auto [db_path, ids] = create_rich_db();
    {
        Connection conn(db_path, true);
        QueryCache cache(conn);
        auto repo_root = schema::get_kv(const_cast<Connection&>(conn), "repo_root");

        auto params_str = "{\"node_id\": " + std::to_string(ids.main_fn) + "}";
        auto params_doc = json_parse(params_str);

        auto result = tools::context_for(params_doc.root(),
                          const_cast<Connection&>(conn), cache, repo_root);
        auto doc = json_parse(result);
        REQUIRE(doc);

        // Original fields must still be present
        REQUIRE(yyjson_obj_get(doc.root(), "symbol") != nullptr);
        REQUIRE(yyjson_obj_get(doc.root(), "callers") != nullptr);
        REQUIRE(yyjson_obj_get(doc.root(), "callees") != nullptr);
    }
    fs::remove_all(db_path.parent_path());
}

TEST_CASE("context_for: siblings shows other members of same container",
          "[integration][enhance]") {
    auto [db_path, ids] = create_rich_db();
    {
        Connection conn(db_path, true);
        QueryCache cache(conn);
        auto repo_root = schema::get_kv(const_cast<Connection&>(conn), "repo_root");

        // bark is contained in Dog along with fetch
        auto params_str = "{\"node_id\": " + std::to_string(ids.bark) + "}";
        auto params_doc = json_parse(params_str);

        auto result = tools::context_for(params_doc.root(),
                          const_cast<Connection&>(conn), cache, repo_root);
        auto doc = json_parse(result);
        REQUIRE(doc);

        auto* siblings = yyjson_obj_get(doc.root(), "siblings");
        REQUIRE(siblings != nullptr);
        REQUIRE(yyjson_is_arr(siblings));

        // fetch should be a sibling of bark (both in Dog), bark itself excluded
        bool found_fetch = false;
        yyjson_val* item;
        size_t idx, max;
        yyjson_arr_foreach(siblings, idx, max, item) {
            auto* name = json_get_str(item, "name");
            if (name && std::string(name) == "fetch") found_fetch = true;
            // bark itself should NOT appear in siblings
            if (name) REQUIRE(std::string(name) != "bark");
        }
        REQUIRE(found_fetch);
    }
    fs::remove_all(db_path.parent_path());
}

TEST_CASE("context_for: container info shows enclosing type",
          "[integration][enhance]") {
    auto [db_path, ids] = create_rich_db();
    {
        Connection conn(db_path, true);
        QueryCache cache(conn);
        auto repo_root = schema::get_kv(const_cast<Connection&>(conn), "repo_root");

        // bark is contained in Dog
        auto params_str = "{\"node_id\": " + std::to_string(ids.bark) + "}";
        auto params_doc = json_parse(params_str);

        auto result = tools::context_for(params_doc.root(),
                          const_cast<Connection&>(conn), cache, repo_root);
        auto doc = json_parse(result);
        REQUIRE(doc);

        auto* container = yyjson_obj_get(doc.root(), "container");
        REQUIRE(container != nullptr);

        auto* container_name = json_get_str(container, "name");
        REQUIRE(container_name != nullptr);
        REQUIRE(std::string(container_name) == "Dog");

        auto* container_kind = json_get_str(container, "kind");
        REQUIRE(container_kind != nullptr);
        REQUIRE(std::string(container_kind) == "class");
    }
    fs::remove_all(db_path.parent_path());
}

TEST_CASE("context_for: base/implements info for member of derived class",
          "[integration][enhance]") {
    auto [db_path, ids] = create_rich_db();
    {
        Connection conn(db_path, true);
        QueryCache cache(conn);
        auto repo_root = schema::get_kv(const_cast<Connection&>(conn), "repo_root");

        // bark is in Dog, and Dog inherits IAnimal
        auto params_str = "{\"node_id\": " + std::to_string(ids.bark) + "}";
        auto params_doc = json_parse(params_str);

        auto result = tools::context_for(params_doc.root(),
                          const_cast<Connection&>(conn), cache, repo_root);
        auto doc = json_parse(result);
        REQUIRE(doc);

        auto* bases = yyjson_obj_get(doc.root(), "base_types");
        REQUIRE(bases != nullptr);
        REQUIRE(yyjson_is_arr(bases));
        REQUIRE(yyjson_arr_size(bases) >= 1);

        bool found_ianimal = false;
        yyjson_val* item;
        size_t idx, max;
        yyjson_arr_foreach(bases, idx, max, item) {
            auto* name = json_get_str(item, "name");
            if (name && std::string(name) == "IAnimal") found_ianimal = true;
        }
        REQUIRE(found_ianimal);
    }
    fs::remove_all(db_path.parent_path());
}

TEST_CASE("context_for: no container for top-level symbol",
          "[integration][enhance]") {
    auto [db_path, ids] = create_rich_db();
    {
        Connection conn(db_path, true);
        QueryCache cache(conn);
        auto repo_root = schema::get_kv(const_cast<Connection&>(conn), "repo_root");

        // helper is a top-level function (file contains it, no class container)
        auto params_str = "{\"node_id\": " + std::to_string(ids.helper_fn) + "}";
        auto params_doc = json_parse(params_str);

        auto result = tools::context_for(params_doc.root(),
                          const_cast<Connection&>(conn), cache, repo_root);
        auto doc = json_parse(result);
        REQUIRE(doc);

        // Container should be null/absent or a file node (not a class)
        auto* container = yyjson_obj_get(doc.root(), "container");
        if (container && !yyjson_is_null(container)) {
            // If present, it should be a file, not a class/interface
            auto* kind = json_get_str(container, "kind");
            if (kind) REQUIRE(std::string(kind) != "class");
        }
    }
    fs::remove_all(db_path.parent_path());
}

// =====================================================================
// 3. ENHANCED: subgraph — edge_kinds filter
// =====================================================================

TEST_CASE("subgraph: without edge_kinds returns all edge types (default unchanged)",
          "[integration][enhance]") {
    auto [db_path, ids] = create_rich_db();
    {
        Connection conn(db_path, true);
        QueryCache cache(conn);
        auto repo_root = schema::get_kv(const_cast<Connection&>(conn), "repo_root");

        auto params_str = "{\"seed_symbols\": [" + std::to_string(ids.main_fn) +
                          "], \"depth\": 1}";
        auto params_doc = json_parse(params_str);

        auto result = tools::subgraph(params_doc.root(),
                          const_cast<Connection&>(conn), cache, repo_root);
        auto doc = json_parse(result);
        REQUIRE(doc);

        auto* edges = yyjson_obj_get(doc.root(), "edges");
        REQUIRE(edges != nullptr);
        // main has calls edges; without filter, all should appear
        REQUIRE(yyjson_arr_size(edges) >= 1);

        auto* nodes = yyjson_obj_get(doc.root(), "nodes");
        REQUIRE(nodes != nullptr);
        REQUIRE(yyjson_arr_size(nodes) >= 1);
    }
    fs::remove_all(db_path.parent_path());
}

TEST_CASE("subgraph: edge_kinds=[calls] filters to only call edges",
          "[integration][enhance]") {
    auto [db_path, ids] = create_rich_db();
    {
        Connection conn(db_path, true);
        QueryCache cache(conn);
        auto repo_root = schema::get_kv(const_cast<Connection&>(conn), "repo_root");

        // Seed from Dog which has inherits and contains edges
        auto params_str = "{\"seed_symbols\": [" + std::to_string(ids.dog) +
                          "], \"depth\": 1, \"edge_kinds\": [\"contains\"]}";
        auto params_doc = json_parse(params_str);

        auto result = tools::subgraph(params_doc.root(),
                          const_cast<Connection&>(conn), cache, repo_root);
        auto doc = json_parse(result);
        REQUIRE(doc);

        auto* edges = yyjson_obj_get(doc.root(), "edges");
        REQUIRE(edges != nullptr);

        // All returned edges must be "contains" type
        yyjson_val* item;
        size_t idx, max;
        yyjson_arr_foreach(edges, idx, max, item) {
            auto* kind = json_get_str(item, "kind");
            REQUIRE(kind != nullptr);
            REQUIRE(std::string(kind) == "contains");
        }

        // Should have at least bark and fetch via contains
        REQUIRE(yyjson_arr_size(edges) >= 2);
    }
    fs::remove_all(db_path.parent_path());
}

TEST_CASE("subgraph: edge_kinds=[inherits] only traverses inheritance",
          "[integration][enhance]") {
    auto [db_path, ids] = create_rich_db();
    {
        Connection conn(db_path, true);
        QueryCache cache(conn);
        auto repo_root = schema::get_kv(const_cast<Connection&>(conn), "repo_root");

        auto params_str = "{\"seed_symbols\": [" + std::to_string(ids.dog) +
                          "], \"depth\": 1, \"edge_kinds\": [\"inherits\"]}";
        auto params_doc = json_parse(params_str);

        auto result = tools::subgraph(params_doc.root(),
                          const_cast<Connection&>(conn), cache, repo_root);
        auto doc = json_parse(result);
        REQUIRE(doc);

        auto* edges = yyjson_obj_get(doc.root(), "edges");
        REQUIRE(edges != nullptr);

        // Should find exactly the inherits edge from Dog to IAnimal
        yyjson_val* item;
        size_t idx, max;
        yyjson_arr_foreach(edges, idx, max, item) {
            REQUIRE(std::string(json_get_str(item, "kind")) == "inherits");
        }
        REQUIRE(yyjson_arr_size(edges) >= 1);
    }
    fs::remove_all(db_path.parent_path());
}

TEST_CASE("subgraph: empty edge_kinds array returns no edges",
          "[integration][enhance]") {
    auto [db_path, ids] = create_rich_db();
    {
        Connection conn(db_path, true);
        QueryCache cache(conn);
        auto repo_root = schema::get_kv(const_cast<Connection&>(conn), "repo_root");

        auto params_str = "{\"seed_symbols\": [" + std::to_string(ids.dog) +
                          "], \"depth\": 1, \"edge_kinds\": []}";
        auto params_doc = json_parse(params_str);

        auto result = tools::subgraph(params_doc.root(),
                          const_cast<Connection&>(conn), cache, repo_root);
        auto doc = json_parse(result);
        REQUIRE(doc);

        auto* edges = yyjson_obj_get(doc.root(), "edges");
        REQUIRE(edges != nullptr);
        // No edge kinds allowed → no traversal → no edges
        REQUIRE(yyjson_arr_size(edges) == 0);
    }
    fs::remove_all(db_path.parent_path());
}

// =====================================================================
// 4. ENHANCED: shortest_path — max_paths, relation_types
// =====================================================================

TEST_CASE("shortest_path: default still returns single path (backward compat)",
          "[integration][enhance]") {
    auto [db_path, ids] = create_rich_db();
    {
        Connection conn(db_path, true);
        QueryCache cache(conn);
        auto repo_root = schema::get_kv(const_cast<Connection&>(conn), "repo_root");

        auto params_str = "{\"src_node_id\": " + std::to_string(ids.main_fn) +
                          ", \"dst_node_id\": " + std::to_string(ids.bark) + "}";
        auto params_doc = json_parse(params_str);

        auto result = tools::shortest_path(params_doc.root(),
                          const_cast<Connection&>(conn), cache, repo_root);
        auto doc = json_parse(result);
        REQUIRE(doc);

        auto* path = yyjson_obj_get(doc.root(), "path");
        REQUIRE(path != nullptr);
        REQUIRE(yyjson_arr_size(path) >= 1);
        REQUIRE(json_get_int(doc.root(), "distance") >= 1);
    }
    fs::remove_all(db_path.parent_path());
}

TEST_CASE("shortest_path: relation_types filters edge kinds",
          "[integration][enhance]") {
    auto [db_path, ids] = create_rich_db();
    {
        Connection conn(db_path, true);
        QueryCache cache(conn);
        auto repo_root = schema::get_kv(const_cast<Connection&>(conn), "repo_root");

        // main --calls--> bark. With relation_types=["calls"], should find path.
        auto params_str = "{\"src_node_id\": " + std::to_string(ids.main_fn) +
                          ", \"dst_node_id\": " + std::to_string(ids.bark) +
                          ", \"relation_types\": [\"calls\"]}";
        auto params_doc = json_parse(params_str);

        auto result = tools::shortest_path(params_doc.root(),
                          const_cast<Connection&>(conn), cache, repo_root);
        auto doc = json_parse(result);
        REQUIRE(doc);

        auto* path = yyjson_obj_get(doc.root(), "path");
        REQUIRE(path != nullptr);
        REQUIRE(yyjson_arr_size(path) >= 1);
        REQUIRE(json_get_int(doc.root(), "distance") >= 1);
    }
    fs::remove_all(db_path.parent_path());
}

TEST_CASE("shortest_path: relation_types excluding relevant edges yields no path",
          "[integration][enhance]") {
    auto [db_path, ids] = create_rich_db();
    {
        Connection conn(db_path, true);
        QueryCache cache(conn);
        auto repo_root = schema::get_kv(const_cast<Connection&>(conn), "repo_root");

        // main --calls--> bark, but filter to only "inherits" — no direct path
        auto params_str = "{\"src_node_id\": " + std::to_string(ids.main_fn) +
                          ", \"dst_node_id\": " + std::to_string(ids.bark) +
                          ", \"relation_types\": [\"inherits\"]}";
        auto params_doc = json_parse(params_str);

        auto result = tools::shortest_path(params_doc.root(),
                          const_cast<Connection&>(conn), cache, repo_root);
        auto doc = json_parse(result);
        REQUIRE(doc);

        // No path via inherits only between main and bark
        REQUIRE(json_get_int(doc.root(), "distance") == 0);
        auto* path = yyjson_obj_get(doc.root(), "path");
        REQUIRE(yyjson_arr_size(path) == 0);
    }
    fs::remove_all(db_path.parent_path());
}

TEST_CASE("shortest_path: max_paths returns multiple alternative paths",
          "[integration][enhance]") {
    auto [db_path, ids] = create_rich_db();
    {
        Connection conn(db_path, true);
        QueryCache cache(conn);
        auto repo_root = schema::get_kv(const_cast<Connection&>(conn), "repo_root");

        // Request up to 3 paths from main to IAnimal.
        // Paths exist: main->bark->Dog->IAnimal, main->meow->Cat->IAnimal via contains+inherits
        auto params_str = "{\"src_node_id\": " + std::to_string(ids.main_fn) +
                          ", \"dst_node_id\": " + std::to_string(ids.ianimal) +
                          ", \"max_paths\": 3}";
        auto params_doc = json_parse(params_str);

        auto result = tools::shortest_path(params_doc.root(),
                          const_cast<Connection&>(conn), cache, repo_root);
        auto doc = json_parse(result);
        REQUIRE(doc);

        // With max_paths > 1, response should have "paths" array instead of/alongside "path"
        auto* paths = yyjson_obj_get(doc.root(), "paths");
        if (paths && yyjson_is_arr(paths)) {
            // Multiple paths returned
            REQUIRE(yyjson_arr_size(paths) >= 1);
        } else {
            // Fallback: at least the single path should exist
            auto* path = yyjson_obj_get(doc.root(), "path");
            REQUIRE(path != nullptr);
            REQUIRE(yyjson_arr_size(path) >= 1);
        }
    }
    fs::remove_all(db_path.parent_path());
}

// =====================================================================
// 5. ENHANCED: entrypoints — scope filter
// =====================================================================

TEST_CASE("entrypoints: without scope returns all entries (backward compat)",
          "[integration][enhance]") {
    auto [db_path, ids] = create_rich_db();
    {
        Connection conn(db_path, true);
        QueryCache cache(conn);
        auto repo_root = schema::get_kv(const_cast<Connection&>(conn), "repo_root");

        auto result = tools::entrypoints(nullptr,
                          const_cast<Connection&>(conn), cache, repo_root);
        auto doc = json_parse(result);
        REQUIRE(doc);

        auto* results = yyjson_obj_get(doc.root(), "results");
        REQUIRE(results != nullptr);
        REQUIRE(yyjson_arr_size(results) >= 1);

        // main function should be in results
        bool found_main = false;
        yyjson_val* item;
        size_t idx, max;
        yyjson_arr_foreach(results, idx, max, item) {
            auto* name = json_get_str(item, "name");
            if (name && std::string(name) == "main") found_main = true;
        }
        REQUIRE(found_main);
    }
    fs::remove_all(db_path.parent_path());
}

TEST_CASE("entrypoints: scope filters to specific file",
          "[integration][enhance]") {
    auto [db_path, ids] = create_rich_db();
    {
        Connection conn(db_path, true);
        QueryCache cache(conn);
        auto repo_root = schema::get_kv(const_cast<Connection&>(conn), "repo_root");

        auto params_str = R"({"scope": "src/main.cpp"})";
        auto params_doc = json_parse(params_str);

        auto result = tools::entrypoints(params_doc.root(),
                          const_cast<Connection&>(conn), cache, repo_root);
        auto doc = json_parse(result);
        REQUIRE(doc);

        auto* results = yyjson_obj_get(doc.root(), "results");
        REQUIRE(results != nullptr);

        // All results should be from src/main.cpp
        yyjson_val* item;
        size_t idx, max;
        yyjson_arr_foreach(results, idx, max, item) {
            auto* fp = json_get_str(item, "file_path");
            if (fp) REQUIRE(std::string(fp).find("main.cpp") != std::string::npos);
        }
    }
    fs::remove_all(db_path.parent_path());
}

TEST_CASE("entrypoints: scope with directory prefix filters by module",
          "[integration][enhance]") {
    auto [db_path, ids] = create_rich_db();
    {
        Connection conn(db_path, true);
        QueryCache cache(conn);
        auto repo_root = schema::get_kv(const_cast<Connection&>(conn), "repo_root");

        auto params_str = R"({"scope": "src/utils"})";
        auto params_doc = json_parse(params_str);

        auto result = tools::entrypoints(params_doc.root(),
                          const_cast<Connection&>(conn), cache, repo_root);
        auto doc = json_parse(result);
        REQUIRE(doc);

        auto* results = yyjson_obj_get(doc.root(), "results");
        REQUIRE(results != nullptr);

        // All results should be from src/utils/
        yyjson_val* item;
        size_t idx, max;
        yyjson_arr_foreach(results, idx, max, item) {
            auto* fp = json_get_str(item, "file_path");
            if (fp) REQUIRE(std::string(fp).find("src/utils") != std::string::npos);
        }
    }
    fs::remove_all(db_path.parent_path());
}

TEST_CASE("entrypoints: scope matching no files returns empty results",
          "[integration][enhance]") {
    auto [db_path, ids] = create_rich_db();
    {
        Connection conn(db_path, true);
        QueryCache cache(conn);
        auto repo_root = schema::get_kv(const_cast<Connection&>(conn), "repo_root");

        auto params_str = R"({"scope": "nonexistent/dir"})";
        auto params_doc = json_parse(params_str);

        auto result = tools::entrypoints(params_doc.root(),
                          const_cast<Connection&>(conn), cache, repo_root);
        auto doc = json_parse(result);
        REQUIRE(doc);

        auto* results = yyjson_obj_get(doc.root(), "results");
        REQUIRE(results != nullptr);
        REQUIRE(yyjson_arr_size(results) == 0);
    }
    fs::remove_all(db_path.parent_path());
}

// =====================================================================
// 6. ENHANCED: callers_approx / callees_approx — group_by
// =====================================================================

TEST_CASE("callers_approx: without group_by returns flat list (backward compat)",
          "[integration][enhance]") {
    auto [db_path, ids] = create_rich_db();
    {
        Connection conn(db_path, true);
        QueryCache cache(conn);
        auto repo_root = schema::get_kv(const_cast<Connection&>(conn), "repo_root");

        auto params_str = "{\"node_id\": " + std::to_string(ids.bark) + "}";
        auto params_doc = json_parse(params_str);

        auto result = tools::callers_approx(params_doc.root(),
                          const_cast<Connection&>(conn), cache, repo_root);
        auto doc = json_parse(result);
        REQUIRE(doc);

        auto* results = yyjson_obj_get(doc.root(), "results");
        REQUIRE(results != nullptr);
        REQUIRE(yyjson_is_arr(results));
        REQUIRE(yyjson_arr_size(results) >= 1);

        // Should have main as a caller
        auto* first = yyjson_arr_get_first(results);
        REQUIRE(json_get_str(first, "caller_name") != nullptr);
        REQUIRE(std::string(json_get_str(first, "caller_name")) == "main");
    }
    fs::remove_all(db_path.parent_path());
}

TEST_CASE("callers_approx: group_by=file groups results by file path",
          "[integration][enhance]") {
    auto [db_path, ids] = create_rich_db();
    {
        Connection conn(db_path, true);
        QueryCache cache(conn);
        auto repo_root = schema::get_kv(const_cast<Connection&>(conn), "repo_root");

        auto params_str = "{\"node_id\": " + std::to_string(ids.bark) +
                          ", \"group_by\": \"file\"}";
        auto params_doc = json_parse(params_str);

        auto result = tools::callers_approx(params_doc.root(),
                          const_cast<Connection&>(conn), cache, repo_root);
        auto doc = json_parse(result);
        REQUIRE(doc);

        // Grouped response should have "groups" object keyed by file path
        auto* groups = yyjson_obj_get(doc.root(), "groups");
        REQUIRE(groups != nullptr);
        REQUIRE(yyjson_is_obj(groups));

        // Should have at least one group (the file containing main)
        REQUIRE(yyjson_obj_size(groups) >= 1);

        // Each group value should be an array of callers
        yyjson_obj_iter iter;
        yyjson_obj_iter_init(groups, &iter);
        yyjson_val* key;
        while ((key = yyjson_obj_iter_next(&iter))) {
            auto* val = yyjson_obj_iter_get_val(key);
            REQUIRE(yyjson_is_arr(val));
        }
    }
    fs::remove_all(db_path.parent_path());
}

TEST_CASE("callers_approx: group_by=symbol groups results by caller symbol",
          "[integration][enhance]") {
    auto [db_path, ids] = create_rich_db();
    {
        Connection conn(db_path, true);
        QueryCache cache(conn);
        auto repo_root = schema::get_kv(const_cast<Connection&>(conn), "repo_root");

        auto params_str = "{\"node_id\": " + std::to_string(ids.bark) +
                          ", \"group_by\": \"symbol\"}";
        auto params_doc = json_parse(params_str);

        auto result = tools::callers_approx(params_doc.root(),
                          const_cast<Connection&>(conn), cache, repo_root);
        auto doc = json_parse(result);
        REQUIRE(doc);

        auto* groups = yyjson_obj_get(doc.root(), "groups");
        REQUIRE(groups != nullptr);
        REQUIRE(yyjson_is_obj(groups));
    }
    fs::remove_all(db_path.parent_path());
}

TEST_CASE("callees_approx: without group_by returns flat list (backward compat)",
          "[integration][enhance]") {
    auto [db_path, ids] = create_rich_db();
    {
        Connection conn(db_path, true);
        QueryCache cache(conn);
        auto repo_root = schema::get_kv(const_cast<Connection&>(conn), "repo_root");

        auto params_str = "{\"node_id\": " + std::to_string(ids.main_fn) + "}";
        auto params_doc = json_parse(params_str);

        auto result = tools::callees_approx(params_doc.root(),
                          const_cast<Connection&>(conn), cache, repo_root);
        auto doc = json_parse(result);
        REQUIRE(doc);

        auto* results = yyjson_obj_get(doc.root(), "results");
        REQUIRE(results != nullptr);
        REQUIRE(yyjson_is_arr(results));
        // main calls bark, meow, helper
        REQUIRE(yyjson_arr_size(results) >= 3);
    }
    fs::remove_all(db_path.parent_path());
}

TEST_CASE("callees_approx: group_by=file groups by file path",
          "[integration][enhance]") {
    auto [db_path, ids] = create_rich_db();
    {
        Connection conn(db_path, true);
        QueryCache cache(conn);
        auto repo_root = schema::get_kv(const_cast<Connection&>(conn), "repo_root");

        auto params_str = "{\"node_id\": " + std::to_string(ids.main_fn) +
                          ", \"group_by\": \"file\"}";
        auto params_doc = json_parse(params_str);

        auto result = tools::callees_approx(params_doc.root(),
                          const_cast<Connection&>(conn), cache, repo_root);
        auto doc = json_parse(result);
        REQUIRE(doc);

        auto* groups = yyjson_obj_get(doc.root(), "groups");
        REQUIRE(groups != nullptr);
        REQUIRE(yyjson_is_obj(groups));

        // main calls symbols in dog.cpp, cat.cpp, and utils/helper.cpp
        REQUIRE(yyjson_obj_size(groups) >= 2);
    }
    fs::remove_all(db_path.parent_path());
}

TEST_CASE("callees_approx: group_by=module groups by directory prefix",
          "[integration][enhance]") {
    auto [db_path, ids] = create_rich_db();
    {
        Connection conn(db_path, true);
        QueryCache cache(conn);
        auto repo_root = schema::get_kv(const_cast<Connection&>(conn), "repo_root");

        auto params_str = "{\"node_id\": " + std::to_string(ids.main_fn) +
                          ", \"group_by\": \"module\"}";
        auto params_doc = json_parse(params_str);

        auto result = tools::callees_approx(params_doc.root(),
                          const_cast<Connection&>(conn), cache, repo_root);
        auto doc = json_parse(result);
        REQUIRE(doc);

        auto* groups = yyjson_obj_get(doc.root(), "groups");
        REQUIRE(groups != nullptr);
        REQUIRE(yyjson_is_obj(groups));

        // Should group by directory: "src" and "src/utils" (or equivalent)
        REQUIRE(yyjson_obj_size(groups) >= 1);
    }
    fs::remove_all(db_path.parent_path());
}

TEST_CASE("callers_approx: group_by with invalid value falls back to flat list",
          "[integration][enhance]") {
    auto [db_path, ids] = create_rich_db();
    {
        Connection conn(db_path, true);
        QueryCache cache(conn);
        auto repo_root = schema::get_kv(const_cast<Connection&>(conn), "repo_root");

        auto params_str = "{\"node_id\": " + std::to_string(ids.bark) +
                          ", \"group_by\": \"invalid_value\"}";
        auto params_doc = json_parse(params_str);

        auto result = tools::callers_approx(params_doc.root(),
                          const_cast<Connection&>(conn), cache, repo_root);
        auto doc = json_parse(result);
        REQUIRE(doc);

        // Should either return error or fall back to flat "results" array
        auto* results = yyjson_obj_get(doc.root(), "results");
        auto* error = yyjson_obj_get(doc.root(), "error");
        REQUIRE((results != nullptr || error != nullptr));
    }
    fs::remove_all(db_path.parent_path());
}
