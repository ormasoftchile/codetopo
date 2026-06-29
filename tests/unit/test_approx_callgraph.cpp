// Regression tests for refs-backed approximate call-site recovery.

#include <catch2/catch_test_macros.hpp>
#include "db/connection.h"
#include "db/schema.h"
#include "db/queries.h"
#include "mcp/tools.h"
#include "util/json.h"
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace codetopo;

namespace {

void cleanup(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
}

struct ApproxIds {
    int64_t linked_set = 0;
    int64_t use_linked = 0;
    int64_t exact_set_caller = 0;
    int64_t flush = 0;
    int64_t flush_caller = 0;
};

struct TestDb {
    fs::path root;
    fs::path db_path;
    ApproxIds ids;

    TestDb() = default;
    TestDb(const TestDb&) = delete;
    TestDb& operator=(const TestDb&) = delete;
    TestDb(TestDb&& other) noexcept
        : root(std::move(other.root)), db_path(std::move(other.db_path)), ids(other.ids) {
        other.root.clear();
    }
    TestDb& operator=(TestDb&& other) noexcept {
        if (this != &other) {
            cleanup(root);
            root = std::move(other.root);
            db_path = std::move(other.db_path);
            ids = other.ids;
            other.root.clear();
        }
        return *this;
    }
    ~TestDb() { cleanup(root); }
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
        "VALUES(?, 'typescript', 100, 1000000, ?, 'ok')",
        -1, &stmt, nullptr) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
    std::string hash = std::string("hash:") + path;
    sqlite3_bind_text(stmt, 2, hash.c_str(), -1, SQLITE_TRANSIENT);
    step_done(conn, stmt);
    return sqlite3_last_insert_rowid(conn.raw());
}

static int64_t insert_symbol(Connection& conn, int64_t file_id, const char* kind,
                             const char* name, const char* qualname,
                             int start_line, int end_line,
                             const char* signature = nullptr) {
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(conn.raw(),
        "INSERT INTO nodes(node_type, file_id, kind, name, qualname, start_line, end_line, stable_key, signature) "
        "VALUES('symbol', ?, ?, ?, ?, ?, ?, ?, ?)",
        -1, &stmt, nullptr) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, file_id);
    sqlite3_bind_text(stmt, 2, kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, qualname, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, start_line);
    sqlite3_bind_int(stmt, 6, end_line);
    std::string stable_key = std::string(qualname) + "::" + kind + "::" + name +
                             "::" + std::to_string(start_line);
    sqlite3_bind_text(stmt, 7, stable_key.c_str(), -1, SQLITE_TRANSIENT);
    if (signature)
        sqlite3_bind_text(stmt, 8, signature, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(stmt, 8);
    step_done(conn, stmt);
    return sqlite3_last_insert_rowid(conn.raw());
}

static int64_t insert_file_node(Connection& conn, const char* path) {
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(conn.raw(),
        "INSERT INTO nodes(node_type, kind, name, stable_key) VALUES('file', 'file', ?, ?)",
        -1, &stmt, nullptr) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
    std::string stable_key = std::string("file:") + path;
    sqlite3_bind_text(stmt, 2, stable_key.c_str(), -1, SQLITE_TRANSIENT);
    step_done(conn, stmt);
    return sqlite3_last_insert_rowid(conn.raw());
}

static void insert_edge(Connection& conn, int64_t src, int64_t dst,
                        const char* kind, double confidence = 1.0) {
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

static void insert_call_ref(Connection& conn, int64_t file_id, const char* name,
                            int start_line, int start_col, int end_col,
                            int64_t containing_node_id, int arg_count = -1,
                            const char* arg_pattern = nullptr,
                            const char* receiver_type_hint = nullptr) {
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(conn.raw(),
        "INSERT INTO refs(file_id, kind, name, start_line, start_col, end_line, end_col, "
        "evidence, containing_node_id, arg_count, arg_pattern, receiver_type_hint) "
        "VALUES(?, 'call', ?, ?, ?, ?, ?, 'call_expression', ?, ?, ?, ?)",
        -1, &stmt, nullptr) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, file_id);
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, start_line);
    sqlite3_bind_int(stmt, 4, start_col);
    sqlite3_bind_int(stmt, 5, start_line);
    sqlite3_bind_int(stmt, 6, end_col);
    sqlite3_bind_int64(stmt, 7, containing_node_id);
    if (arg_count >= 0) sqlite3_bind_int(stmt, 8, arg_count);
    else sqlite3_bind_null(stmt, 8);
    if (arg_pattern) sqlite3_bind_text(stmt, 9, arg_pattern, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 9);
    if (receiver_type_hint) sqlite3_bind_text(stmt, 10, receiver_type_hint, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 10);
    step_done(conn, stmt);
}

