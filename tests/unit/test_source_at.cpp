// Tests for source_at MCP tool (T092).
// Validates line range reading, edge cases, and error handling.
//
// source_at reads from disk via read_source_snippet — tests create actual files.
// DEC-008 convention: scoped Connection + cleanup() for WAL safety on Windows.

#include <catch2/catch_test_macros.hpp>
#include "db/connection.h"
#include "db/schema.h"
#include "db/queries.h"
#include "mcp/tools.h"
#include "util/json.h"
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace codetopo;

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

static void write_test_file(const fs::path& dir, const std::string& relpath,
                            const std::string& content) {
    auto full = dir / relpath;
    fs::create_directories(full.parent_path());
    std::ofstream f(full);
    f << content;
}

// Helper: build source_at params JSON and call the tool
static std::string call_source_at(Connection& conn, QueryCache& cache,
                                  const std::string& repo_root,
                                  const std::string& path,
                                  int64_t start_line, int64_t end_line) {
    JsonMutDoc pdoc;
    auto* proot = pdoc.new_obj();
    pdoc.set_root(proot);
    yyjson_mut_obj_add_str(pdoc.doc, proot, "path", path.c_str());
    yyjson_mut_obj_add_int(pdoc.doc, proot, "start_line", start_line);
    yyjson_mut_obj_add_int(pdoc.doc, proot, "end_line", end_line);
    auto parsed = json_parse(pdoc.to_string());
    return tools::source_at(parsed.root(), conn, cache, repo_root);
}

// ── Test 1: source_at returns correct lines ─────────────────────────────────

TEST_CASE("source_at returns correct line range", "[unit][source_at]") {
    auto tmp = make_test_dir("test_source_at_range");
    auto db_path = tmp / "db.sqlite";
    auto repo_root = tmp.string();

    // 5-line file with known content
    std::string content =
        "line one\n"
        "line two\n"
        "line three\n"
        "line four\n"
        "line five\n";
    write_test_file(tmp, "range.txt", content);

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);
        QueryCache cache(conn);

        std::string result = call_source_at(conn, cache, repo_root, "range.txt", 2, 4);
        auto doc = json_parse(result);
        REQUIRE(doc);

        const char* source = json_get_str(doc.root(), "source");
        REQUIRE(source);

        std::string expected = "line two\nline three\nline four\n";
        REQUIRE(std::string(source) == expected);

        // Verify metadata
        REQUIRE(json_get_int(doc.root(), "start_line") == 2);
        REQUIRE(json_get_int(doc.root(), "end_line") == 4);
    }
    cleanup(tmp);
}

// ── Test 2: source_at single line ───────────────────────────────────────────

TEST_CASE("source_at returns single line when start==end", "[unit][source_at]") {
    auto tmp = make_test_dir("test_source_at_single");
    auto db_path = tmp / "db.sqlite";
    auto repo_root = tmp.string();

    std::string content = "alpha\nbeta\ngamma\n";
    write_test_file(tmp, "single.txt", content);

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);
        QueryCache cache(conn);

        std::string result = call_source_at(conn, cache, repo_root, "single.txt", 2, 2);
        auto doc = json_parse(result);
        REQUIRE(doc);

        const char* source = json_get_str(doc.root(), "source");
        REQUIRE(source);
        REQUIRE(std::string(source) == "beta\n");
    }
    cleanup(tmp);
}

// ── Test 3: source_at rejects invalid range ─────────────────────────────────

TEST_CASE("source_at rejects end_line < start_line", "[unit][source_at]") {
    auto tmp = make_test_dir("test_source_at_invalid");
    auto db_path = tmp / "db.sqlite";
    auto repo_root = tmp.string();

    write_test_file(tmp, "dummy.txt", "a\nb\nc\n");

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);
        QueryCache cache(conn);

        std::string result = call_source_at(conn, cache, repo_root, "dummy.txt", 3, 1);
        auto doc = json_parse(result);
        REQUIRE(doc);

        // Should contain an error (McpError format)
        auto* error = yyjson_obj_get(doc.root(), "error");
        REQUIRE(error);
    }
    cleanup(tmp);
}

// ── Test 4: source_at rejects too large range ───────────────────────────────

TEST_CASE("source_at rejects range exceeding 500 lines", "[unit][source_at]") {
    auto tmp = make_test_dir("test_source_at_toolarge");
    auto db_path = tmp / "db.sqlite";
    auto repo_root = tmp.string();

    write_test_file(tmp, "big.txt", "content\n");

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);
        QueryCache cache(conn);

        // 501-line range exceeds the 500 max
        std::string result = call_source_at(conn, cache, repo_root, "big.txt", 1, 502);
        auto doc = json_parse(result);
        REQUIRE(doc);

        auto* error = yyjson_obj_get(doc.root(), "error");
        REQUIRE(error);
    }
    cleanup(tmp);
}

// ── Test 5: source_at nonexistent file ──────────────────────────────────────

TEST_CASE("source_at returns error for missing file", "[unit][source_at]") {
    auto tmp = make_test_dir("test_source_at_missing");
    auto db_path = tmp / "db.sqlite";
    auto repo_root = tmp.string();

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);
        QueryCache cache(conn);

        std::string result = call_source_at(conn, cache, repo_root, "nonexistent.txt", 1, 5);
        auto doc = json_parse(result);
        REQUIRE(doc);

        auto* error = yyjson_obj_get(doc.root(), "error");
        REQUIRE(error);
    }
    cleanup(tmp);
}

// ── Test 6: source_at out-of-bounds lines ───────────────────────────────────

TEST_CASE("source_at with out-of-bounds end returns available lines", "[unit][source_at]") {
    auto tmp = make_test_dir("test_source_at_oob");
    auto db_path = tmp / "db.sqlite";
    auto repo_root = tmp.string();

    // 4-line file
    std::string content = "one\ntwo\nthree\nfour\n";
    write_test_file(tmp, "short.txt", content);

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);
        QueryCache cache(conn);

        // Request lines 3-10, but file only has 4 lines
        std::string result = call_source_at(conn, cache, repo_root, "short.txt", 3, 10);
        auto doc = json_parse(result);
        REQUIRE(doc);

        // read_source_snippet returns what's available — lines 3-4
        const char* source = json_get_str(doc.root(), "source");
        REQUIRE(source);

        std::string expected = "three\nfour\n";
        REQUIRE(std::string(source) == expected);
    }
    cleanup(tmp);
}
