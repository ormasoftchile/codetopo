// Tests for protocol-aware HTTP call extraction and listing.

#include <catch2/catch_test_macros.hpp>
#include "core/arena.h"
#include "core/arena_pool.h"
#include "index/parser.h"
#include "index/extractor.h"
#include "db/connection.h"
#include "db/schema.h"
#include "db/queries.h"
#include "mcp/tools.h"
#include "util/json.h"
#include <algorithm>
#include <filesystem>
#include <fstream>

using namespace codetopo;
namespace fs = std::filesystem;

namespace codetopo {
    void set_thread_arena(Arena* arena);
    void register_arena_allocator();
}

static void setup_arena_once() {
    static bool initialized = false;
    if (!initialized) {
        register_arena_allocator();
        initialized = true;
    }
}

static ExtractionResult extract_source(const std::string& language,
                                       const std::string& rel_path,
                                       const std::string& source) {
    setup_arena_once();
    ArenaPool pool(1, 32 * 1024 * 1024);
    auto lease = ArenaLease(pool);
    set_thread_arena(lease.get());

    Parser parser;
    REQUIRE(parser.set_language(language));
    auto tree = TreeGuard(parser.parse(source));
    REQUIRE(tree.tree);

    Extractor extractor(5000, 100, 0);
    return extractor.extract(tree.tree, source, language, rel_path);
}

static std::vector<ExtractedRef> http_refs(const ExtractionResult& result) {
    std::vector<ExtractedRef> refs;
    for (const auto& ref : result.refs) {
        if (ref.kind == "http_call") refs.push_back(ref);
    }
    return refs;
}

TEST_CASE("TypeScript extractor emits http_call refs for fetch and axios",
          "[unit][typescript][extractor]") {
    std::string source = R"(async function run(client: any) {
  await fetch("https://example.com/api/users?active=1");
  await axios.get("/api/orders?page=1");
  await this.http.post("/api/widgets");
  await fetch(buildUrl());
  await client.get("/not-http-client");
})";

    auto result = extract_source("typescript", "test.ts", source);
    auto refs = http_refs(result);

    REQUIRE(refs.size() == 3);
    CHECK(refs[0].name == "/api/users");
    CHECK(refs[0].evidence == "http_client_call");
    CHECK(refs[1].name == "/api/orders");
    CHECK(refs[2].name == "/api/widgets");
    CHECK(std::none_of(refs.begin(), refs.end(), [](const ExtractedRef& ref) {
        return ref.name == "/not-http-client";
    }));
}

TEST_CASE("Go extractor emits http_call refs for literal HTTP client calls",
          "[unit][go][extractor]") {
    std::string source = R"(package demo

import "net/http"

func run(client *http.Client, base string) {
    http.Get("https://service.local/api/users?active=1")
    client.Post("/api/orders", "application/json", nil)
    client.Do(req)
    httpClient.Delete("/api/widgets")
    http.Get(base + "/api/dynamic")
})";

    auto result = extract_source("go", "test.go", source);
    auto refs = http_refs(result);

    REQUIRE(refs.size() == 3);
    CHECK(refs[0].name == "/api/users");
    CHECK(refs[1].name == "/api/orders");
    CHECK(refs[2].name == "/api/widgets");
}

TEST_CASE("list_http_calls returns stored HTTP call refs",
          "[unit][mcp][http_call]") {
    auto base = fs::current_path() / ".codetopo-http-call-tool-test";
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base);

    auto db_path = base / "index.sqlite";
    {
        Connection conn(db_path);
        schema::ensure_schema(conn);
        conn.exec("INSERT INTO files(id, path, language, size_bytes, mtime_ns, content_hash, parse_status) "
                  "VALUES(1, 'src/app.ts', 'typescript', 100, 1000, 'hash', 'ok')");
        conn.exec("INSERT INTO nodes(id, node_type, file_id, kind, name, qualname, is_definition, stable_key) "
                  "VALUES(2, 'symbol', 1, 'function', 'run', 'run', 1, 'sym:run')");
        conn.exec("INSERT INTO refs(id, file_id, kind, name, start_line, start_col, end_line, end_col, evidence, containing_node_id) "
                  "VALUES(1, 1, 'http_call', '/api/users', 3, 2, 3, 30, 'http_client_call', 2)");
        conn.exec("INSERT INTO refs(id, file_id, kind, name, start_line, start_col, end_line, end_col, evidence, containing_node_id) "
                  "VALUES(2, 1, 'http_call', '/api/orders', 4, 2, 4, 35, 'http_client_call', 2)");
        schema::set_kv(conn, "repo_root", base.string());
    }

    {
        Connection conn(db_path, true);
        QueryCache cache(conn);
        auto params_doc = json_parse(R"({"file_pattern":"src/*.ts","limit":10})");
        REQUIRE(params_doc);

        auto result = tools::list_http_calls(params_doc.root(), conn, cache, base.string());
        auto doc = json_parse(result);
        REQUIRE(doc);

        auto* results = yyjson_obj_get(doc.root(), "results");
        REQUIRE(results != nullptr);
        REQUIRE(yyjson_arr_size(results) == 2);
        CHECK(json_get_int(doc.root(), "total") == 2);

        auto* first = yyjson_arr_get(results, 0);
        REQUIRE(first != nullptr);
        CHECK(std::string(json_get_str(first, "file")) == "src/app.ts");
        CHECK(std::string(json_get_str(first, "url_path")) == "/api/users");
        CHECK(std::string(json_get_str(first, "containing_symbol")) == "run");
    }

    fs::remove_all(base, ec);
}
