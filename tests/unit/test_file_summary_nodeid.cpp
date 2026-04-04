// Tests for file_summary node_id inclusion.
// Validates that file_summary returns node_id for each symbol,
// that node_ids are unique within a file, and that they chain to symbol_get.
//
// DEC-008 convention: scoped Connection + cleanup() for WAL safety on Windows.

#include <catch2/catch_test_macros.hpp>
#include "db/connection.h"
#include "db/schema.h"
#include "db/queries.h"
#include "index/persister.h"
#include "index/extractor.h"
#include "index/scanner.h"
#include "mcp/tools.h"
#include "util/json.h"
#include <sqlite3.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <set>

namespace fs = std::filesystem;
using namespace codetopo;

// DEC-008: cleanup helper
static void cleanup(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
}

static fs::path make_test_dir(const std::string& name) {
    auto p = fs::temp_directory_path() / name;
    cleanup(p);
    fs::create_directories(p);
    return p;
}

static ScannedFile make_scanned_file(const std::string& relpath,
                                      const std::string& lang = "cpp",
                                      int64_t size = 1024,
                                      int64_t mtime = 100000) {
    return ScannedFile{
        .absolute_path = fs::path("/repo") / relpath,
        .relative_path = relpath,
        .language = lang,
        .size_bytes = size,
        .mtime_ns = mtime
    };
}

// Build extraction with N distinct symbols
static ExtractionResult make_extraction(int num_symbols, const std::string& prefix = "sym") {
    ExtractionResult result;
    for (int i = 0; i < num_symbols; ++i) {
        ExtractedSymbol sym;
        sym.kind = "function";
        sym.name = prefix + "_" + std::to_string(i);
        sym.qualname = "ns::" + sym.name;
        sym.stable_key = prefix + "::" + sym.name + "::key";
        sym.start_line = i + 1;
        sym.end_line = i + 2;
        sym.is_definition = true;
        result.symbols.push_back(std::move(sym));

        ExtractedEdge edge;
        edge.src_index = -1;
        edge.dst_index = i;
        edge.kind = "contains";
        edge.confidence = 1.0;
        result.edges.push_back(std::move(edge));
    }
    return result;
}

// Write a test source file to disk so file_summary can count lines
static void write_test_file(const fs::path& dir, const std::string& relpath,
                            const std::string& content) {
    auto full = dir / relpath;
    fs::create_directories(full.parent_path());
    std::ofstream f(full);
    f << content;
}

// ── Test 1: file_summary symbols have node_id ───────────────────────────────

TEST_CASE("file_summary symbols include node_id field", "[unit][file_summary]") {
    auto tmp = make_test_dir("test_file_summary_nodeid");
    auto db_path = tmp / "db.sqlite";
    auto repo_root = tmp.string();

    // Create a real file on disk for line counting
    std::string source = "class Foo {\npublic:\n    void bar() {}\n    int baz() { return 0; }\n};\n";
    write_test_file(tmp, "test.cpp", source);

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);

        // Persist a file with symbols
        Persister persister(conn);
        auto file = make_scanned_file("test.cpp", "cpp");
        auto extraction = make_extraction(3, "sym");
        persister.persist_file(file, extraction, "hash1", "ok");

        // Call file_summary
        QueryCache cache(conn);

        // Build params JSON: {"path": "test.cpp"}
        JsonMutDoc pdoc;
        auto* proot = pdoc.new_obj();
        pdoc.set_root(proot);
        yyjson_mut_obj_add_str(pdoc.doc, proot, "path", "test.cpp");
        std::string params_json = pdoc.to_string();

        auto parsed = json_parse(params_json);
        auto* params = parsed.root();

        std::string result = tools::file_summary(params, conn, cache, repo_root);

        // Parse result JSON
        auto result_doc = json_parse(result);
        REQUIRE(result_doc);
        auto* root = result_doc.root();
        REQUIRE(root);

        auto* symbols_arr = yyjson_obj_get(root, "symbols");
        REQUIRE(symbols_arr);
        REQUIRE(yyjson_arr_size(symbols_arr) == 3);

        // Each symbol must have a node_id field with a positive integer
        size_t idx, max;
        yyjson_val* sym;
        yyjson_arr_foreach(symbols_arr, idx, max, sym) {
            auto* nid = yyjson_obj_get(sym, "node_id");
            REQUIRE(nid);
            REQUIRE(yyjson_is_int(nid));
            REQUIRE(yyjson_get_sint(nid) > 0);
        }
    }
    cleanup(tmp);
}

// ── Test 2: node_id enables tool chaining to symbol_get ─────────────────────