static TestDb make_approx_db(const std::string& name, bool exact_set_edge = false,
                             int extra_linked_set_refs = 0) {
    TestDb db;
    db.root = fs::path("build") / "test_approx_callgraph" / name;
    cleanup(db.root);
    fs::create_directories(db.root);
    db.db_path = db.root / "codetopo.sqlite";

    Connection conn(db.db_path);
    schema::ensure_schema(conn);

    int64_t map_file = insert_file(conn, "src/linked_map.ts");
    int64_t other_file = insert_file(conn, "src/other_map.ts");
    int64_t caller_file = insert_file(conn, "src/use_map.ts");
    int64_t exact_file = insert_file(conn, "src/exact_set.ts");
    int64_t unique_file = insert_file(conn, "src/use_flush.ts");

    int64_t linked_map = insert_symbol(conn, map_file, "class", "LinkedMap", "LinkedMap", 1, 20);
    db.ids.linked_set = insert_symbol(conn, map_file, "method", "set", "LinkedMap.set", 3, 5);
    int64_t linked_set_overload = insert_symbol(conn, map_file, "method", "set", "LinkedMap.set overload", 7, 9);
    db.ids.flush = insert_symbol(conn, map_file, "method", "flush", "LinkedMap.flush", 11, 13);
    int64_t other_map = insert_symbol(conn, other_file, "class", "OtherMap", "OtherMap", 1, 8);
    int64_t other_set = insert_symbol(conn, other_file, "method", "set", "OtherMap.set", 3, 5);

    db.ids.use_linked = insert_symbol(conn, caller_file, "function", "useLinked", "useLinked", 1, 6);
    db.ids.exact_set_caller = insert_symbol(conn, exact_file, "function", "setExactly", "setExactly", 1, 4);
    db.ids.flush_caller = insert_symbol(conn, unique_file, "function", "callFlush", "callFlush", 1, 4);

    insert_edge(conn, linked_map, db.ids.linked_set, "contains");
    insert_edge(conn, linked_map, linked_set_overload, "contains");
    insert_edge(conn, linked_map, db.ids.flush, "contains");
    insert_edge(conn, other_map, other_set, "contains");
    insert_edge(conn, db.ids.flush_caller, db.ids.flush, "calls", 0.9);
    if (exact_set_edge) {
        insert_edge(conn, db.ids.exact_set_caller, db.ids.linked_set, "calls", 0.9);
    }

    insert_call_ref(conn, caller_file, "linked.set", 4, 3, 13, db.ids.use_linked);
    insert_call_ref(conn, caller_file, "this.set", 5, 3, 11, db.ids.use_linked);
    insert_call_ref(conn, caller_file, "LinkedMap.set", 6, 3, 16, db.ids.use_linked);
    insert_call_ref(conn, caller_file, "unrelated.set", 7, 3, 17, db.ids.use_linked);
    insert_call_ref(conn, caller_file, "OtherMap.set", 8, 3, 16, db.ids.use_linked);
    for (int i = 0; i < extra_linked_set_refs; ++i) {
        insert_call_ref(conn, caller_file, "LinkedMap.set", 20 + i, 3, 16, db.ids.use_linked);
    }

    schema::set_kv(conn, "schema_version", "1");
    schema::set_kv(conn, "indexer_version", "test");
    schema::set_kv(conn, "repo_root", db.root.string());
    schema::set_kv(conn, "last_index_time", "2026-06-28T22:59:24-04:00");
    conn.wal_checkpoint();

    return db;
}

static std::string invoke_callers_raw(const TestDb& db, const std::string& params) {
    Connection conn(db.db_path, true);
    QueryCache cache(conn);
    auto params_doc = json_parse(params);
    REQUIRE(params_doc);
    return tools::callers_approx(params_doc.root(), conn, cache, db.root.string());
}

static std::string invoke_callers(const TestDb& db, int64_t node_id,
                                 const char* mode = "exact_then_candidates") {
    std::string params = "{\"node_id\":" + std::to_string(node_id) +
                         ",\"limit\":20,\"include_candidates\":true,\"mode\":\"" +
                         mode + "\",\"include_handles\":true}";
    return invoke_callers_raw(db, params);
}

