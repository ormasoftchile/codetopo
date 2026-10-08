#include <catch2/catch_test_macros.hpp>
#include "core/arena_pool.h"
#include "cli/cmd_index.h"
#include "db/connection.h"
#include "db/schema.h"
#include "db/workspace.h"
#include "index/call_binding.h"
#include "index/extractor.h"
#include "index/parser.h"
#include "index/persister.h"
#include "util/hash.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <unordered_set>

using namespace codetopo;

namespace codetopo {
void set_thread_arena(Arena* arena);
void register_arena_allocator();
}

namespace {

ExtractionResult extract_cpp(const std::string& source, int limit = 5000,
                             const std::string& path = "src/query_host.cpp") {
    register_arena_allocator();
    ArenaPool pool(1, 32 * 1024 * 1024);
    ArenaLease lease(pool);
    set_thread_arena(lease.get());
    Parser parser;
    REQUIRE(parser.set_language("cpp"));
    TreeGuard tree(parser.parse(source));
    REQUIRE(tree.tree);
    Extractor extractor(limit, 100);
    return extractor.extract(tree.tree, source, "cpp", path);
}

std::vector<const ExtractedSymbol*> symbols_named(const ExtractionResult& result,
                                                 const std::string& name) {
    std::vector<const ExtractedSymbol*> symbols;
    for (const auto& symbol : result.symbols)
        if (symbol.name == name) symbols.push_back(&symbol);
    return symbols;
}

const ExtractedRef& call_named(const ExtractionResult& result, const std::string& name) {
    auto found = std::find_if(result.refs.begin(), result.refs.end(), [&](const auto& ref) {
        return ref.kind == "call" && ref.name == name;
    });
    REQUIRE(found != result.refs.end());
    return *found;
}

int64_t scalar(Connection& conn, const std::string& sql) {
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(conn.raw(), sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    auto value = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return value;
}

void persist_cpp(Connection& conn, const std::string& path, const std::string& source) {
    ScannedFile file{.absolute_path = std::filesystem::current_path() / path,
                     .relative_path = path, .language = "cpp",
                     .size_bytes = static_cast<int64_t>(source.size()), .mtime_ns = 1};
    Persister persister(conn);
    REQUIRE(persister.persist_file(file, extract_cpp(source, 5000, path), "fixture", "ok"));
}

int capture_call_queries(unsigned, void* context, void* statement, void*) {
    auto* sql = sqlite3_sql(static_cast<sqlite3_stmt*>(statement));
    if (!sql) return 0;
    std::string query(sql);
    if (query.find("__ct_call_owners") != std::string::npos ||
        query.find("__ct_want") != std::string::npos ||
        query.find("LEFT JOIN nodes cn") != std::string::npos ||
        query.find("workspace_lookup_names") != std::string::npos ||
        query.find("__ct_refresh_") != std::string::npos)
        static_cast<std::set<std::string>*>(context)->insert(query);
    return 0;
}

void check_call_plans(Connection& conn, const std::set<std::string>& queries) {
    REQUIRE_FALSE(queries.empty());
    conn.exec("CREATE TEMP TABLE IF NOT EXISTS __ct_changed(path TEXT PRIMARY KEY)");
    conn.exec("CREATE TEMP TABLE IF NOT EXISTS __ct_defnames(name TEXT PRIMARY KEY)");
    conn.exec("CREATE TEMP TABLE IF NOT EXISTS __ct_kinds(kind TEXT PRIMARY KEY)");
    conn.exec("CREATE TEMP TABLE IF NOT EXISTS __ct_affected(id INTEGER PRIMARY KEY)");
    conn.exec("CREATE TEMP TABLE IF NOT EXISTS __ct_want(name TEXT PRIMARY KEY)");
    for (const auto& query : queries) {
        if (!query.starts_with("SELECT ") && !query.starts_with("UPDATE ") &&
            !query.starts_with("INSERT ")) continue;
        CAPTURE(query);
        sqlite3_stmt* plan = nullptr;
        auto sql = "EXPLAIN QUERY PLAN " + query;
        REQUIRE(sqlite3_prepare_v2(conn.raw(), sql.c_str(), -1, &plan, nullptr) == SQLITE_OK);
        while (sqlite3_step(plan) == SQLITE_ROW) {
            auto* detail = reinterpret_cast<const char*>(sqlite3_column_text(plan, 3));
            REQUIRE(detail);
            std::string description(detail);
            CAPTURE(description);
            CHECK(description.find("SCAN nodes") == std::string::npos);
            CHECK(description.find("SCAN cn") == std::string::npos);
            if (query.find("nodes n ") != std::string::npos)
                CHECK(description.find("SCAN n") == std::string::npos);
        }
        sqlite3_finalize(plan);
    }
}

} // namespace

TEST_CASE("C++ definitions are emitted once and retain legacy stable handles",
          "[unit][cpp_binding][extractor]") {
    auto result = extract_cpp(R"(
namespace execution {
void ConcreteQueryHost::submit(int query, int context, int options) {
    auto handle = prepare();
    dispatch(handle);
}
}
)");
    auto submit = symbols_named(result, "submit");
    REQUIRE(submit.size() == 1);
    CHECK(submit[0]->is_definition);
    CHECK(submit[0]->qualname == "execution::ConcreteQueryHost::submit");
    CHECK(submit[0]->stable_key == "src/query_host.cpp::function::submit");
    CHECK(submit[0]->end_line == 6);
    for (auto name : {"prepare", "dispatch"}) {
        const auto& ref = call_named(result, name);
        REQUIRE(ref.containing_symbol_index >= 0);
        CHECK(result.symbols[ref.containing_symbol_index].qualname == submit[0]->qualname);
    }
}

TEST_CASE("C++ declarations, overloads, pointer declarators and special names remain distinct",
          "[unit][cpp_binding][extractor]") {
    auto result = extract_cpp(R"(
namespace execution {
struct Host {
    Host();
    ~Host();
    int operator()(int value);
    bool operator<(const Host& other) const;
    void submit(int value);
    void submit(int value, int options);
};
void Host::submit(int value) {}
void Host::submit(int value, int options) {}
Host::Host() {}
Host::~Host() {}
int Host::operator()(int value) { return value; }
bool Host::operator<(const Host& other) const { return false; }
int* pointer_return(int value) { return nullptr; }
int (*callback)(int);
void takes_callback(void (*fn)(int));
using Callback = void (*)(int);
}
)");
    auto submit = symbols_named(result, "submit");
    REQUIRE(submit.size() == 4);
    CHECK(std::count_if(submit.begin(), submit.end(), [](auto* sym) { return sym->is_definition; }) == 2);
    std::unordered_set<std::string> handles;
    for (auto* symbol : submit) {
        CHECK(symbol->kind == "function");
        CHECK(symbol->qualname == "execution::Host::submit");
        CHECK(handles.insert(symbol->stable_key).second);
    }
    for (auto name : {"Host", "~Host", "operator()", "operator<"}) {
        auto symbols = symbols_named(result, name);
        auto count = std::count_if(symbols.begin(), symbols.end(), [](auto* symbol) {
            return symbol->kind == "function";
        });
        CHECK(count == 2);
    }
    auto pointer_return = symbols_named(result, "pointer_return");
    REQUIRE(pointer_return.size() == 1);
    CHECK(pointer_return[0]->is_definition);
    auto callback = symbols_named(result, "callback");
    REQUIRE(callback.size() == 1);
    CHECK(callback[0]->kind == "variable");
    CHECK(symbols_named(result, "fn").empty());
    CHECK(symbols_named(result, "takes_callback").size() == 1);
}

TEST_CASE("C++ call refs retain receivers, templates, arity and lexical type ownership",
          "[unit][cpp_binding][extractor]") {
    auto result = extract_cpp(R"(
namespace engine {
struct Scheduler { void submit(int value); };
struct Host {
    void run(Scheduler& scheduler) {
        scheduler.submit(1);
        this->submit(1, 2, 3);
        Scheduler local;
        local.submit(2);
        { Other local; local.submit(3); }
        local.submit(4);
        carrier::submit<Task>(1, 2, 3);
    }
    Scheduler later;
    void submit(int query, int context, int options);
};
}
)");
    const auto& scheduler = call_named(result, "scheduler.submit");
    CHECK(scheduler.arg_count == 1);
    CHECK(scheduler.receiver_type_hint == "Scheduler");
    CHECK(call_named(result, "this->submit").receiver_type_hint == "engine::Host");
    CHECK(call_named(result, "carrier::submit<Task>").arg_count == 3);
    std::vector<std::string> local_types;
    for (const auto& ref : result.refs)
        if (ref.kind == "call" && ref.name == "local.submit") local_types.push_back(ref.receiver_type_hint);
    CHECK(local_types == std::vector<std::string>{"Scheduler", "Other", "Scheduler"});
    for (const auto& ref : result.refs) {
        if (ref.kind != "call") continue;
        REQUIRE(ref.containing_symbol_index >= 0);
        CHECK(result.symbols[ref.containing_symbol_index].qualname == "engine::Host::run");
    }
}

TEST_CASE("Unknown C++ local types do not borrow unrelated earlier declarations",
          "[unit][cpp_binding][extractor]") {
    auto result = extract_cpp(R"(
void earlier() { Scheduler scheduler; }
void current() { auto scheduler = make_scheduler(); scheduler.submit(1); }
)");
    CHECK(call_named(result, "scheduler.submit").receiver_type_hint.empty());
}

TEST_CASE("Truncated C++ extraction does not change a previous symbol's definition flag",
          "[unit][cpp_binding][extractor]") {
    auto result = extract_cpp("int first() {} int second();", 1);
    REQUIRE(result.symbols.size() == 1);
    CHECK(result.truncated);
    CHECK(result.symbols[0].is_definition);
}

TEST_CASE("Call binding rejects incompatible submit targets and preserves unresolved overloads",
          "[unit][cpp_binding]") {
    using namespace call_binding;
    std::unordered_set<std::string> classes{"engine::QueryHost", "engine::Scheduler"};
    auto host = target("function", "submit", "engine::QueryHost::submit",
                       "void submit(int query, int context, int options)", "cpp");
    auto scheduler = target("function", "submit", "engine::Scheduler::submit",
                            "void submit(Task task)", "cpp");
    auto carrier = target("function", "submit", "carrier::submit",
                          "void submit(Backend backend, Task* task, Args&&... args)", "cpp");
    CHECK_FALSE(compatible(host, "scheduler.submit", "Scheduler", 1, "cpp", classes));
    CHECK_FALSE(compatible(host, "submit<Task>", "", 3, "cpp", classes));
    CHECK_FALSE(compatible(host, "unknown.submit", "", 3, "cpp", classes));
    CHECK(compatible(scheduler, "scheduler.submit", "Scheduler", 1, "cpp", classes));
    CHECK(compatible(host, "this->submit", "engine::QueryHost", 3, "cpp", classes));
    CHECK(compatible(host, "submit", "", 3, "cpp", classes, "engine::QueryHost::run"));
    CHECK(compatible(carrier, "carrier::submit<Task>", "", 3, "cpp", classes));
    CHECK_FALSE(compatible(carrier, "unrelated::submit", "", 3, "cpp", classes));
    auto global = target("function", "submit", "submit", "void submit(int a,int b,int c)", "cpp");
    CHECK(compatible(global, "::submit", "", 3, "cpp", classes));
    CHECK_FALSE(compatible(host, "::submit", "", 3, "cpp", classes));
    std::unordered_set<std::string> namespaces{"carrier"};
    CHECK(compatible(carrier, "submit", "", 3, "cpp", classes, "carrier::Caller::run", namespaces));
    CHECK_FALSE(compatible(carrier, "submit", "", 3, "cpp", classes, "unrelated::Caller::run", namespaces));
    CHECK_FALSE(compatible(host, "submit", "", 3, "cpp", {}, "unrelated::run"));
    CHECK(bare_name("carrier::submit<engine::Task>") == "submit");
    CHECK(bare_name("Host::operator<") == "operator<");
}

TEST_CASE("Shared parameter parsing handles defaults, callback types, variadics and operators",
          "[unit][cpp_binding]") {
    using namespace call_binding;
    auto params = parameters(R"cpp(void submit(int n, Callback cb = [](int a, int b) { return a + b; }, const char* text = ")"))cpp",
                             "submit");
    REQUIRE(params);
    REQUIRE(params->size() == 3);
    auto arity = signature_arity(R"cpp(void submit(int n, Callback cb = [](int a, int b) { return a + b; }, const char* text = ")"))cpp",
                                 "submit");
    CHECK(arity.minimum == 1);
    CHECK(arity.maximum == 3);
    auto variadic = signature_arity("void submit(Backend backend, Task* task, Args&&... args)", "submit");
    CHECK(variadic.minimum == 2);
    CHECK(variadic.maximum == -1);
    auto op = signature_arity("int Host::operator()(int n)", "operator()");
    CHECK(op.minimum == 1);
    CHECK(op.maximum == 1);
    CHECK(signature_arity("void no_args(void)", "no_args").maximum == 0);
    CHECK_FALSE(signature_arity("void incomplete(int", "incomplete").known);
    CHECK_FALSE(signature_arity("void compare(bool ok = value < limit, int n = 1)", "compare").known);
}

