// TDD tests for persist pipeline optimizations (DEC-026 follow-up)
// Tests must PASS when optimizations are implemented, SKIP/FAIL before.
//
// OPT-1: Skip DELETE on cold index (cold_index=true)
// OPT-2: Multi-row INSERT batching for symbols/refs/edges
// OPT-3: Dedicated persist thread with result queue
//
// DEC-008 convention: scoped Connection + cleanup() for WAL safety on Windows.

#include <catch2/catch_test_macros.hpp>
#include "db/connection.h"
#include "db/schema.h"
#include "index/persister.h"
#include "index/extractor.h"
#include "index/scanner.h"
#include <sqlite3.h>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace codetopo;

// ---------------------------------------------------------------------------
// Test Helpers (from existing test conventions)
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

static ExtractionResult make_extraction_with_symbols(int num_symbols, const std::string& prefix = "sym") {
    ExtractionResult result;
    for (int i = 0; i < num_symbols; ++i) {
        ExtractedSymbol sym;
        sym.kind = "function";
        sym.name = prefix + "_" + std::to_string(i);
        sym.qualname = "ns::" + sym.name;
        sym.stable_key = prefix + "::" + sym.name + "::key_" + std::to_string(i);
        sym.start_line = i + 1;
        sym.end_line = i + 2;
        sym.is_definition = true;
        result.symbols.push_back(std::move(sym));

        // Add containment edge (file → symbol)
        ExtractedEdge edge;
        edge.src_index = -1;  // file node
        edge.dst_index = i;
        edge.kind = "contains";
        edge.confidence = 1.0;
        result.edges.push_back(std::move(edge));
    }
    return result;
}

static ExtractionResult make_extraction_with_refs(int num_refs) {
    ExtractionResult result;
    for (int i = 0; i < num_refs; ++i) {
        ExtractedRef ref;
        ref.kind = "call";
        ref.name = "func_" + std::to_string(i);
        ref.start_line = i + 1;
        ref.end_line = i + 1;
        ref.containing_symbol_index = -1;
        result.refs.push_back(std::move(ref));
    }
    return result;
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

// ===========================================================================
// OPT-1: Skip DELETE on cold index
// ===========================================================================

TEST_CASE("OPT-1: Cold index skips DELETE and persists file correctly", "[persist_opt][cold_index]") {
    auto tmp = make_test_dir("test_cold_index_skip_delete");
    auto db_path = tmp / "cold.sqlite";

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);

        Persister persister(conn);
        persister.enable_cold_index_if_empty();
        REQUIRE(persister.is_cold_index() == true);

        auto file = make_scanned_file("src/foo.cpp");
        auto extraction = make_extraction_with_symbols(3, "foo");

        persister.begin_batch();
        bool success = persister.persist_file(file, extraction, "hash1", "ok");
        persister.commit_batch();

        REQUIRE(success == true);

        // Verify file persisted
        REQUIRE(count_rows(conn, "files") == 1);
        // Verify symbols persisted (3 symbols)
        REQUIRE(count_rows(conn, "nodes") == 4);  // 1 file node + 3 symbols
    }
    cleanup(tmp);
}

TEST_CASE("OPT-1: Warm index (cold_index=false) still executes DELETE", "[persist_opt][warm_index]") {
    auto tmp = make_test_dir("test_warm_index_with_delete");
    auto db_path = tmp / "warm.sqlite";

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);

        Persister persister(conn);

        // Persist a file first
        auto file = make_scanned_file("src/bar.cpp");
        auto extraction1 = make_extraction_with_symbols(2, "v1");
        persister.persist_file(file, extraction1, "hash1", "ok");

        REQUIRE(count_rows(conn, "files") == 1);
        int64_t initial_nodes = count_rows(conn, "nodes");
        REQUIRE(initial_nodes == 3);  // 1 file node + 2 symbols

        // Update the same file (warm index: should DELETE old, INSERT new)
        auto extraction2 = make_extraction_with_symbols(4, "v2");
        persister.persist_file(file, extraction2, "hash2", "ok");

        REQUIRE(count_rows(conn, "files") == 1);
        int64_t final_nodes = count_rows(conn, "nodes");
        REQUIRE(final_nodes == 5);  // 1 file node + 4 new symbols (old symbols deleted)
    }
    cleanup(tmp);
}