static std::string invoke_context_for(const TestDb& db, int64_t node_id) {
    Connection conn(db.db_path, true);
    QueryCache cache(conn);
    std::string params = "{\"node_id\":" + std::to_string(node_id) +
                         ",\"include_source\":false,\"max_callers\":20,"
                         "\"include_candidates\":true,\"mode\":\"exact_then_candidates\","
                         "\"include_handles\":true}";
    auto params_doc = json_parse(params);
    REQUIRE(params_doc);
    return tools::context_for(params_doc.root(), conn, cache, db.root.string());
}

static std::string invoke_impact_of(const TestDb& db, int64_t node_id) {
    Connection conn(db.db_path, true);
    QueryCache cache(conn);
    std::string params = "{\"node_id\":" + std::to_string(node_id) +
                         ",\"depth\":1,\"max_nodes\":20,"
                         "\"include_candidates\":true,\"mode\":\"exact_then_candidates\","
                         "\"include_handles\":true}";
    auto params_doc = json_parse(params);
    REQUIRE(params_doc);
    return tools::impact_of(params_doc.root(), conn, cache, db.root.string());
}

static yyjson_val* require_array_field(yyjson_val* obj, const char* key) {
    yyjson_val* arr = yyjson_obj_get(obj, key);
    INFO("expected JSON array field: " << key);
    REQUIRE(arr != nullptr);
    REQUIRE(yyjson_is_arr(arr));
    return arr;
}

static bool has_field(yyjson_val* obj, const char* key) {
    return yyjson_obj_get(obj, key) != nullptr;
}

static yyjson_val* find_item_with_string(yyjson_val* arr, const char* key,
                                         const std::string& value) {
    yyjson_val* item = nullptr;
    yyjson_arr_iter iter;
    yyjson_arr_iter_init(arr, &iter);
    while ((item = yyjson_arr_iter_next(&iter))) {
        const char* field = json_get_str(item, key);
        if (field && value == field) return item;
    }
    return nullptr;
}

static double require_confidence(yyjson_val* candidate) {
    REQUIRE(candidate != nullptr);
    yyjson_val* confidence = yyjson_obj_get(candidate, "confidence");
    REQUIRE(confidence != nullptr);
    REQUIRE(yyjson_is_num(confidence));
    return yyjson_get_real(confidence);
}

static void require_callsite_candidate(yyjson_val* candidate, const std::string& callee_text) {
    REQUIRE(candidate != nullptr);
    const char* file_path = json_get_str(candidate, "file_path");
    REQUIRE(file_path != nullptr);
    CHECK(std::string(file_path) == "src/use_map.ts");

    yyjson_val* span = yyjson_obj_get(candidate, "span");
    REQUIRE(span != nullptr);
    CHECK(json_get_int(span, "start_line", -1) == 4);
    CHECK(json_get_int(span, "start_col", -1) == 3);
    CHECK(json_get_int(span, "end_line", -1) == 4);
    CHECK(json_get_int(span, "end_col", -1) == 13);

    const char* actual_callee_text = json_get_str(candidate, "callee_text");
    REQUIRE(actual_callee_text != nullptr);
    CHECK(std::string(actual_callee_text) == callee_text);
    const char* receiver_hint = json_get_str(candidate, "receiver_hint");
    REQUIRE(receiver_hint != nullptr);
    CHECK(std::string(receiver_hint) == "linked");
    yyjson_val* confidence = yyjson_obj_get(candidate, "confidence");
    REQUIRE(confidence != nullptr);
    REQUIRE(yyjson_is_num(confidence));
    CHECK(yyjson_get_real(confidence) > 0.0);
}

static void require_no_exact_set_edges(const TestDb& db) {
    Connection conn(db.db_path, true);
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(conn.raw(),
        "SELECT COUNT(*) FROM edges WHERE dst_id = ? AND kind = 'calls'",
        -1, &stmt, nullptr) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, db.ids.linked_set);
    REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    CHECK(sqlite3_column_int64(stmt, 0) == 0);
    sqlite3_finalize(stmt);
}

} // namespace