TEST_CASE("C++ call arity excludes comments and remains unknown for expanded packs",
          "[unit][cpp_binding][extractor]") {
    auto result = extract_cpp(R"(
template<class... Args> void invoke(Host& host, Args&&... args) {
    host.submit(1 /* first */, 2, 3 /* last */);
    submit(args...);
}
)");
    CHECK(call_named(result, "host.submit").arg_count == 3);
    CHECK(call_named(result, "submit").arg_count == -1);
}

TEST_CASE("Persisted C++ calls bind by target identity rather than common names",
          "[unit][cpp_binding][resolve_refs]") {
    Connection conn(":memory:");
    REQUIRE(schema::ensure_schema(conn) == 0);
    persist_cpp(conn, "src/targets.cpp", R"(
namespace engine {
struct QueryHost { void submit(int query, int context, int options) {} };
struct Scheduler { void submit(int task) {} };
}
namespace other { struct QueryHost { void submit(int query, int context, int options) {} }; }
namespace carrier { template<class... Args> void submit(int backend, int task, Args&&... args) {} }
void choose(int value) {}
void choose(double value) {}
)");
    persist_cpp(conn, "tests/callers.cpp", R"(
void scheduler_call(engine::Scheduler& scheduler) { auto handle = prepare(); scheduler.submit(1); }
void host_call(engine::QueryHost& host) { host.submit(1, 2, 3); }
void carrier_call() { carrier::submit<int>(1, 2, 3, 4); }
void unknown_call(auto& unknown) { unknown.submit(1, 2, 3); }
void ambiguous_call() { choose(1); }
void recursive_call() { recursive_call(); }
)");
    Persister persister(conn);
    auto resolved = persister.resolve_references();
    CHECK(resolved.first >= 4);
    CHECK(resolved.second >= 6);
    CHECK(scalar(conn, "SELECT COUNT(*) FROM refs WHERE kind='call' AND resolved_node_id IS NOT NULL") == 4);
    CHECK(scalar(conn, "SELECT COUNT(*) FROM edges WHERE kind='calls'") == 6);
    CHECK(scalar(conn,
        "SELECT COUNT(*) FROM refs r JOIN nodes n ON n.id=r.resolved_node_id "
        "WHERE r.name='scheduler.submit' AND n.qualname='engine::Scheduler::submit'") == 1);
    CHECK(scalar(conn,
        "SELECT COUNT(*) FROM refs r JOIN nodes n ON n.id=r.resolved_node_id "
        "WHERE r.name='host.submit' AND n.qualname='engine::QueryHost::submit'") == 1);
    CHECK(scalar(conn,
        "SELECT COUNT(*) FROM refs r JOIN nodes n ON n.id=r.resolved_node_id "
        "WHERE r.name='carrier::submit<int>' AND n.qualname='carrier::submit'") == 1);
    CHECK(scalar(conn,
        "SELECT COUNT(*) FROM refs WHERE name='unknown.submit' AND resolved_node_id IS NULL") == 1);
    CHECK(scalar(conn,
        "SELECT COUNT(*) FROM refs WHERE name='choose' AND resolved_node_id IS NULL") == 1);
    CHECK(scalar(conn,
        "SELECT COUNT(*) FROM edges e JOIN nodes n ON n.id=e.dst_id "
        "WHERE n.name='choose' AND e.evidence='call-candidate' AND e.confidence<=0.60") == 2);
    CHECK(scalar(conn,
        "SELECT COUNT(*) FROM edges e JOIN nodes src ON src.id=e.src_id JOIN nodes dst ON dst.id=e.dst_id "
        "WHERE src.name IN ('scheduler_call','carrier_call','unknown_call') "
        "AND dst.qualname LIKE '%QueryHost::submit'") == 0);
    CHECK(scalar(conn,
        "SELECT COUNT(*) FROM edges e JOIN nodes n ON n.id=e.src_id "
        "WHERE n.name='recursive_call' AND e.src_id=e.dst_id AND e.kind='calls'") == 1);
    CHECK(persister.resolve_references().second == 0);
}

