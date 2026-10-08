#include <catch2/catch_test_macros.hpp>
#include "db/connection.h"
#include "db/schema.h"
#include "db/queries.h"
#include "db/workspace.h"
#include "db/fts.h"
#include "mcp/tools.h"
#include "util/git.h"
#include "util/hash.h"
#include "util/json.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace codetopo;
namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
constexpr const char* redirect = " >NUL 2>NUL";
#else
constexpr const char* redirect = " >/dev/null 2>/dev/null";
#endif

std::string lower_path(std::string value) {
#ifdef _WIN32
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return value;
}

std::string json_args(const std::string& root, const std::string& since) {
    JsonMutDoc doc;
    auto* args = doc.new_obj();
    doc.set_root(args);
    yyjson_mut_obj_add_strcpy(doc.doc, args, "repo_root", root.c_str());
    yyjson_mut_obj_add_strcpy(doc.doc, args, "since", since.c_str());
    yyjson_mut_obj_add_int(doc.doc, args, "depth", 2);
    return doc.to_string();
}

struct Fixture {
    fs::path base = fs::current_path() / "build" / "extra-root-resolution-fixture";
    fs::path primary = base / "Primary";
    fs::path extra = base / "Extra Root";
    fs::path database = primary / ".codetopo" / "index.sqlite";
    std::string stored_extra;
    std::string base_commit;
    std::string head_commit;
    static constexpr const char* target_key =
        "1:src/execution/query_host.cpp::function::submit";
    static constexpr const char* target_content = "int submit() { return 2; }\n";
    static constexpr const char* type_content = "struct query_host { int value; int extra; };\n";

    Fixture() {
        REQUIRE_FALSE(fs::exists(base));
        fs::create_directories(primary / ".codetopo");
        fs::create_directories(extra / "src" / "execution");
        fs::create_directories(extra / "include" / "skiff");
        fs::create_directories(extra / "docs");
        git("init");
        git("config user.name Fixture");
        git("config user.email fixture@example.invalid");
        git("config core.autocrlf false");
        fs::create_directories(base / "empty-hooks");
        git("config core.hooksPath \"" + (base / "empty-hooks").string() + "\"");
        write("src/execution/query_host.cpp", "int submit() { return 1; }\n");
        write("include/skiff/query_host.h", "struct query_host { int value; };\n");
        write("docs/revision.md", "before\n");
        git("add .");
        git("commit -m base");
        base_commit = get_git_head(extra.string());
        write("src/execution/query_host.cpp", target_content);
        write("include/skiff/query_host.h", type_content);
        write("docs/revision.md", "after\n");
        git("add .");
        git("commit -m change");
        head_commit = get_git_head(extra.string());
        REQUIRE_FALSE(base_commit.empty());
        REQUIRE_FALSE(head_commit.empty());
        stored_extra = lower_path(fs::canonical(extra).string());
        Connection conn(database);
        REQUIRE(schema::ensure_schema(conn) == 0);
        schema::set_kv(conn, "repo_root", fs::canonical(primary).string());
        schema::set_kv(conn, "git_head", std::string(40, 'a'));
        schema::set_kv(conn, workspace_root_metadata_key(1, "git_head"), head_commit);
        sqlite3_stmt* root_stmt = nullptr;
        REQUIRE(sqlite3_prepare_v2(conn.raw(),
            "INSERT INTO roots(id,path,added_at) VALUES(1,?,datetime('now'))",
            -1, &root_stmt, nullptr) == SQLITE_OK);
        sqlite3_bind_text(root_stmt, 1, stored_extra.c_str(), -1, SQLITE_TRANSIENT);
        REQUIRE(sqlite3_step(root_stmt) == SQLITE_DONE);
        sqlite3_finalize(root_stmt);
        add_file(conn, 1, "src/execution/query_host.cpp", 0, "int primary_submit();\n");
        add_file(conn, 2, stored_extra + "/src/execution/query_host.cpp", 1, target_content);
        add_file(conn, 3, stored_extra + "/include/skiff/query_host.h", 1, type_content);
        add_file(conn, 4, stored_extra + "/src/execution/runner.cpp", 1, "int runner();\n");
        conn.exec(
            "INSERT INTO nodes(id,node_type,file_id,kind,name,qualname,start_line,end_line,stable_key) VALUES"
            "(11,'symbol',1,'function','submit','primary::submit',1,1,'primary::submit'),"
            "(21,'symbol',2,'function','submit','skiff::submit',1,1,"
            "'1:src/execution/query_host.cpp::function::submit'),"
            "(31,'symbol',3,'struct','query_host','skiff::query_host',1,1,"
            "'1:include/skiff/query_host.h::struct::query_host'),"
            "(41,'symbol',4,'function','runner','skiff::runner',1,1,"
            "'1:src/execution/runner.cpp::function::runner')");
        conn.exec(
            "INSERT INTO edges(src_id,dst_id,kind,confidence,evidence,source) VALUES"
            "(41,21,'calls',0.75,'name-match','static'),"
            "(11,21,'calls',0.6,'name-match','static')");
        fts::rebuild(conn);
    }