TEST_CASE("callers/context/impact recover approx call sites for overloaded set without exact edges",
          "[unit][approx-callgraph]") {
    auto db = make_approx_db("set-no-exact");
    require_no_exact_set_edges(db);

    auto callers_result = invoke_callers(db, db.ids.linked_set);
    auto callers_doc = json_parse(callers_result);
    REQUIRE(callers_doc);
    CHECK(yyjson_arr_size(require_array_field(callers_doc.root(), "results")) == 0);

    // TODO(approx-callgraph): Keep using Simon's proposed response field names
    // until the implementation settles: candidate_results, candidate_callers,
    // and candidate_impacted.
    auto* candidates = require_array_field(callers_doc.root(), "candidate_results");
    REQUIRE(yyjson_arr_size(candidates) >= 1);
    require_callsite_candidate(find_item_with_string(candidates, "callee_text", "linked.set"), "linked.set");

    auto context_result = invoke_context_for(db, db.ids.linked_set);
    auto context_doc = json_parse(context_result);
    REQUIRE(context_doc);
    INFO(context_result);
    CHECK(yyjson_arr_size(require_array_field(context_doc.root(), "callers")) == 0);
    auto* candidate_callers = require_array_field(context_doc.root(), "candidate_callers");
    REQUIRE(yyjson_arr_size(candidate_callers) >= 1);
    require_callsite_candidate(find_item_with_string(candidate_callers, "callee_text", "linked.set"),
                               "linked.set");

    auto impact_result = invoke_impact_of(db, db.ids.linked_set);
    auto impact_doc = json_parse(impact_result);
    REQUIRE(impact_doc);
    INFO(impact_result);
    CHECK(yyjson_arr_size(require_array_field(impact_doc.root(), "impacted")) == 0);
    auto* candidate_impacted = require_array_field(impact_doc.root(), "candidate_impacted");
    REQUIRE(yyjson_arr_size(candidate_impacted) >= 1);
    require_callsite_candidate(find_item_with_string(candidate_impacted, "callee_text", "linked.set"),
                               "linked.set");
}

TEST_CASE("callers_approx keeps approximate candidates separate from exact edges",
          "[unit][approx-callgraph]") {
    auto db = make_approx_db("set-exact-plus-candidate", true);

    auto result = invoke_callers(db, db.ids.linked_set, "exact_plus_candidates");
    auto doc = json_parse(result);
    REQUIRE(doc);

    auto* results = require_array_field(doc.root(), "results");
    REQUIRE(yyjson_arr_size(results) == 1);
    auto* exact = yyjson_arr_get_first(results);
    REQUIRE(exact != nullptr);
    REQUIRE(json_get_str(exact, "caller_name") != nullptr);
    CHECK(std::string(json_get_str(exact, "caller_name")) == "setExactly");

    auto* candidates = require_array_field(doc.root(), "candidate_results");
    REQUIRE(yyjson_arr_size(candidates) >= 1);
    CHECK(find_item_with_string(candidates, "caller_name", "setExactly") == nullptr);
    require_callsite_candidate(find_item_with_string(candidates, "callee_text", "linked.set"), "linked.set");
}

TEST_CASE("callers_approx uses exact edges for uniquely named symbols",
          "[unit][approx-callgraph]") {
    auto db = make_approx_db("unique-exact");

    auto result = invoke_callers(db, db.ids.flush);
    auto doc = json_parse(result);
    REQUIRE(doc);

    auto* results = require_array_field(doc.root(), "results");
    REQUIRE(yyjson_arr_size(results) == 1);
    auto* exact = yyjson_arr_get_first(results);
    REQUIRE(json_get_str(exact, "caller_name") != nullptr);
    CHECK(std::string(json_get_str(exact, "caller_name")) == "callFlush");

    yyjson_val* candidates = yyjson_obj_get(doc.root(), "candidate_results");
    if (candidates) {
        REQUIRE(yyjson_is_arr(candidates));
        CHECK(yyjson_arr_size(candidates) == 0);
    }
}

TEST_CASE("callers_approx candidate results respect max_bytes budget",
          "[unit][approx-callgraph]") {
    auto db = make_approx_db("candidate-budget", false, 200);

    auto result = invoke_callers_raw(db,
        "{\"node_id\":" + std::to_string(db.ids.linked_set) +
        ",\"limit\":500,\"include_candidates\":true,\"mode\":\"exact_plus_candidates\","
        "\"max_bytes\":1200}");
    INFO("candidate response bytes: " << result.size());
    CHECK(result.size() < 4096);

    auto doc = json_parse(result);
    REQUIRE(doc);
    auto* root = doc.root();
    CHECK(json_get_int(root, "max_bytes", -1) == 1200);
    CHECK(json_get_bool(root, "budget_exceeded", false));
    CHECK(json_get_int(root, "total_candidates", -1) > 20);

    auto* candidates = require_array_field(root, "candidate_results");
    CHECK(yyjson_arr_size(candidates) > 0);
    CHECK(yyjson_arr_size(candidates) <
          static_cast<size_t>(json_get_int(root, "total_candidates", 0)));
}

