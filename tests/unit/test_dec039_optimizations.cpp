// DEC-039 optimization tests — batch symbol INSERT, pre-sized file read, parser reuse.
//
// DEC-008 convention: scoped Connection + cleanup() for WAL safety on Windows.
// Batch symbol INSERT uses SYMBOL_BATCH_SIZE = 20 (from persister.h).

#include <catch2/catch_test_macros.hpp>
#include "db/connection.h"
#include "db/schema.h"
#include "index/persister.h"
#include "index/extractor.h"
#include "index/scanner.h"
#include "index/parser.h"
#include "core/arena.h"
#include "core/arena_pool.h"
#include <sqlite3.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <cstring>
#include <set>

namespace fs = std::filesystem;
using namespace codetopo;

// Arena allocator must be registered before any tree-sitter parser use,
// because other tests in the suite call register_arena_allocator() globally.
namespace codetopo {
    void set_thread_arena(Arena* arena);
    void register_arena_allocator();
}

namespace fs = std::filesystem;
using namespace codetopo;

// ---------------------------------------------------------------------------
// Helpers (match existing test conventions from test_persist_optimizations.cpp)
// ---------------------------------------------------------------------------

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

// Generate N symbols with distinct names, qualnames, and stable_keys
static ExtractionResult make_extraction_with_symbols(int num_symbols,
                                                      const std::string& prefix = "sym") {
    ExtractionResult result;
    for (int i = 0; i < num_symbols; ++i) {
        ExtractedSymbol sym;
        sym.kind = "function";
        sym.name = prefix + "_" + std::to_string(i);
        sym.qualname = "ns::" + sym.name;
        sym.stable_key = prefix + "::" + sym.name + "::key_" + std::to_string(i);
        sym.start_line = i + 1;
        sym.start_col = 0;
        sym.end_line = i + 2;
        sym.end_col = 0;
        sym.is_definition = true;
        result.symbols.push_back(std::move(sym));

        // Containment edge (file node → symbol)
        ExtractedEdge edge;
        edge.src_index = -1;  // file node
        edge.dst_index = i;
        edge.kind = "contains";
        edge.confidence = 1.0;
        result.edges.push_back(std::move(edge));
    }
    return result;
}

// Add refs that point to specific symbols
static void add_refs_to_extraction(ExtractionResult& result, int num_refs) {
    for (int i = 0; i < num_refs; ++i) {
        ExtractedRef ref;
        ref.kind = "call";
        ref.name = "func_" + std::to_string(i);
        ref.start_line = i + 1;
        ref.end_line = i + 1;
        ref.containing_symbol_index = (result.symbols.empty()) ? -1 : (i % static_cast<int>(result.symbols.size()));
        result.refs.push_back(std::move(ref));
    }
}

static int64_t count_rows(Connection& conn, const char* table) {
    sqlite3_stmt* stmt = nullptr;
    std::string query = "SELECT COUNT(*) FROM " + std::string(table);
    sqlite3_prepare_v2(conn.raw(), query.c_str(), -1, &stmt, nullptr);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return -1;
    }
    int64_t count = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

// Query a node row from the nodes table by name
struct NodeRow {
    int64_t id;
    std::string node_type;
    int64_t file_id;
    std::string kind;
    std::string name;
    std::string qualname;
    int start_line;
    int end_line;
};

static std::vector<NodeRow> query_symbol_nodes(Connection& conn) {
    std::vector<NodeRow> rows;
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(conn.raw(),
        "SELECT id, node_type, file_id, kind, name, qualname, start_line, end_line "
        "FROM nodes WHERE node_type='symbol' ORDER BY id",
        -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        NodeRow r;
        r.id = sqlite3_column_int64(stmt, 0);
        r.node_type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        r.file_id = sqlite3_column_int64(stmt, 2);
        r.kind = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        r.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        auto qn = sqlite3_column_text(stmt, 5);
        r.qualname = qn ? reinterpret_cast<const char*>(qn) : "";
        r.start_line = sqlite3_column_int(stmt, 6);
        r.end_line = sqlite3_column_int(stmt, 7);
        rows.push_back(r);
    }
    sqlite3_finalize(stmt);
    return rows;
}