TEST_CASE("Incremental binding loads containing types through bounded owner-name probes",
          "[unit][cpp_binding][resolve_refs]") {
    Connection conn(":memory:");
    REQUIRE(schema::ensure_schema(conn) == 0);
    persist_cpp(conn, "src/host.cpp",
        "namespace engine { struct QueryHost { void submit(int a,int b,int c) {} }; }\n");
    persist_cpp(conn, "tests/caller.cpp", "void caller() { submit(1,2,3); }\n");
    std::set<std::string> queries;
    sqlite3_trace_v2(conn.raw(), SQLITE_TRACE_STMT, capture_call_queries, &queries);
    Persister persister(conn);
    auto result = persister.resolve_references(true, {"tests/caller.cpp"});
    sqlite3_trace_v2(conn.raw(), 0, nullptr, nullptr);
    CHECK(result.first == 0);
    CHECK(result.second == 0);
    check_call_plans(conn, queries);
    CHECK(scalar(conn, "SELECT COUNT(*) FROM refs WHERE kind='call' AND resolved_node_id IS NULL") == 1);
}

TEST_CASE("Workspace linking rejects unrelated members and never chooses a tied overload",
          "[unit][cpp_binding][workspace]") {
    namespace fs = std::filesystem;
    auto base = fs::current_path() / "build" / "test_cpp_workspace_binding";
    fs::remove_all(base);
    auto primary = base / "primary";
    auto extra = base / "extra";
    fs::create_directories(primary / ".codetopo");
    fs::create_directories(extra / ".codetopo");
    auto primary_db = primary / ".codetopo" / "index.sqlite";
    {
        Connection conn(primary_db);
        REQUIRE(schema::ensure_schema(conn) == 0);
        persist_cpp(conn, "src/primary.cpp", R"(
namespace primary { struct QueryHost { void submit(int a,int b,int c) {} }; }
void host_call(shared::QueryHost& host) { host.submit(1,2,3); }
void scheduler_call(Scheduler& scheduler) { scheduler.submit(1); }
void carrier_call() { submit<Task>(1,2,3); }
void ambiguous_call() { choose(1); }
)");
        Persister persister(conn);
        persister.write_metadata(fs::canonical(primary).string());
    }
    {
        Connection conn(extra / ".codetopo" / "index.sqlite");
        REQUIRE(schema::ensure_schema(conn) == 0);
        persist_cpp(conn, "src/extra.cpp", R"(
namespace shared { struct QueryHost { void submit(int a,int b,int c) {} }; }
template<class... Args> void submit(int backend,int task,Args&&... args) {}
void choose(int value) {}
void choose(double value) {}
)");
        Persister persister(conn);
        persister.write_metadata(fs::canonical(extra).string());
    }
    int64_t primary_id;
    {
        Connection conn(primary_db, true);
        primary_id = scalar(conn, "SELECT id FROM nodes WHERE name='host_call'");
    }
    {
        WorkspaceDB workspace(primary_db.string());
        auto added = workspace.add_root(extra.string(), Config{});
        REQUIRE(added.root_id == 1);
        Connection conn(primary_db, true);
        CHECK(scalar(conn, "SELECT COUNT(*) FROM nodes WHERE id=" + std::to_string(primary_id) + " AND name='host_call'") == 1);
        CHECK(scalar(conn,
            "SELECT COUNT(*) FROM refs r JOIN nodes n ON n.id=r.resolved_node_id "
            "WHERE r.name='host.submit' AND n.qualname='shared::QueryHost::submit'") == 1);
        CHECK(scalar(conn,
            "SELECT COUNT(*) FROM refs r JOIN nodes n ON n.id=r.resolved_node_id "
            "WHERE r.name='submit<Task>' AND n.qualname='submit'") == 1);
        CHECK(scalar(conn,
            "SELECT COUNT(*) FROM refs WHERE name='choose' AND resolved_node_id IS NULL") == 1);
        CHECK(scalar(conn,
            "SELECT COUNT(*) FROM edges WHERE evidence='workspace_call_candidate'") == 2);
        CHECK(scalar(conn,
            "SELECT COUNT(*) FROM edges e JOIN nodes src ON src.id=e.src_id JOIN nodes dst ON dst.id=e.dst_id "
            "WHERE src.name IN ('scheduler_call','carrier_call') AND dst.qualname LIKE '%QueryHost::submit'") == 0);
        CHECK(scalar(conn, "SELECT COUNT(*) FROM files WHERE root_id IS NULL") == 1);
        CHECK(conn.foreign_key_check() == 0);
    }
    fs::remove_all(base);
}