TEST_CASE("callers_approx candidate confidence rewards matching receiver hints",
          "[unit][approx-callgraph]") {
    auto db = make_approx_db("candidate-confidence");

    auto result = invoke_callers_raw(db,
        "{\"node_id\":" + std::to_string(db.ids.linked_set) +
        ",\"limit\":20,\"include_candidates\":true,\"mode\":\"exact_plus_candidates\"}");
    auto doc = json_parse(result);
    REQUIRE(doc);

    auto* candidates = require_array_field(doc.root(), "candidate_results");
    auto* matching_receiver = find_item_with_string(candidates, "callee_text", "LinkedMap.set");
    auto* unrelated_receiver = find_item_with_string(candidates, "callee_text", "unrelated.set");
    REQUIRE(matching_receiver != nullptr);
    REQUIRE(unrelated_receiver != nullptr);
    CHECK(require_confidence(matching_receiver) > require_confidence(unrelated_receiver));
}

TEST_CASE("callers_approx receiver filter narrows candidates and reports hidden count",
          "[unit][approx-callgraph]") {
    auto db = make_approx_db("receiver-filter");

    auto unfiltered_result = invoke_callers_raw(db,
        "{\"node_id\":" + std::to_string(db.ids.linked_set) +
        ",\"limit\":20,\"include_candidates\":true,\"mode\":\"exact_plus_candidates\"}");
    auto unfiltered_doc = json_parse(unfiltered_result);
    REQUIRE(unfiltered_doc);
    auto* unfiltered_candidates = require_array_field(unfiltered_doc.root(), "candidate_results");
    REQUIRE(yyjson_arr_size(unfiltered_candidates) >= 3);

    auto filtered_result = invoke_callers_raw(db,
        "{\"node_id\":" + std::to_string(db.ids.linked_set) +
        ",\"limit\":20,\"include_candidates\":true,\"mode\":\"exact_plus_candidates\","
        "\"receiver\":\"LinkedMap\"}");
    auto filtered_doc = json_parse(filtered_result);
    REQUIRE(filtered_doc);
    auto* root = filtered_doc.root();
    auto* filtered_candidates = require_array_field(root, "candidate_results");

    CHECK(yyjson_arr_size(filtered_candidates) > 0);
    CHECK(yyjson_arr_size(filtered_candidates) < yyjson_arr_size(unfiltered_candidates));
    CHECK(has_field(root, "total"));
    CHECK(has_field(root, "filtered_hidden"));
    CHECK(json_get_int(root, "filtered_hidden", -1) > 0);

    yyjson_val* item = nullptr;
    yyjson_arr_iter iter;
    yyjson_arr_iter_init(filtered_candidates, &iter);
    while ((item = yyjson_arr_iter_next(&iter))) {
        const char* receiver_hint = json_get_str(item, "receiver_hint");
        REQUIRE(receiver_hint != nullptr);
        CHECK(std::string(receiver_hint) == "LinkedMap");
    }
}

TEST_CASE("callers_approx candidate handles are opt-in",
          "[unit][approx-callgraph]") {
    auto db = make_approx_db("candidate-include-handles");

    auto lean_result = invoke_callers_raw(db,
        "{\"node_id\":" + std::to_string(db.ids.linked_set) +
        ",\"limit\":20,\"include_candidates\":true,\"mode\":\"exact_plus_candidates\"}");
    auto lean_doc = json_parse(lean_result);
    REQUIRE(lean_doc);
    auto* lean_candidates = require_array_field(lean_doc.root(), "candidate_results");
    auto* lean_item = find_item_with_string(lean_candidates, "callee_text", "LinkedMap.set");
    REQUIRE(lean_item != nullptr);
    CHECK_FALSE(has_field(lean_item, "node_id"));
    CHECK_FALSE(has_field(lean_item, "caller_node_id"));
    CHECK_FALSE(has_field(lean_item, "ref_id"));
    CHECK_FALSE(has_field(lean_item, "span"));

    auto handles_result = invoke_callers_raw(db,
        "{\"node_id\":" + std::to_string(db.ids.linked_set) +
        ",\"limit\":20,\"include_candidates\":true,\"mode\":\"exact_plus_candidates\","
        "\"include_handles\":true}");
    auto handles_doc = json_parse(handles_result);
    REQUIRE(handles_doc);
    auto* handles_candidates = require_array_field(handles_doc.root(), "candidate_results");
    auto* handles_item = find_item_with_string(handles_candidates, "callee_text", "LinkedMap.set");
    REQUIRE(handles_item != nullptr);

    bool has_node_handle = has_field(handles_item, "node_id") ||
                           has_field(handles_item, "caller_node_id");
    CHECK(has_node_handle);
    CHECK(has_field(handles_item, "span"));
}