static std::vector<std::pair<int64_t, int64_t>> query_edge_ids(Connection& conn) {
    std::vector<std::pair<int64_t, int64_t>> edges;
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(conn.raw(),
        "SELECT src_id, dst_id FROM edges ORDER BY id", -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        edges.emplace_back(sqlite3_column_int64(stmt, 0), sqlite3_column_int64(stmt, 1));
    }
    sqlite3_finalize(stmt);
    return edges;
}

static std::vector<int64_t> query_ref_containing_ids(Connection& conn) {
    std::vector<int64_t> ids;
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(conn.raw(),
        "SELECT containing_node_id FROM refs ORDER BY id", -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (sqlite3_column_type(stmt, 0) == SQLITE_NULL) ids.push_back(-1);
        else ids.push_back(sqlite3_column_int64(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return ids;
}


// ===========================================================================
// TEST GROUP 1: Batch Symbol INSERT correctness
// Batch size = 20 (from persister.h SYMBOL_BATCH_SIZE)
// ===========================================================================

TEST_CASE("Batch symbol: 25 symbols — one full batch + 5 remainder", "[persist][batch_symbol]") {
    auto tmp = make_test_dir("test_batch_sym_25");
    auto db_path = tmp / "db.sqlite";

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);

        Persister persister(conn);
        auto file = make_scanned_file("src/big.cpp");
        auto extraction = make_extraction_with_symbols(25, "bs25");

        persister.begin_batch();
        bool ok = persister.persist_file(file, extraction, "hash1", "ok");
        persister.commit_batch();

        REQUIRE(ok);

        // 1 file node + 25 symbols = 26
        REQUIRE(count_rows(conn, "nodes") == 26);
        REQUIRE(count_rows(conn, "files") == 1);

        // Verify ALL symbol data
        auto syms = query_symbol_nodes(conn);
        REQUIRE(syms.size() == 25);
        for (int i = 0; i < 25; ++i) {
            CHECK(syms[i].name == "bs25_" + std::to_string(i));
            CHECK(syms[i].qualname == "ns::bs25_" + std::to_string(i));
            CHECK(syms[i].kind == "function");
            CHECK(syms[i].start_line == i + 1);
            CHECK(syms[i].end_line == i + 2);
        }

        // Verify symbol IDs are sequential (no gaps from batch boundary)
        for (size_t i = 1; i < syms.size(); ++i) {
            CHECK(syms[i].id == syms[i - 1].id + 1);
        }

        // 25 containment edges (file_node → each symbol)
        REQUIRE(count_rows(conn, "edges") == 25);
        auto edges = query_edge_ids(conn);
        REQUIRE(edges.size() == 25);
        // Each edge's dst_id should match the corresponding symbol's id
        for (int i = 0; i < 25; ++i) {
            CHECK(edges[i].second == syms[i].id);
        }
    }
    cleanup(tmp);
}

TEST_CASE("Batch symbol: exactly 20 symbols — single full batch, no remainder", "[persist][batch_symbol]") {
    auto tmp = make_test_dir("test_batch_sym_20");
    auto db_path = tmp / "db.sqlite";

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);

        Persister persister(conn);
        auto file = make_scanned_file("src/exact20.cpp");
        auto extraction = make_extraction_with_symbols(20, "ex20");

        persister.begin_batch();
        bool ok = persister.persist_file(file, extraction, "hash1", "ok");
        persister.commit_batch();

        REQUIRE(ok);
        REQUIRE(count_rows(conn, "nodes") == 21); // 1 file + 20 syms

        auto syms = query_symbol_nodes(conn);
        REQUIRE(syms.size() == 20);

        // Sequential IDs
        for (size_t i = 1; i < syms.size(); ++i) {
            CHECK(syms[i].id == syms[i - 1].id + 1);
        }

        // All names present
        for (int i = 0; i < 20; ++i) {
            CHECK(syms[i].name == "ex20_" + std::to_string(i));
        }

        REQUIRE(count_rows(conn, "edges") == 20);
    }
    cleanup(tmp);
}