TEST_CASE("file_summary node_id chains to symbol_get", "[unit][file_summary]") {
    auto tmp = make_test_dir("test_file_summary_chain");
    auto db_path = tmp / "db.sqlite";
    auto repo_root = tmp.string();

    std::string source = "void hello() {}\nvoid world() {}\n";
    write_test_file(tmp, "chain.cpp", source);

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);

        Persister persister(conn);
        auto file = make_scanned_file("chain.cpp", "cpp");

        ExtractionResult extraction;
        ExtractedSymbol s1;
        s1.kind = "function"; s1.name = "hello"; s1.qualname = "hello";
        s1.stable_key = "chain::hello::key"; s1.start_line = 1; s1.end_line = 1;
        s1.is_definition = true;
        extraction.symbols.push_back(s1);

        ExtractedSymbol s2;
        s2.kind = "function"; s2.name = "world"; s2.qualname = "world";
        s2.stable_key = "chain::world::key"; s2.start_line = 2; s2.end_line = 2;
        s2.is_definition = true;
        extraction.symbols.push_back(s2);

        ExtractedEdge e1; e1.src_index = -1; e1.dst_index = 0; e1.kind = "contains"; e1.confidence = 1.0;
        ExtractedEdge e2; e2.src_index = -1; e2.dst_index = 1; e2.kind = "contains"; e2.confidence = 1.0;
        extraction.edges.push_back(e1);
        extraction.edges.push_back(e2);

        persister.persist_file(file, extraction, "hash2", "ok");

        QueryCache cache(conn);

        // Step 1: call file_summary to get node_ids
        {
            JsonMutDoc pdoc;
            auto* proot = pdoc.new_obj();
            pdoc.set_root(proot);
            yyjson_mut_obj_add_str(pdoc.doc, proot, "path", "chain.cpp");
            auto parsed = json_parse(pdoc.to_string());

            std::string summary_result = tools::file_summary(parsed.root(), conn, cache, repo_root);
            auto sdoc = json_parse(summary_result);
            auto* symbols_arr = yyjson_obj_get(sdoc.root(), "symbols");
            REQUIRE(yyjson_arr_size(symbols_arr) >= 2);

            // Get node_id of first symbol
            auto* first_sym = yyjson_arr_get_first(symbols_arr);
            int64_t node_id = yyjson_get_sint(yyjson_obj_get(first_sym, "node_id"));
            REQUIRE(node_id > 0);

            const char* expected_name = yyjson_get_str(yyjson_obj_get(first_sym, "name"));
            REQUIRE(expected_name);

            // Step 2: call symbol_get with that node_id
            JsonMutDoc sg_pdoc;
            auto* sg_root = sg_pdoc.new_obj();
            sg_pdoc.set_root(sg_root);
            yyjson_mut_obj_add_int(sg_pdoc.doc, sg_root, "node_id", node_id);
            yyjson_mut_obj_add_bool(sg_pdoc.doc, sg_root, "include_source", false);
            auto sg_parsed = json_parse(sg_pdoc.to_string());

            std::string sg_result = tools::symbol_get(sg_parsed.root(), conn, cache, repo_root);
            auto sg_doc = json_parse(sg_result);
            REQUIRE(sg_doc);

            const char* sg_name = json_get_str(sg_doc.root(), "name");
            REQUIRE(sg_name);
            REQUIRE(std::string(sg_name) == std::string(expected_name));
        }
    }
    cleanup(tmp);
}

// ── Test 3: node_id values are unique within a file ─────────────────────────

TEST_CASE("file_summary node_ids are unique within a file", "[unit][file_summary]") {
    auto tmp = make_test_dir("test_file_summary_unique");
    auto db_path = tmp / "db.sqlite";
    auto repo_root = tmp.string();

    std::string source = "int a;\nint b;\nint c;\nint d;\nint e;\n";
    write_test_file(tmp, "unique.cpp", source);

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);

        Persister persister(conn);
        auto file = make_scanned_file("unique.cpp", "cpp");
        auto extraction = make_extraction(5, "var");
        persister.persist_file(file, extraction, "hash3", "ok");

        QueryCache cache(conn);

        JsonMutDoc pdoc;
        auto* proot = pdoc.new_obj();
        pdoc.set_root(proot);
        yyjson_mut_obj_add_str(pdoc.doc, proot, "path", "unique.cpp");
        auto parsed = json_parse(pdoc.to_string());

        std::string result = tools::file_summary(parsed.root(), conn, cache, repo_root);
        auto result_doc = json_parse(result);
        auto* symbols_arr = yyjson_obj_get(result_doc.root(), "symbols");
        REQUIRE(yyjson_arr_size(symbols_arr) == 5);

        std::set<int64_t> seen_ids;
        size_t idx, max;
        yyjson_val* sym;
        yyjson_arr_foreach(symbols_arr, idx, max, sym) {
            int64_t nid = yyjson_get_sint(yyjson_obj_get(sym, "node_id"));
            REQUIRE(nid > 0);
            REQUIRE(seen_ids.find(nid) == seen_ids.end());
            seen_ids.insert(nid);
        }
        REQUIRE(seen_ids.size() == 5);
    }
    cleanup(tmp);
}