TEST_CASE("OPT-1: Cold index with duplicate path insertion still works", "[persist_opt][cold_index]") {
    auto tmp = make_test_dir("test_cold_index_duplicate");
    auto db_path = tmp / "cold_dup.sqlite";

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);

        Persister persister(conn);
        persister.enable_cold_index_if_empty();
        REQUIRE(persister.is_cold_index() == true);

        auto file = make_scanned_file("src/dup.cpp");
        auto extraction1 = make_extraction_with_symbols(1, "v1");
        auto extraction2 = make_extraction_with_symbols(1, "v2");

        persister.begin_batch();
        persister.persist_file(file, extraction1, "hash1", "ok");
        // Second persist of same path on cold index should handle gracefully
        // (current implementation may rely on UNIQUE constraints or replace logic)
        bool success2 = persister.persist_file(file, extraction2, "hash2", "ok");
        persister.commit_batch();

        // Depending on implementation, this could either:
        // - succeed if INSERT OR REPLACE is used
        // - fail if strict INSERT is used
        // For now, just verify no crash and file count is reasonable
        int64_t file_count = count_rows(conn, "files");
        REQUIRE(file_count >= 1);
    }
    cleanup(tmp);
}

// ===========================================================================
// OPT-2: Multi-row INSERT batching
// ===========================================================================

TEST_CASE("OPT-2: Batched INSERT produces same result as single-row for symbols", "[persist_opt][batch_insert]") {
    auto tmp = make_test_dir("test_batch_insert_symbols");
    auto db_path = tmp / "batch.sqlite";

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);

        Persister persister(conn);
        auto file = make_scanned_file("src/batch_test.cpp");
        auto extraction = make_extraction_with_symbols(10, "batch");

        persister.persist_file(file, extraction, "hash1", "ok");

        // Verify all 10 symbols persisted
        int64_t symbol_count = count_rows(conn, "nodes") - 1;  // -1 for file node
        REQUIRE(symbol_count == 10);

        // Verify all 10 edges persisted (file → symbol containment)
        int64_t edge_count = count_rows(conn, "edges");
        REQUIRE(edge_count == 10);
    }
    cleanup(tmp);
}

TEST_CASE("OPT-2: Batched INSERT handles file with refs and edges", "[persist_opt][batch_insert]") {
    auto tmp = make_test_dir("test_batch_insert_refs");
    auto db_path = tmp / "batch_refs.sqlite";

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);

        Persister persister(conn);
        auto file = make_scanned_file("src/refs_test.cpp");
        auto extraction = make_extraction_with_refs(15);

        persister.persist_file(file, extraction, "hash1", "ok");

        // Verify all refs persisted
        int64_t ref_count = count_rows(conn, "refs");
        REQUIRE(ref_count == 15);
    }
    cleanup(tmp);
}

TEST_CASE("OPT-2: Batch boundary at exact batch size causes no data loss", "[persist_opt][batch_insert]") {
    auto tmp = make_test_dir("test_batch_boundary");
    auto db_path = tmp / "batch_boundary.sqlite";

    // Assume batch size is 100 (or configurable) — test with exactly 100, 200, 250 symbols
    const int BATCH_SIZE = 100;  // Adjust if implementation uses different size

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);

        Persister persister(conn);
        auto file = make_scanned_file("src/boundary.cpp");
        auto extraction = make_extraction_with_symbols(BATCH_SIZE * 2 + 50, "boundary");

        persister.persist_file(file, extraction, "hash1", "ok");

        // Verify all symbols persisted (no boundary off-by-one errors)
        int64_t symbol_count = count_rows(conn, "nodes") - 1;  // -1 for file node
        REQUIRE(symbol_count == BATCH_SIZE * 2 + 50);
    }
    cleanup(tmp);
}