TEST_CASE("callers_approx filters arity mismatches and ranks same-arity patterns",
          "[unit][approx-callgraph]") {
    auto db = make_approx_db("candidate-arity");
    Connection conn(db.db_path);
    conn.exec(std::string("UPDATE nodes SET signature = 'set(key: string, value: number): void' WHERE id = ") +
              std::to_string(db.ids.linked_set));
    conn.exec("UPDATE refs SET arg_count = 2, arg_pattern = 'string,number' WHERE name = 'linked.set'");
    conn.exec("UPDATE refs SET arg_count = 1, arg_pattern = 'string' WHERE name = 'this.set'");
    conn.exec("UPDATE refs SET arg_count = 3, arg_pattern = 'string,number,number' WHERE name = 'LinkedMap.set'");
    conn.exec("UPDATE refs SET arg_count = 1, arg_pattern = 'string' WHERE name = 'unrelated.set'");
    conn.exec("UPDATE refs SET arg_count = 2, arg_pattern = 'string,number' WHERE name = 'OtherMap.set'");
    conn.wal_checkpoint();

    auto result = invoke_callers_raw(db,
        "{\"node_id\":" + std::to_string(db.ids.linked_set) +
        ",\"limit\":20,\"include_candidates\":true,\"mode\":\"exact_plus_candidates\","
        "\"include_handles\":true}");
    auto doc = json_parse(result);
    REQUIRE(doc);
    auto* candidates = require_array_field(doc.root(), "candidate_results");

    REQUIRE(find_item_with_string(candidates, "callee_text", "linked.set") != nullptr);
    CHECK(find_item_with_string(candidates, "callee_text", "this.set") == nullptr);
    CHECK(find_item_with_string(candidates, "callee_text", "unrelated.set") == nullptr);
    CHECK(json_get_int(doc.root(), "arity_filtered", 0) >= 2);
}

TEST_CASE("callers_approx accepts calls omitting optional/defaulted trailing params",
          "[unit][approx-callgraph]") {
    auto db = make_approx_db("candidate-optional-arity");
    Connection conn(db.db_path);
    conn.exec(std::string("UPDATE nodes SET signature = 'set(key: string, value: number, touch: Touch = Touch.None): this' WHERE id = ") +
             std::to_string(db.ids.linked_set));
    conn.exec("UPDATE refs SET arg_count = 2, arg_pattern = 'string,number', receiver_type_hint = 'LinkedMap' WHERE name = 'linked.set'");
    conn.exec("UPDATE refs SET arg_count = 1, arg_pattern = 'string', receiver_type_hint = 'LinkedMap' WHERE name = 'this.set'");
    conn.exec("UPDATE refs SET arg_count = 4, arg_pattern = 'string,number,number,number', receiver_type_hint = 'LinkedMap' WHERE name = 'LinkedMap.set'");
    conn.wal_checkpoint();

    auto result = invoke_callers_raw(db,
        "{\"node_id\":" + std::to_string(db.ids.linked_set) +
        ",\"limit\":20,\"include_candidates\":true,\"mode\":\"exact_plus_candidates\","
        "\"include_handles\":true,\"receiver\":\"LinkedMap\"}");
    auto doc = json_parse(result);
    REQUIRE(doc);
    auto* candidates = require_array_field(doc.root(), "candidate_results");

    CHECK(find_item_with_string(candidates, "callee_text", "linked.set") != nullptr);
    CHECK(find_item_with_string(candidates, "callee_text", "this.set") == nullptr);
    CHECK(find_item_with_string(candidates, "callee_text", "LinkedMap.set") == nullptr);
    CHECK(json_get_int(doc.root(), "candidate_eligible", 0) > 0);
}