TEST_CASE("Batch symbol: 21 symbols — one full batch + 1 remainder", "[persist][batch_symbol]") {
    auto tmp = make_test_dir("test_batch_sym_21");
    auto db_path = tmp / "db.sqlite";

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);

        Persister persister(conn);
        auto file = make_scanned_file("src/boundary21.cpp");
        auto extraction = make_extraction_with_symbols(21, "b21");

        persister.begin_batch();
        bool ok = persister.persist_file(file, extraction, "hash1", "ok");
        persister.commit_batch();

        REQUIRE(ok);
        REQUIRE(count_rows(conn, "nodes") == 22); // 1 file + 21 syms

        auto syms = query_symbol_nodes(conn);
        REQUIRE(syms.size() == 21);

        // Sequential IDs across batch boundary (batch=20, remainder=1)
        for (size_t i = 1; i < syms.size(); ++i) {
            CHECK(syms[i].id == syms[i - 1].id + 1);
        }

        // Verify the 21st symbol (remainder) has correct data
        CHECK(syms[20].name == "b21_20");
        CHECK(syms[20].qualname == "ns::b21_20");
        CHECK(syms[20].start_line == 21);

        REQUIRE(count_rows(conn, "edges") == 21);
    }
    cleanup(tmp);
}

TEST_CASE("Batch symbol: 1 symbol — no batch, single-row only", "[persist][batch_symbol]") {
    auto tmp = make_test_dir("test_batch_sym_1");
    auto db_path = tmp / "db.sqlite";

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);

        Persister persister(conn);
        auto file = make_scanned_file("src/one.cpp");
        auto extraction = make_extraction_with_symbols(1, "single");

        persister.begin_batch();
        bool ok = persister.persist_file(file, extraction, "hash1", "ok");
        persister.commit_batch();

        REQUIRE(ok);
        REQUIRE(count_rows(conn, "nodes") == 2); // 1 file + 1 sym

        auto syms = query_symbol_nodes(conn);
        REQUIRE(syms.size() == 1);
        CHECK(syms[0].name == "single_0");
        CHECK(syms[0].kind == "function");

        REQUIRE(count_rows(conn, "edges") == 1);
    }
    cleanup(tmp);
}

TEST_CASE("Batch symbol: 0 symbols — empty extraction", "[persist][batch_symbol]") {
    auto tmp = make_test_dir("test_batch_sym_0");
    auto db_path = tmp / "db.sqlite";

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);

        Persister persister(conn);
        auto file = make_scanned_file("src/empty.cpp");
        ExtractionResult extraction; // empty

        persister.begin_batch();
        bool ok = persister.persist_file(file, extraction, "hash1", "ok");
        persister.commit_batch();

        REQUIRE(ok);
        REQUIRE(count_rows(conn, "files") == 1);
        REQUIRE(count_rows(conn, "nodes") == 1); // file node only
        REQUIRE(count_rows(conn, "edges") == 0);
        REQUIRE(count_rows(conn, "refs") == 0);
    }
    cleanup(tmp);
}

TEST_CASE("Batch symbol: 40 symbols — two full batches, no remainder", "[persist][batch_symbol]") {
    auto tmp = make_test_dir("test_batch_sym_40");
    auto db_path = tmp / "db.sqlite";

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);

        Persister persister(conn);
        auto file = make_scanned_file("src/multi_batch.cpp");
        auto extraction = make_extraction_with_symbols(40, "mb40");

        persister.begin_batch();
        bool ok = persister.persist_file(file, extraction, "hash1", "ok");
        persister.commit_batch();

        REQUIRE(ok);
        REQUIRE(count_rows(conn, "nodes") == 41); // 1 file + 40

        auto syms = query_symbol_nodes(conn);
        REQUIRE(syms.size() == 40);

        // IDs sequential across two batch boundaries
        for (size_t i = 1; i < syms.size(); ++i) {
            CHECK(syms[i].id == syms[i - 1].id + 1);
        }

        // Spot-check data from each batch
        CHECK(syms[0].name == "mb40_0");
        CHECK(syms[19].name == "mb40_19");  // last of batch 1
        CHECK(syms[20].name == "mb40_20");  // first of batch 2
        CHECK(syms[39].name == "mb40_39");  // last of batch 2

        REQUIRE(count_rows(conn, "edges") == 40);
    }
    cleanup(tmp);
}