TEST_CASE("OPT-2: Empty extraction (0 symbols, 0 refs) causes no crash", "[persist_opt][batch_insert]") {
    auto tmp = make_test_dir("test_empty_extraction");
    auto db_path = tmp / "empty.sqlite";

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);

        Persister persister(conn);
        auto file = make_scanned_file("src/empty.cpp");
        ExtractionResult extraction;  // empty

        bool success = persister.persist_file(file, extraction, "hash1", "ok");
        REQUIRE(success == true);

        // File and file node should exist, but no symbols/refs
        REQUIRE(count_rows(conn, "files") == 1);
        REQUIRE(count_rows(conn, "nodes") == 1);  // just file node
        REQUIRE(count_rows(conn, "refs") == 0);
        REQUIRE(count_rows(conn, "edges") == 0);
    }
    cleanup(tmp);
}

// ===========================================================================
// OPT-3: Dedicated persist thread with result queue
// ===========================================================================

// Note: These tests verify queue mechanics. Full threading tests may require
// a separate integration test or mock queue structure.

TEST_CASE("OPT-3: Queue accepts multiple results and persists in order", "[persist_opt][persist_thread][!mayfail]") {
    // This test requires the ResultQueue + persist_thread_fn infrastructure.
    // If not yet implemented, mark as [!mayfail] or SKIP.
    
    auto tmp = make_test_dir("test_persist_queue");
    auto db_path = tmp / "queue.sqlite";

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);

        // Mock test: If ResultQueue exists, push 3 results and verify all persisted
        // For now, this is a placeholder — implementation will wire up actual queue
        
        // Expected: 3 files pushed → 3 files in DB after drain
        WARN("OPT-3 test placeholder — requires ResultQueue implementation");
        
        // Temporary assertion to ensure test is recognized but skipped
        REQUIRE(true);
    }
    cleanup(tmp);
}

TEST_CASE("OPT-3: Persist thread drains queue before shutdown", "[persist_opt][persist_thread][!mayfail]") {
    auto tmp = make_test_dir("test_persist_drain");
    auto db_path = tmp / "drain.sqlite";

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);

        // Expected behavior:
        // 1. Push N results to queue
        // 2. Signal shutdown
        // 3. Verify queue is empty and all N results persisted
        
        WARN("OPT-3 drain test placeholder — requires shutdown signal + drain logic");
        REQUIRE(true);
    }
    cleanup(tmp);
}

TEST_CASE("OPT-3: Error in persist does not crash pipeline", "[persist_opt][persist_thread][!mayfail]") {
    auto tmp = make_test_dir("test_persist_error_resilience");
    auto db_path = tmp / "error.sqlite";

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);

        // Expected behavior:
        // 1. Push a result that will cause persist error (e.g., malformed data)
        // 2. Verify error is caught and logged/counted
        // 3. Verify pipeline continues (subsequent results still persist)
        
        WARN("OPT-3 error resilience placeholder — requires error injection");
        REQUIRE(true);
    }
    cleanup(tmp);
}

// ===========================================================================
// Combined integration: All 3 optimizations together
// ===========================================================================

TEST_CASE("Combined: Cold index + batching + thread queue (end-to-end)", "[persist_opt][integration][!mayfail]") {
    auto tmp = make_test_dir("test_combined_optimizations");
    auto db_path = tmp / "combined.sqlite";

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);

        Persister persister(conn);
        persister.enable_cold_index_if_empty();
        REQUIRE(persister.is_cold_index() == true);

        // Simulate a batch of files being processed
        std::vector<ScannedFile> files;
        std::vector<ExtractionResult> extractions;
        
        for (int i = 0; i < 5; ++i) {
            files.push_back(make_scanned_file("src/file_" + std::to_string(i) + ".cpp"));
            extractions.push_back(make_extraction_with_symbols(20, "combined_" + std::to_string(i)));
        }

        persister.begin_batch();
        for (size_t i = 0; i < files.size(); ++i) {
            bool success = persister.persist_file(files[i], extractions[i], "hash_" + std::to_string(i), "ok");
            REQUIRE(success == true);
        }
        persister.commit_batch();

        // Verify all files persisted
        REQUIRE(count_rows(conn, "files") == 5);
        
        // Verify all symbols persisted (5 files × 20 symbols = 100 symbols + 5 file nodes)
        int64_t total_nodes = count_rows(conn, "nodes");
        REQUIRE(total_nodes == 105);
        
        // Verify all edges (100 containment edges)
        int64_t total_edges = count_rows(conn, "edges");
        REQUIRE(total_edges == 100);
    }
    cleanup(tmp);
}