TEST_CASE("callers_approx prefers same-file local receiver type hints",
          "[unit][approx-callgraph]") {
    auto db = make_approx_db("candidate-local-receiver-type");
    Connection conn(db.db_path);
    conn.exec("UPDATE refs SET receiver_type_hint = 'LinkedMap' WHERE name = 'linked.set'");
    conn.wal_checkpoint();

    auto result = invoke_callers_raw(db,
        "{\"node_id\":" + std::to_string(db.ids.linked_set) +
        ",\"limit\":20,\"include_candidates\":true,\"mode\":\"exact_plus_candidates\","
        "\"include_handles\":true}");
    auto doc = json_parse(result);
    REQUIRE(doc);
    auto* candidates = require_array_field(doc.root(), "candidate_results");
    auto* typed_receiver = find_item_with_string(candidates, "callee_text", "linked.set");
    auto* unrelated_receiver = find_item_with_string(candidates, "callee_text", "unrelated.set");
    REQUIRE(typed_receiver != nullptr);
    REQUIRE(unrelated_receiver != nullptr);
    REQUIRE(json_get_str(typed_receiver, "receiver_type_hint") != nullptr);
    CHECK(std::string(json_get_str(typed_receiver, "receiver_type_hint")) == "LinkedMap");
    CHECK(require_confidence(typed_receiver) > require_confidence(unrelated_receiver));
    REQUIRE(json_get_str(typed_receiver, "heuristic") != nullptr);
    CHECK(std::string(json_get_str(typed_receiver, "heuristic")).find("receiver_type") != std::string::npos);
}

TEST_CASE("callers_approx infers target owner from lexical span when containment is flat",
          "[unit][approx-callgraph]") {
    auto db = make_approx_db("candidate-lexical-owner");
    Connection conn(db.db_path);
    conn.exec(std::string("UPDATE nodes SET qualname = 'set' WHERE id = ") +
             std::to_string(db.ids.linked_set));
    conn.exec(std::string("DELETE FROM edges WHERE kind = 'contains' AND dst_id = ") +
             std::to_string(db.ids.linked_set));
    conn.exec("UPDATE refs SET receiver_type_hint = 'LinkedMap' WHERE name = 'linked.set'");
    conn.wal_checkpoint();

    auto result = invoke_callers_raw(db,
        "{\"node_id\":" + std::to_string(db.ids.linked_set) +
        ",\"limit\":20,\"include_candidates\":true,\"mode\":\"exact_plus_candidates\","
        "\"include_handles\":true,\"receiver\":\"LinkedMap\"}");
    auto doc = json_parse(result);
    REQUIRE(doc);
    auto* candidates = require_array_field(doc.root(), "candidate_results");
    auto* typed_receiver = find_item_with_string(candidates, "callee_text", "linked.set");
    REQUIRE(typed_receiver != nullptr);
    REQUIRE(json_get_str(typed_receiver, "heuristic") != nullptr);
    CHECK(std::string(json_get_str(typed_receiver, "heuristic")).find("receiver_type") != std::string::npos);
    CHECK(json_get_int(doc.root(), "candidate_eligible", 0) > 0);
}

TEST_CASE("callers_approx lean mode returns summary buckets and top candidates",
          "[unit][approx-callgraph]") {
    auto db = make_approx_db("candidate-lean-mode", false, 40);

    auto verbose_result = invoke_callers_raw(db,
        "{\"node_id\":" + std::to_string(db.ids.linked_set) +
        ",\"limit\":100,\"include_candidates\":true,\"mode\":\"exact_plus_candidates\","
        "\"include_handles\":true}");
    auto lean_result = invoke_callers_raw(db,
        "{\"node_id\":" + std::to_string(db.ids.linked_set) +
        ",\"response_mode\":\"lean\",\"top_n\":3,\"include_candidates\":true,"
        "\"mode\":\"exact_plus_candidates\"}");

    INFO("verbose bytes: " << verbose_result.size() << " lean bytes: " << lean_result.size());
    CHECK(lean_result.size() < verbose_result.size());

    auto doc = json_parse(lean_result);
    REQUIRE(doc);
    auto* root = doc.root();
    REQUIRE(json_get_str(root, "response_mode") != nullptr);
    CHECK(std::string(json_get_str(root, "response_mode")) == "lean");
    CHECK(json_get_int(root, "exact_total", -1) == 0);
    CHECK(json_get_int(root, "candidate_total", -1) > 3);
    CHECK(json_get_int(root, "candidate_eligible", -1) > 0);
    CHECK(json_get_int(root, "candidate_max_bytes", -1) == 4096);

    auto* candidates = require_array_field(root, "candidate_results");
    CHECK(yyjson_arr_size(candidates) <= 3);
    auto* first = yyjson_arr_get_first(candidates);
    REQUIRE(first != nullptr);
    CHECK(has_field(first, "file"));
    CHECK(has_field(first, "line"));
    CHECK_FALSE(has_field(first, "span"));
    CHECK_FALSE(has_field(first, "evidence"));
    CHECK_FALSE(has_field(first, "why"));

    auto* buckets = yyjson_obj_get(root, "candidate_buckets");
    REQUIRE(buckets != nullptr);
    REQUIRE(yyjson_obj_get(buckets, "arg_count") != nullptr);
    REQUIRE(yyjson_obj_get(buckets, "heuristic") != nullptr);
    REQUIRE(yyjson_obj_get(buckets, "file") != nullptr);
}