TEST_CASE("Batch symbol: refs reference correct node IDs", "[persist][batch_symbol]") {
    auto tmp = make_test_dir("test_batch_sym_refs");
    auto db_path = tmp / "db.sqlite";

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);

        Persister persister(conn);
        auto file = make_scanned_file("src/with_refs.cpp");
        auto extraction = make_extraction_with_symbols(25, "wr");
        add_refs_to_extraction(extraction, 10);

        persister.begin_batch();
        bool ok = persister.persist_file(file, extraction, "hash1", "ok");
        persister.commit_batch();

        REQUIRE(ok);
        REQUIRE(count_rows(conn, "refs") == 10);

        // Verify containing_node_id references valid symbol nodes
        auto syms = query_symbol_nodes(conn);
        auto ref_containing = query_ref_containing_ids(conn);
        REQUIRE(ref_containing.size() == 10);

        std::set<int64_t> valid_sym_ids;
        for (auto& s : syms) valid_sym_ids.insert(s.id);

        for (auto cid : ref_containing) {
            if (cid != -1) {
                CHECK(valid_sym_ids.count(cid) == 1);
            }
        }
    }
    cleanup(tmp);
}

TEST_CASE("Batch symbol: edges reference correct node IDs across batch boundary", "[persist][batch_symbol]") {
    auto tmp = make_test_dir("test_batch_sym_edges");
    auto db_path = tmp / "db.sqlite";

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);

        Persister persister(conn);
        auto file = make_scanned_file("src/edge_check.cpp");
        auto extraction = make_extraction_with_symbols(25, "ec");

        // Add cross-symbol edges (sym[0] → sym[20], sym[1] → sym[21], etc.)
        // These cross the batch boundary (batch 1 symbols → batch 2 symbols)
        for (int i = 0; i < 5; ++i) {
            ExtractedEdge edge;
            edge.src_index = i;      // in batch 1 (indices 0-19)
            edge.dst_index = 20 + i; // in batch 2 remainder (indices 20-24)
            edge.kind = "calls";
            edge.confidence = 0.9;
            extraction.edges.push_back(std::move(edge));
        }

        persister.begin_batch();
        bool ok = persister.persist_file(file, extraction, "hash1", "ok");
        persister.commit_batch();

        REQUIRE(ok);
        // 25 containment edges + 5 cross-symbol edges = 30
        REQUIRE(count_rows(conn, "edges") == 30);

        auto syms = query_symbol_nodes(conn);
        auto edges = query_edge_ids(conn);

        // Verify the last 5 edges are cross-symbol (src from batch 1, dst from remainder)
        for (int i = 0; i < 5; ++i) {
            auto& e = edges[25 + i]; // after the 25 containment edges
            CHECK(e.first == syms[i].id);       // src = sym[i]
            CHECK(e.second == syms[20 + i].id); // dst = sym[20+i]
        }
    }
    cleanup(tmp);
}


// ===========================================================================
// TEST GROUP 2: Pre-sized file read
// read_file_content() — pre-allocates string to file size
// ===========================================================================

TEST_CASE("File read: 0-byte file returns empty string", "[file_read][presized]") {
    auto tmp = make_test_dir("test_fread_0");
    auto fpath = tmp / "empty.txt";

    {
        std::ofstream f(fpath, std::ios::binary);
        // write nothing
    }

    auto content = read_file_content(fpath);
    CHECK(content.empty());
    cleanup(tmp);
}

TEST_CASE("File read: 1-byte file", "[file_read][presized]") {
    auto tmp = make_test_dir("test_fread_1");
    auto fpath = tmp / "one.txt";

    {
        std::ofstream f(fpath, std::ios::binary);
        f.put('X');
    }

    auto content = read_file_content(fpath);
    REQUIRE(content.size() == 1);
    CHECK(content[0] == 'X');
    cleanup(tmp);
}