    ~Fixture() {
        std::error_code ec;
        for (fs::recursive_directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
            fs::permissions(it->path(), fs::perms::owner_write, fs::perm_options::add, ec);
            if (ec) ec.clear();
        }
        fs::remove_all(base, ec);
    }

    void git(const std::string& args) {
        auto command = "git -C \"" + extra.string() + "\" " + args + redirect;
        INFO(command);
        REQUIRE(std::system(command.c_str()) == 0);
    }

    void write(const fs::path& relative, const std::string& text) {
        auto file = extra / relative;
        fs::create_directories(file.parent_path());
        std::ofstream output(file, std::ios::binary);
        output << text;
        output.close();
        REQUIRE(output.good());
    }

    static void add_file(Connection& conn, int64_t id, const std::string& path,
                         int64_t root, const std::string& content) {
        sqlite3_stmt* stmt = nullptr;
        REQUIRE(sqlite3_prepare_v2(conn.raw(),
            "INSERT INTO files(id,path,language,size_bytes,mtime_ns,content_hash,parse_status,root_id) "
            "VALUES(?,?,'cpp',?,1,?,'ok',?)", -1, &stmt, nullptr) == SQLITE_OK);
        sqlite3_bind_int64(stmt, 1, id);
        sqlite3_bind_text(stmt, 2, path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, content.size());
        auto hash = hash_string(content);
        sqlite3_bind_text(stmt, 4, hash.c_str(), -1, SQLITE_TRANSIENT);
        if (root == 0) sqlite3_bind_null(stmt, 5);
        else sqlite3_bind_int64(stmt, 5, root);
        REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
    }

    JsonDoc impact(const std::string& path, const char* stable_key = nullptr) {
        Connection conn(database, true);
        QueryCache cache(conn);
        JsonMutDoc doc;
        auto* args = doc.new_obj();
        doc.set_root(args);
        if (stable_key) yyjson_mut_obj_add_str(doc.doc, args, "stable_key", stable_key);
        else {
            yyjson_mut_obj_add_strcpy(doc.doc, args, "file", path.c_str());
            yyjson_mut_obj_add_str(doc.doc, args, "symbol", "submit");
        }
        auto params = json_parse(doc.to_string());
        return json_parse(tools::impact_of(params.root(), conn, cache, primary.string()));
    }

    JsonDoc changes(const std::string& root = {}) {
        Connection conn(database, true);
        QueryCache cache(conn);
        auto params = json_parse(json_args(root.empty() ? extra.string() : root, base_commit));
        return json_parse(tools::detect_changes(params.root(), conn, cache, primary.string()));
    }
};

yyjson_val* field(yyjson_val* obj, const char* name) {
    auto* value = yyjson_obj_get(obj, name);
    REQUIRE(value != nullptr);
    return value;
}

bool diagnostic(yyjson_val* result, const std::string& code) {
    auto* diagnostics = field(result, "diagnostics");
    for (size_t i = 0; i < yyjson_arr_size(diagnostics); ++i) {
        auto* value = json_get_str(yyjson_arr_get(diagnostics, i), "code");
        if (value && value == code) return true;
    }
    return false;
}

int collect_sql(unsigned, void* context, void* statement, void*) {
    auto* raw = static_cast<sqlite3_stmt*>(statement);
    auto* sql = sqlite3_sql(raw);
    if (sql && (std::string(sql).starts_with("SELECT") ||
                std::string(sql).starts_with("WITH"))) {
        static_cast<std::vector<std::string>*>(context)->emplace_back(sql);
    }
    return 0;
}

} // namespace

