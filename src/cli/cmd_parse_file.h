#pragma once
// cmd_parse_file.h — Subprocess-safe single-file parse+extract.
//
// Called as: codetopo parse-file --lang cpp --root /repo < filepath
// Reads the file, parses with tree-sitter, extracts symbols/refs/edges,
// writes JSON result to stdout. If tree-sitter crashes, this process dies
// but the parent indexer survives and records the file in the crashlist.

#include "core/config.h"
#include "core/arena.h"
#include "index/parser.h"
#include "index/extractor.h"
#include "util/hash.h"
#include <yyjson.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <filesystem>

namespace codetopo {

// Forward declarations
void set_thread_arena(Arena* arena);
void register_arena_allocator();

inline int run_parse_file(const std::string& file_path,
                          const std::string& language,
                          const std::string& repo_root,
                          int max_symbols = 50000,
                          int max_depth = 500) {
    namespace fs = std::filesystem;

    register_arena_allocator();

    // Create a single arena for this process
    Arena arena(128 * 1024 * 1024); // 128 MB
    set_thread_arena(&arena);

    // Read file
    std::ifstream f(file_path, std::ios::binary);
    if (!f) {
        std::cout << R"({"status":"failed","error":"could not read file"})" << std::endl;
        return 1;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string content = ss.str();

    if (content.empty()) {
        std::cout << R"({"status":"failed","error":"empty file"})" << std::endl;
        return 1;
    }

    std::string content_hash = hash_string(content);

    // Compute relative path
    std::string rel_path = file_path;
    if (!repo_root.empty()) {
        auto abs = fs::canonical(file_path);
        auto root = fs::canonical(repo_root);
        rel_path = fs::relative(abs, root).string();
    }

    // Parse
    Parser parser;
    if (!parser.set_language(language)) {
        std::cout << R"({"status":"skipped","error":"language grammar not available"})" << std::endl;
        return 0;
    }

    // No safe_parse here — this IS the isolated process.
    // If tree-sitter crashes, the process dies and the parent handles it.
    auto tree = TreeGuard(parser.parse(content));
    if (!tree) {
        std::cout << R"({"status":"failed","error":"tree-sitter parse returned null"})" << std::endl;
        return 1;
    }

    // Extract
    Extractor extractor(max_symbols, max_depth);
    auto extraction = extractor.extract(tree.tree, content, language, rel_path);

    // Build JSON output
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root_val = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root_val);

    yyjson_mut_obj_add_str(doc, root_val, "status",
                           extraction.truncated ? "partial" : "ok");
    yyjson_mut_obj_add_str(doc, root_val, "content_hash", content_hash.c_str());
    if (extraction.truncated) {
        yyjson_mut_obj_add_str(doc, root_val, "error",
                               extraction.truncation_reason.c_str());
    }

    // Symbols array
    yyjson_mut_val* syms = yyjson_mut_arr(doc);
    for (auto& sym : extraction.symbols) {
        yyjson_mut_val* s = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, s, "kind", sym.kind.c_str());
        yyjson_mut_obj_add_str(doc, s, "name", sym.name.c_str());
        yyjson_mut_obj_add_str(doc, s, "qualname", sym.qualname.c_str());
        yyjson_mut_obj_add_str(doc, s, "signature", sym.signature.c_str());
        yyjson_mut_obj_add_int(doc, s, "start_line", sym.start_line);
        yyjson_mut_obj_add_int(doc, s, "start_col", sym.start_col);
        yyjson_mut_obj_add_int(doc, s, "end_line", sym.end_line);
        yyjson_mut_obj_add_int(doc, s, "end_col", sym.end_col);
        yyjson_mut_obj_add_bool(doc, s, "is_definition", sym.is_definition);
        yyjson_mut_obj_add_str(doc, s, "visibility", sym.visibility.c_str());
        yyjson_mut_obj_add_str(doc, s, "stable_key", sym.stable_key.c_str());
        yyjson_mut_arr_append(syms, s);
    }
    yyjson_mut_obj_add_val(doc, root_val, "symbols", syms);

    // Refs array
    yyjson_mut_val* refs = yyjson_mut_arr(doc);
    for (auto& ref : extraction.refs) {
        yyjson_mut_val* r = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, r, "kind", ref.kind.c_str());
        yyjson_mut_obj_add_str(doc, r, "name", ref.name.c_str());
        yyjson_mut_obj_add_int(doc, r, "start_line", ref.start_line);
        yyjson_mut_obj_add_int(doc, r, "start_col", ref.start_col);
        yyjson_mut_obj_add_str(doc, r, "evidence", ref.evidence.c_str());
        yyjson_mut_arr_append(refs, r);
    }
    yyjson_mut_obj_add_val(doc, root_val, "refs", refs);

    // Edges array
    yyjson_mut_val* edges = yyjson_mut_arr(doc);
    for (auto& edge : extraction.edges) {
        yyjson_mut_val* e = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_int(doc, e, "src_index", edge.src_index);
        yyjson_mut_obj_add_int(doc, e, "dst_index", edge.dst_index);
        yyjson_mut_obj_add_str(doc, e, "dst_name", edge.dst_name.c_str());
        yyjson_mut_obj_add_str(doc, e, "kind", edge.kind.c_str());
        yyjson_mut_obj_add_real(doc, e, "confidence", edge.confidence);
        yyjson_mut_arr_append(edges, e);
    }
    yyjson_mut_obj_add_val(doc, root_val, "edges", edges);

    // Write to stdout
    char* json = yyjson_mut_write(doc, 0, nullptr);
    if (json) {
        std::cout << json << std::endl;
        free(json);
    }
    yyjson_mut_doc_free(doc);
    return 0;
}

} // namespace codetopo