TEST_CASE("File read: 1KB file preserves content", "[file_read][presized]") {
    auto tmp = make_test_dir("test_fread_1kb");
    auto fpath = tmp / "data.bin";
    std::string expected(1024, '\0');
    for (size_t i = 0; i < expected.size(); ++i) {
        expected[i] = static_cast<char>(i % 256);
    }

    {
        std::ofstream f(fpath, std::ios::binary);
        f.write(expected.data(), expected.size());
    }

    auto content = read_file_content(fpath);
    REQUIRE(content.size() == expected.size());
    CHECK(content == expected);
    cleanup(tmp);
}

TEST_CASE("File read: 100KB file preserves content", "[file_read][presized]") {
    auto tmp = make_test_dir("test_fread_100kb");
    auto fpath = tmp / "big.bin";
    std::string expected(100 * 1024, '\0');
    for (size_t i = 0; i < expected.size(); ++i) {
        expected[i] = static_cast<char>((i * 37 + 13) % 256);
    }

    {
        std::ofstream f(fpath, std::ios::binary);
        f.write(expected.data(), expected.size());
    }

    auto content = read_file_content(fpath);
    REQUIRE(content.size() == expected.size());
    CHECK(content == expected);
    cleanup(tmp);
}

TEST_CASE("File read: 1MB file preserves content", "[file_read][presized]") {
    auto tmp = make_test_dir("test_fread_1mb");
    auto fpath = tmp / "large.bin";
    std::string expected(1024 * 1024, '\0');
    for (size_t i = 0; i < expected.size(); ++i) {
        expected[i] = static_cast<char>((i * 71 + 29) % 256);
    }

    {
        std::ofstream f(fpath, std::ios::binary);
        f.write(expected.data(), expected.size());
    }

    auto content = read_file_content(fpath);
    REQUIRE(content.size() == expected.size());
    CHECK(content == expected);
    cleanup(tmp);
}

TEST_CASE("File read: binary content with null bytes and high bytes preserved", "[file_read][presized]") {
    auto tmp = make_test_dir("test_fread_binary");
    auto fpath = tmp / "binary.bin";
    // Craft content with null bytes, 0xFF, and every byte value
    std::string expected(512, '\0');
    for (size_t i = 0; i < 256; ++i) {
        expected[i * 2]     = static_cast<char>(i);
        expected[i * 2 + 1] = static_cast<char>(255 - i);
    }

    {
        std::ofstream f(fpath, std::ios::binary);
        f.write(expected.data(), expected.size());
    }

    auto content = read_file_content(fpath);
    REQUIRE(content.size() == expected.size());
    // Byte-by-byte comparison to catch any null-termination bugs
    CHECK(std::memcmp(content.data(), expected.data(), expected.size()) == 0);
    cleanup(tmp);
}

TEST_CASE("File read: nonexistent file returns empty string gracefully", "[file_read][presized]") {
    auto fpath = fs::temp_directory_path() / "test_fread_nonexistent" / "nosuchfile.txt";
    cleanup(fpath.parent_path());

    auto content = read_file_content(fpath);
    CHECK(content.empty());
}


// ===========================================================================
// TEST GROUP 3: Parser reuse
// Verify that reusing a Parser object across parses produces correct results.
// Arena setup required because ts_set_allocator() may be active from other tests.
// ===========================================================================

TEST_CASE("Parser reuse: two C++ files parsed sequentially produce valid trees", "[parser][reuse]") {
    register_arena_allocator();
    ArenaPool pool(1, 32 * 1024 * 1024);
    auto lease = ArenaLease(pool);
    set_thread_arena(lease.get());

    Parser parser;
    REQUIRE(parser.set_language("cpp"));

    // Parse first C++ snippet — verify immediately
    {
        std::string src1 = "int main() { return 0; }";
        TreeGuard tree1(parser.parse(src1));
        REQUIRE(static_cast<bool>(tree1));
        CHECK(ts_node_child_count(tree1.root()) > 0);
        CHECK(std::string(ts_node_type(tree1.root())) == "translation_unit");
    }

    // Parse second C++ snippet (reusing same parser) — verify immediately
    {
        std::string src2 = "namespace foo { class Bar { void baz(); }; }";
        TreeGuard tree2(parser.parse(src2));
        REQUIRE(static_cast<bool>(tree2));
        CHECK(ts_node_child_count(tree2.root()) > 0);
        CHECK(std::string(ts_node_type(tree2.root())) == "translation_unit");
    }

    set_thread_arena(nullptr);
}