TEST_CASE("callers_approx resolves receiver type through include edges conservatively",
          "[unit][approx-callgraph]") {
    auto db = make_approx_db("candidate-cross-file-receiver-type");
    Connection conn(db.db_path);
    int64_t duplicate_file = insert_file(conn, "src/duplicate_linked_map.ts");
    insert_symbol(conn, duplicate_file, "class", "LinkedMap", "Duplicate.LinkedMap", 1, 10);
    int64_t caller_file_node = insert_file_node(conn, "src/use_map.ts");
    int64_t map_file_node = insert_file_node(conn, "src/linked_map.ts");
    insert_edge(conn, caller_file_node, map_file_node, "includes", 0.7);
    conn.exec("UPDATE refs SET receiver_type_hint = 'LinkedMap' WHERE name = 'linked.set'");
    conn.wal_checkpoint();

    auto result = invoke_callers_raw(db,
        "{\"node_id\":" + std::to_string(db.ids.linked_set) +
        ",\"limit\":20,\"include_candidates\":true,\"mode\":\"exact_plus_candidates\","
        "\"include_handles\":true}");
    auto doc = json_parse(result);
    REQUIRE(doc);
    auto* candidates = require_array_field(doc.root(), "candidate_results");
    auto* typed_receiver = find_item_with_string(candidates, "callee_text", "linked.set");
    auto* unrelated_receiver = find_item_with_string(candidates, "callee_text", "unrelated.set");
    REQUIRE(typed_receiver != nullptr);
    REQUIRE(unrelated_receiver != nullptr);
    CHECK(json_get_bool(typed_receiver, "receiver_type_resolved", false));
    REQUIRE(json_get_str(typed_receiver, "receiver_type_resolution_source") != nullptr);
    CHECK(std::string(json_get_str(typed_receiver, "receiver_type_resolution_source")) == "include");
    REQUIRE(json_get_str(typed_receiver, "heuristic") != nullptr);
    CHECK(std::string(json_get_str(typed_receiver, "heuristic")).find("cross_file_receiver_type") != std::string::npos);
    CHECK(require_confidence(typed_receiver) > require_confidence(unrelated_receiver));
}

TEST_CASE("callers_approx does not boost ambiguous global receiver type hints",
          "[unit][approx-callgraph]") {
    auto db = make_approx_db("candidate-ambiguous-receiver-type");
    Connection conn(db.db_path);
    int64_t duplicate_file = insert_file(conn, "src/ambiguous_linked_map.ts");
    insert_symbol(conn, duplicate_file, "class", "LinkedMap", "Ambiguous.LinkedMap", 1, 10);
    conn.exec("UPDATE refs SET receiver_type_hint = 'LinkedMap' WHERE name = 'linked.set'");
    conn.wal_checkpoint();

    auto result = invoke_callers_raw(db,
        "{\"node_id\":" + std::to_string(db.ids.linked_set) +
        ",\"limit\":20,\"include_candidates\":true,\"mode\":\"exact_plus_candidates\","
        "\"include_handles\":true}");
    auto doc = json_parse(result);
    REQUIRE(doc);
    auto* candidates = require_array_field(doc.root(), "candidate_results");
    auto* typed_receiver = find_item_with_string(candidates, "callee_text", "linked.set");
    REQUIRE(typed_receiver != nullptr);
    CHECK(json_get_bool(typed_receiver, "receiver_type_ambiguous", false));
    REQUIRE(json_get_str(typed_receiver, "heuristic") != nullptr);
    CHECK(std::string(json_get_str(typed_receiver, "heuristic")).find("cross_file_receiver_type") == std::string::npos);
}