TEST_CASE("Additional-root absolute path case and separator variants preserve exact target identity",
          "[unit][extra-root-resolution][lookup-api]") {
    Fixture fixture;
    auto path = (fixture.extra / "src" / "execution" / "query_host.cpp").string();
    std::vector<std::string> variants{path, (fixture.extra / "src/execution/query_host.cpp").generic_string()};
#ifdef _WIN32
    auto upper = path;
    std::transform(upper.begin(), upper.end(), upper.begin(),
        [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    variants.push_back(upper);
    auto mixed = lower_path(fixture.extra.string()) + "/src\\execution/query_host.cpp";
    variants.push_back(mixed);
#endif
    for (const auto& variant : variants) {
        INFO(variant);
        auto result = fixture.impact(variant);
        REQUIRE(result);
        REQUIRE_FALSE(yyjson_obj_get(result.root(), "error"));
        auto* symbol = field(result.root(), "symbol");
        CHECK(json_get_int(symbol, "node_id") == 21);
        CHECK(json_get_int(symbol, "root_id") == 1);
        CHECK(std::string(json_get_str(symbol, "stable_key")) == Fixture::target_key);
        CHECK(std::string(json_get_str(symbol, "file_path")) ==
              fixture.stored_extra + "/src/execution/query_host.cpp");
    }
    auto stable = fixture.impact({}, Fixture::target_key);
    REQUIRE(stable);
    CHECK(json_get_int(field(stable.root(), "symbol"), "node_id") == 21);
    auto primary = fixture.impact("src/execution/query_host.cpp");
    REQUIRE(primary);
    CHECK(json_get_int(field(primary.root(), "symbol"), "node_id") == 11);
}

TEST_CASE("Commit changed symbols map to the selected extra root and include C++ structs",
          "[unit][extra-root-resolution][detect-changes]") {
    Fixture fixture;
    std::string root = fixture.extra.string();
#ifdef _WIN32
    std::transform(root.begin(), root.end(), root.begin(),
        [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    std::replace(root.begin(), root.end(), '\\', '/');
#endif
    auto result = fixture.changes(root);
    REQUIRE(result);
    REQUIRE_FALSE(yyjson_obj_get(result.root(), "error"));
    CHECK(json_get_int(result.root(), "root_id") == 1);
    CHECK(std::string(json_get_str(result.root(), "root_role")) == "additional");
    CHECK(std::string(json_get_str(result.root(), "indexed_commit")) == fixture.head_commit);
    CHECK(std::string(json_get_str(result.root(), "compared_commit")) == fixture.head_commit);
    CHECK(std::string(json_get_str(result.root(), "mapping_status")) == "complete");
    CHECK(json_get_bool(result.root(), "analysis_complete"));
    auto* symbols = field(result.root(), "changed_symbols");
    REQUIRE(yyjson_arr_size(symbols) == 2);
    for (size_t i = 0; i < yyjson_arr_size(symbols); ++i) {
        auto* item = yyjson_arr_get(symbols, i);
        CHECK(json_get_int(item, "node_id") != 11);
        CHECK(std::string(json_get_str(item, "stable_key")).starts_with("1:"));
    }
    CHECK(diagnostic(result.root(), "unsupported_language"));
    auto* impacted = field(result.root(), "impacted_symbols");
    REQUIRE(yyjson_arr_size(impacted) == 2);
    CHECK(json_get_int(yyjson_arr_get(impacted, 0), "via_node_id") == 21);
    CHECK(std::string(json_get_str(yyjson_arr_get(impacted, 0), "evidence")) == "name-match");
}

TEST_CASE("Unknown additional snapshot never borrows primary revision metadata",
          "[unit][extra-root-resolution][detect-changes]") {
    Fixture fixture;
    {
        Connection conn(fixture.database);
        schema::delete_kv(conn, workspace_root_metadata_key(1, "git_head"));
        schema::set_kv(conn, "git_head", fixture.head_commit);
    }
    auto result = fixture.changes();
    REQUIRE(result);
    CHECK(yyjson_is_null(field(result.root(), "indexed_commit")));
    CHECK(std::string(json_get_str(result.root(), "index_revision_status")) == "unknown");
    CHECK_FALSE(json_get_bool(result.root(), "analysis_complete"));
    CHECK(diagnostic(result.root(), "indexed_revision_unknown"));
    CHECK(yyjson_arr_size(field(result.root(), "changed_symbols")) == 2);
}

TEST_CASE("Stale source symbols are not projected onto another indexed revision",
          "[unit][extra-root-resolution][detect-changes]") {
    Fixture fixture;
    {
        Connection conn(fixture.database);
        schema::set_kv(conn, workspace_root_metadata_key(1, "git_head"), fixture.base_commit);
        auto stale_hash = hash_string("int submit() { return 1; }\n");
        conn.exec("UPDATE files SET content_hash='" + stale_hash + "' WHERE id=2");
    }
    auto result = fixture.changes();
    REQUIRE(result);
    CHECK(std::string(json_get_str(result.root(), "index_revision_status")) == "stale");
    CHECK_FALSE(json_get_bool(result.root(), "analysis_complete"));
    CHECK(diagnostic(result.root(), "indexed_revision_mismatch"));
    CHECK(diagnostic(result.root(), "source_revision_unavailable"));
    auto* symbols = field(result.root(), "changed_symbols");
    REQUIRE(yyjson_arr_size(symbols) == 1);
    CHECK(json_get_int(yyjson_arr_get_first(symbols), "node_id") == 31);
    CHECK(yyjson_arr_size(field(result.root(), "unresolved_changed_files")) == 1);
}

TEST_CASE("Unindexed changed source is explicit rather than empty clean impact",
          "[unit][extra-root-resolution][detect-changes]") {
    Fixture fixture;
    fixture.write("src/execution/new.cpp", "int unindexed();\n");
    fixture.git("add .");
    fixture.git("commit -m unindexed");
    auto result = fixture.changes();
    REQUIRE(result);
    CHECK_FALSE(json_get_bool(result.root(), "analysis_complete"));
    CHECK(diagnostic(result.root(), "not_indexed"));
    CHECK(std::string(json_get_str(result.root(), "mapping_status")) == "partial");
    CHECK(yyjson_arr_size(field(result.root(), "unresolved_changed_files")) == 1);
}

TEST_CASE("Git CRLF worktree hashes are compared without changing the repository",
          "[unit][extra-root-resolution][detect-changes]") {
    Fixture fixture;
    {
        Connection conn(fixture.database);
        auto hash = hash_string("int submit() { return 2; }\r\n");
        conn.exec("UPDATE files SET content_hash='" + hash + "' WHERE id=2");
    }
    auto result = fixture.changes();
    REQUIRE(result);
    CHECK(json_get_bool(result.root(), "analysis_complete"));
    auto* resolutions = field(result.root(), "file_resolution");
    bool found = false;
    for (size_t i = 0; i < yyjson_arr_size(resolutions); ++i) {
        auto* status = json_get_str(yyjson_arr_get(resolutions, i), "source_revision_status");
        found |= status && std::string(status) == "matches_target_revision_crlf";
    }
    CHECK(found);
}

TEST_CASE("Requested additional subtree maps Git-relative paths through its owning root",
          "[unit][extra-root-resolution][detect-changes]") {
    Fixture fixture;
    auto result = fixture.changes((fixture.extra / "src" / "execution").string());
    REQUIRE(result);
    REQUIRE_FALSE(yyjson_obj_get(result.root(), "error"));
    CHECK(json_get_int(result.root(), "root_id") == 1);
    CHECK(json_get_bool(result.root(), "analysis_complete"));
    auto* files = field(result.root(), "changed_files");
    REQUIRE(yyjson_arr_size(files) == 1);
    CHECK(std::string(yyjson_get_str(yyjson_arr_get_first(files))) == "query_host.cpp");
    auto* symbols = field(result.root(), "changed_symbols");
    REQUIRE(yyjson_arr_size(symbols) == 1);
    CHECK(json_get_int(yyjson_arr_get_first(symbols), "node_id") == 21);
}

TEST_CASE("Deleted historical source is explicitly unavailable in commit mapping",
          "[unit][extra-root-resolution][detect-changes]") {
    Fixture fixture;
    fixture.write("src/execution/removed.cpp", "int removed();\n");
    fixture.git("add .");
    fixture.git("commit -m add-removed");
    fixture.base_commit = get_git_head(fixture.extra.string());
    {
        Connection conn(fixture.database);
        Fixture::add_file(conn, 5, fixture.stored_extra + "/src/execution/removed.cpp",
                          1, "int removed();\n");
    }
    fixture.git("rm src/execution/removed.cpp");
    fixture.git("commit -m remove-source");
    auto result = fixture.changes();
    REQUIRE(result);
    CHECK_FALSE(json_get_bool(result.root(), "analysis_complete"));
    auto* resolutions = field(result.root(), "file_resolution");
    bool found = false;
    for (size_t i = 0; i < yyjson_arr_size(resolutions); ++i) {
        auto* status = json_get_str(yyjson_arr_get(resolutions, i), "source_revision_status");
        found |= status && std::string(status) == "not_in_target_revision";
    }
    CHECK(found);
    CHECK(diagnostic(result.root(), "source_revision_unavailable"));
}

TEST_CASE("Unregistered repository requests are rejected instead of searching primary suffixes",
          "[unit][extra-root-resolution][detect-changes]") {
    Fixture fixture;
    auto unknown = fixture.base / "Unregistered";
    fs::create_directories(unknown);
    auto result = fixture.changes(unknown.string());
    REQUIRE(result);
    auto* error = field(result.root(), "error");
    CHECK(std::string(json_get_str(error, "message")).find("not an indexed") != std::string::npos);
}

TEST_CASE("Multiple same-name definitions require explicit stable-handle selection",
          "[unit][extra-root-resolution][lookup-api]") {
    Fixture fixture;
    {
        Connection conn(fixture.database);
        conn.exec(
            "INSERT INTO nodes(id,node_type,file_id,kind,name,qualname,start_line,end_line,stable_key) "
            "VALUES(22,'symbol',2,'function','submit','skiff::submit',1,1,"
            "'1:src/execution/query_host.cpp::function::submit#2')");
    }
    auto result = fixture.impact((fixture.extra / "src/execution/query_host.cpp").string());
    REQUIRE(result);
    CHECK(json_get_bool(result.root(), "ambiguous"));
    CHECK(yyjson_arr_size(field(result.root(), "candidates")) == 2);
    auto stable = fixture.impact({}, Fixture::target_key);
    REQUIRE(stable);
    CHECK(json_get_int(field(stable.root(), "symbol"), "node_id") == 21);
}

TEST_CASE("Indexed impact confidence filter excludes weak unrelated name matches",
          "[unit][extra-root-resolution][impact]") {
    Fixture fixture;
    Connection conn(fixture.database, true);
    QueryCache cache(conn);
    auto params = json_parse(
        R"({"stable_key":"1:src/execution/query_host.cpp::function::submit","min_confidence":0.7})");
    auto result = json_parse(tools::impact_of(params.root(), conn, cache, fixture.primary.string()));
    REQUIRE(result);
    auto* impacted = field(result.root(), "impacted");
    REQUIRE(yyjson_arr_size(impacted) == 1);
    CHECK(json_get_int(yyjson_arr_get_first(impacted), "node_id") == 41);
}

TEST_CASE("Indexed impact exposes heuristic edges and exact hop target identity",
          "[unit][extra-root-resolution][impact]") {
    Fixture fixture;
    auto result = fixture.impact({}, Fixture::target_key);
    REQUIRE(result);
    auto* impacted = field(result.root(), "impacted");
    REQUIRE(yyjson_arr_size(impacted) == 2);
    for (size_t i = 0; i < yyjson_arr_size(impacted); ++i) {
        auto* entry = yyjson_arr_get(impacted, i);
        CHECK(json_get_int(entry, "via_node_id") == 21);
        CHECK(std::string(json_get_str(entry, "via_stable_key")) == Fixture::target_key);
        CHECK(std::string(json_get_str(entry, "evidence")) == "name-match");
        CHECK(std::string(json_get_str(entry, "edge_source")) == "static");
        CHECK(std::string(json_get_str(entry, "edge_interpretation")) == "name_match_heuristic");
        CHECK(yyjson_is_num(field(entry, "confidence")));
    }
}

TEST_CASE("Commit impact cap does not present a truncated traversal as complete",
          "[unit][extra-root-resolution][detect-changes]") {
    Fixture fixture;
    {
        Connection conn(fixture.database);
        for (int i = 0; i < 501; ++i) {
            auto id = std::to_string(1000 + i);
            auto name = "caller_" + std::to_string(i);
            conn.exec(
                "INSERT INTO nodes(id,node_type,file_id,kind,name,stable_key) VALUES(" +
                id + ",'symbol',4,'function','" + name + "','1:runner::" + name + "')");
            conn.exec(
                "INSERT INTO edges(src_id,dst_id,kind,confidence) VALUES(" +
                id + ",21,'calls',0.75)");
        }
    }
    auto result = fixture.changes();
    REQUIRE(result);
    CHECK(std::string(json_get_str(result.root(), "mapping_status")) == "complete");
    CHECK(std::string(json_get_str(result.root(), "index_revision_status")) == "matches_target");
    CHECK_FALSE(json_get_bool(result.root(), "analysis_complete"));
    CHECK(json_get_bool(result.root(), "impact_truncated"));
    CHECK(json_get_int(result.root(), "impact_limit") == 500);
    CHECK(diagnostic(result.root(), "impact_limit_reached"));
    CHECK(yyjson_arr_size(field(result.root(), "impacted_symbols")) == 500);
}

TEST_CASE("Additional-root mapping and impact queries drive from bounded graph indexes",
          "[unit][extra-root-resolution][lookup-api][query-plan]") {
    Fixture fixture;
    Connection conn(fixture.database, true);
    QueryCache cache(conn);
    std::vector<std::string> queries;
    sqlite3_trace_v2(conn.raw(), SQLITE_TRACE_STMT, collect_sql, &queries);
    auto params = json_parse(json_args(fixture.extra.string(), fixture.base_commit));
    auto result = json_parse(tools::detect_changes(params.root(), conn, cache, fixture.primary.string()));
    REQUIRE(result);
    REQUIRE_FALSE(yyjson_obj_get(result.root(), "error"));
    auto impact_args = json_parse(
        R"({"stable_key":"1:src/execution/query_host.cpp::function::submit","depth":2})");
    auto impact = json_parse(tools::impact_of(impact_args.root(), conn, cache, fixture.primary.string()));
    REQUIRE(impact);
    sqlite3_trace_v2(conn.raw(), 0, nullptr, nullptr);
    cache.reset_all();
    REQUIRE_FALSE(queries.empty());
    for (const auto& sql : queries) {
        CAPTURE(sql);
        sqlite3_stmt* stmt = nullptr;
        REQUIRE(sqlite3_prepare_v2(conn.raw(), ("EXPLAIN QUERY PLAN " + sql).c_str(),
            -1, &stmt, nullptr) == SQLITE_OK);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            REQUIRE(text != nullptr);
            std::string plan(text);
            CAPTURE(plan);
            CHECK_FALSE(plan.starts_with("SCAN nodes"));
            CHECK_FALSE(plan.starts_with("SCAN edges"));
            CHECK_FALSE(plan.starts_with("SCAN n "));
            CHECK_FALSE(plan.starts_with("SCAN e "));
            if (plan.starts_with("SEARCH n ") || plan.starts_with("SEARCH target ")) {
                CHECK((plan.find("file_id") != std::string::npos ||
                      plan.find("stable_key") != std::string::npos ||
                      plan.find("PRIMARY KEY") != std::string::npos));
            }
        }
        sqlite3_finalize(stmt);
    }
}

#ifdef _WIN32
TEST_CASE("Case-normalized extra-file collisions are explicitly ambiguous",
          "[unit][extra-root-resolution][lookup-api]") {
    Fixture fixture;
    {
        Connection conn(fixture.database);
        Fixture::add_file(conn, 5, fixture.stored_extra + "/src/execution/Query_Host.cpp",
                          1, Fixture::target_content);
    }
    auto full = (fixture.extra / "SRC" / "EXECUTION" / "QUERY_HOST.CPP").string();
    auto result = fixture.impact(full);
    REQUIRE(result);
    auto* error = field(result.root(), "error");
    CHECK(std::string(json_get_str(error, "message")).find("Ambiguous") != std::string::npos);
    auto stable = fixture.impact({}, Fixture::target_key);
    REQUIRE(stable);
    CHECK(json_get_int(field(stable.root(), "symbol"), "node_id") == 21);
}
#endif