TEST_CASE("Parser reuse: C++ then C# — language switch produces correct trees", "[parser][reuse]") {
    register_arena_allocator();
    ArenaPool pool(1, 32 * 1024 * 1024);
    auto lease = ArenaLease(pool);
    set_thread_arena(lease.get());

    Parser parser;

    // Parse C++ — scope tree lifetime
    {
        REQUIRE(parser.set_language("cpp"));
        std::string cpp_src = "void hello() { int x = 42; }";
        TreeGuard cpp_tree(parser.parse(cpp_src));
        REQUIRE(static_cast<bool>(cpp_tree));
        TSNode cpp_root = cpp_tree.root();
        CHECK(std::string(ts_node_type(cpp_root)) == "translation_unit");
    }

    // Switch to C# and parse — previous tree already freed
    {
        REQUIRE(parser.set_language("csharp"));
        std::string cs_src = "using System; namespace Foo { class Bar { void Baz() {} } }";
        TreeGuard cs_tree(parser.parse(cs_src));
        REQUIRE(static_cast<bool>(cs_tree));
        TSNode cs_root = cs_tree.root();
        CHECK(std::string(ts_node_type(cs_root)) == "compilation_unit");
    }

    set_thread_arena(nullptr);
}

TEST_CASE("Parser reuse: many sequential parses same language don't leak or corrupt", "[parser][reuse]") {
    register_arena_allocator();
    ArenaPool pool(1, 32 * 1024 * 1024);
    auto lease = ArenaLease(pool);
    set_thread_arena(lease.get());

    Parser parser;
    REQUIRE(parser.set_language("python"));

    for (int i = 0; i < 50; ++i) {
        std::string src = "def func_" + std::to_string(i) + "(): return " + std::to_string(i);
        TreeGuard tree(parser.parse(src));
        REQUIRE(static_cast<bool>(tree));
        CHECK(ts_node_child_count(tree.root()) > 0);
    }

    set_thread_arena(nullptr);
}

TEST_CASE("Parser reuse: language round-trip C++ → Python → C++ produces valid trees", "[parser][reuse]") {
    register_arena_allocator();
    ArenaPool pool(1, 32 * 1024 * 1024);
    auto lease = ArenaLease(pool);
    set_thread_arena(lease.get());

    Parser parser;

    // C++ — scoped
    {
        REQUIRE(parser.set_language("cpp"));
        TreeGuard t1(parser.parse("int a = 1;"));
        REQUIRE(static_cast<bool>(t1));
        CHECK(std::string(ts_node_type(t1.root())) == "translation_unit");
    }

    // Python — scoped
    {
        REQUIRE(parser.set_language("python"));
        TreeGuard t2(parser.parse("a = 1"));
        REQUIRE(static_cast<bool>(t2));
        CHECK(std::string(ts_node_type(t2.root())) == "module");
    }

    // Back to C++ — scoped
    {
        REQUIRE(parser.set_language("cpp"));
        TreeGuard t3(parser.parse("class Foo {};"));
        REQUIRE(static_cast<bool>(t3));
        CHECK(std::string(ts_node_type(t3.root())) == "translation_unit");
    }

    set_thread_arena(nullptr);
}

TEST_CASE("Parser reuse: unsupported language returns false, parser remains usable", "[parser][reuse]") {
    register_arena_allocator();
    ArenaPool pool(1, 32 * 1024 * 1024);
    auto lease = ArenaLease(pool);
    set_thread_arena(lease.get());

    Parser parser;

    // Set valid language first
    REQUIRE(parser.set_language("cpp"));
    {
        TreeGuard t1(parser.parse("int x;"));
        REQUIRE(static_cast<bool>(t1));
    }

    // Try unsupported language — should return false (sql is deferred)
    CHECK(parser.set_language("sql") == false);

    // Parser should still be usable with the previous language
    {
        TreeGuard t2(parser.parse("int y;"));
        REQUIRE(static_cast<bool>(t2));
        CHECK(std::string(ts_node_type(t2.root())) == "translation_unit");
    }

    set_thread_arena(nullptr);
}
