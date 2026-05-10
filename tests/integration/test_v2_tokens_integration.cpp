// Integration tests for v2-Tokens Phase 1 MCP tools  
// Simplified version: tests _meta block presence and basic functionality
#include <catch2/catch_test_macros.hpp>
#include "mcp/tools.h"
#include "db/connection.h"
#include "db/queries.h"
#include "db/schema.h"
#include "util/json.h"
#include <string>

using namespace codetopo;

// Helper: parse JSON response and check for _meta block
static bool has_meta_block(const std::string& json_response) {
    yyjson_doc* doc = yyjson_read(json_response.c_str(), json_response.size(), 0);
    if (!doc) return false;
    auto* root = yyjson_doc_get_root(doc);
    auto* meta = yyjson_obj_get(root, "_meta");
    bool has_it = (meta != nullptr);
    yyjson_doc_free(doc);
    return has_it;
}

static bool get_meta_truncated(const std::string& json_response) {
    yyjson_doc* doc = yyjson_read(json_response.c_str(), json_response.size(), 0);
    if (!doc) return false;
    auto* root = yyjson_doc_get_root(doc);
    auto* meta = yyjson_obj_get(root, "_meta");
    if (!meta) {
        yyjson_doc_free(doc);
        return false;
    }
    auto* trunc = yyjson_obj_get(meta, "truncated");
    bool val = trunc ? yyjson_get_bool(trunc) : false;
    yyjson_doc_free(doc);
    return val;
}

// Helper: create immutable params from mutable doc
static yyjson_doc* make_immut_params(JsonMutDoc& doc) {
    std::string str = doc.to_string();
    return yyjson_read(str.c_str(), str.size(), 0);
}

TEST_CASE("find_implementations: v2-Tokens _meta block", "[v2-tokens]") {
    Connection conn(":memory:");
    schema::create_tables(conn);
    
    conn.exec("INSERT INTO files (path, language, size_bytes, mtime_ns, content_hash, parse_status) VALUES "
              "('base.cpp', 'cpp', 100, 1000, 'h1', 'ok'), ('impl.cpp', 'cpp', 100, 1000, 'h2', 'ok')");
    conn.exec("INSERT INTO nodes (node_type, kind, name, file_id, start_line, stable_key) VALUES "
              "('symbol', 'class', 'Base', 1, 1, 'Base::class'), "
              "('symbol', 'class', 'Impl', 2, 1, 'Impl::class')");
    conn.exec("INSERT INTO edges (src_id, dst_id, kind) VALUES (2, 1, 'inherits')");
    
    QueryCache cache(conn);
    
    SECTION("Unbounded: _meta present, not truncated") {
        JsonMutDoc pdoc;
        auto* p = pdoc.new_obj();
        pdoc.set_root(p);
        yyjson_mut_obj_add_str(pdoc.doc, p, "symbol", "Base");
        auto* immut = make_immut_params(pdoc);
        
        std::string result = tools::find_implementations(yyjson_doc_get_root(immut), conn, cache, ".");
        yyjson_doc_free(immut);
        
        REQUIRE(has_meta_block(result));
        REQUIRE_FALSE(get_meta_truncated(result));
        REQUIRE(result.find("Impl") != std::string::npos);
    }
    
    SECTION("With max_tokens: _meta present") {
        JsonMutDoc pdoc;
        auto* p = pdoc.new_obj();
        pdoc.set_root(p);
        yyjson_mut_obj_add_str(pdoc.doc, p, "symbol", "Base");
        yyjson_mut_obj_add_int(pdoc.doc, p, "max_tokens", 500);
        auto* immut = make_immut_params(pdoc);
        
        std::string result = tools::find_implementations(yyjson_doc_get_root(immut), conn, cache, ".");
        yyjson_doc_free(immut);
        
        REQUIRE(has_meta_block(result));
    }
}

TEST_CASE("callers_approx: v2-Tokens _meta block", "[v2-tokens]") {
    Connection conn(":memory:");
    schema::create_tables(conn);
    
    conn.exec("INSERT INTO files (path, language, size_bytes, mtime_ns, content_hash, parse_status) VALUES "
              "('a.cpp', 'cpp', 100, 1000, 'h1', 'ok')");
    conn.exec("INSERT INTO nodes (node_type, kind, name, file_id, start_line, stable_key) VALUES "
              "('symbol', 'function', 'callee', 1, 10, 'callee::function'), "
              "('symbol', 'function', 'caller', 1, 1, 'caller::function')");
    conn.exec("INSERT INTO edges (src_id, dst_id, kind, confidence) VALUES (2, 1, 'calls', 0.9)");
    
    QueryCache cache(conn);
    
    SECTION("_meta present") {
        JsonMutDoc pdoc;
        auto* p = pdoc.new_obj();
        pdoc.set_root(p);
        yyjson_mut_obj_add_int(pdoc.doc, p, "node_id", 1);
        auto* immut = make_immut_params(pdoc);
        
        std::string result = tools::callers_approx(yyjson_doc_get_root(immut), conn, cache, ".");
        yyjson_doc_free(immut);
        
        REQUIRE(has_meta_block(result));
        REQUIRE(result.find("caller") != std::string::npos);
    }
}

TEST_CASE("callees_approx: v2-Tokens _meta block", "[v2-tokens]") {
    Connection conn(":memory:");
    schema::create_tables(conn);
    
    conn.exec("INSERT INTO files (path, language, size_bytes, mtime_ns, content_hash, parse_status) VALUES "
              "('a.cpp', 'cpp', 100, 1000, 'h1', 'ok')");
    conn.exec("INSERT INTO nodes (node_type, kind, name, file_id, start_line, stable_key) VALUES "
              "('symbol', 'function', 'caller', 1, 1, 'caller::function'), "
              "('symbol', 'function', 'callee', 1, 10, 'callee::function')");
    conn.exec("INSERT INTO edges (src_id, dst_id, kind, confidence) VALUES (1, 2, 'calls', 0.9)");
    
    QueryCache cache(conn);
    
    SECTION("_meta present") {
        JsonMutDoc pdoc;
        auto* p = pdoc.new_obj();
        pdoc.set_root(p);
        yyjson_mut_obj_add_int(pdoc.doc, p, "node_id", 1);
        auto* immut = make_immut_params(pdoc);
        
        std::string result = tools::callees_approx(yyjson_doc_get_root(immut), conn, cache, ".");
        yyjson_doc_free(immut);
        
        REQUIRE(has_meta_block(result));
        REQUIRE(result.find("callee") != std::string::npos);
    }
}
