#pragma once

#include "core/arena.h"
#include "core/arena_pool.h"
#include "index/parser.h"
#include "index/extractor.h"
#include "util/hash.h"
#include <string>
#include <iostream>
#include <fstream>
#include <filesystem>

namespace codetopo {

struct ParseFileConfig {
    std::string file_path;
    int arena_size_mb = 1024;
    int parse_timeout_s = 0;
    int max_symbols = 50000;
    bool show_symbols = false;
    bool show_refs = false;
    bool show_edges = false;
};

inline int run_parse_file(const ParseFileConfig& cfg) {
    namespace fs = std::filesystem;

    auto path = fs::canonical(cfg.file_path);
    if (!fs::exists(path)) {
        std::cerr << "ERROR: File not found: " << path.string() << "\n";
        return 1;
    }

    // Detect language from extension
    auto ext = path.extension().string();
    std::string lang;
    if (ext == ".c" || ext == ".h") lang = "c";
    else if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".hpp" || ext == ".hxx") lang = "cpp";
    else if (ext == ".cs") lang = "csharp";
    else if (ext == ".ts" || ext == ".tsx") lang = "typescript";
    else if (ext == ".js" || ext == ".jsx" || ext == ".mjs") lang = "javascript";
    else if (ext == ".py") lang = "python";
    else if (ext == ".rs") lang = "rust";
    else if (ext == ".java") lang = "java";
    else if (ext == ".go") lang = "go";
    else if (ext == ".sh" || ext == ".bash") lang = "bash";
    else if (ext == ".sql") lang = "sql";
    else if (ext == ".yaml" || ext == ".yml") lang = "yaml";
    else {
        std::cerr << "ERROR: Unsupported file extension: " << ext << "\n";
        return 1;
    }

    // Read file
    std::string content = read_file_content(path);
    if (content.empty()) {
        std::cerr << "ERROR: Could not read file or file is empty\n";
        return 1;
    }

    std::cerr << "File: " << path.string() << "\n";
    std::cerr << "Language: " << lang << "\n";
    std::cerr << "Size: " << content.size() << " bytes\n";

    // Set up arena
    register_arena_allocator();
    ArenaPool pool(1, static_cast<size_t>(cfg.arena_size_mb) * 1024 * 1024);
    ArenaLease lease(pool);
    set_thread_arena(lease.get());

    // Parse
    Parser parser;
    if (!parser.set_language(lang)) {
        std::cerr << "ERROR: Language grammar not available for " << lang << "\n";
        return 1;
    }
    if (cfg.parse_timeout_s > 0) {
        parser.set_timeout(static_cast<uint64_t>(cfg.parse_timeout_s) * 1'000'000);
    }

    TreeGuard tree(parser.parse(content));
    if (!tree) {
        std::cerr << "ERROR: Parse failed (timeout or error)\n";
        return 1;
    }

    std::cerr << "Parse: OK\n";

    // Extract
    Extractor extractor(cfg.max_symbols, 200, cfg.parse_timeout_s);
    auto rel_path = path.filename().string();
    auto result = extractor.extract(tree.tree, content, lang, rel_path);

    std::cerr << "Symbols: " << result.symbols.size() << "\n";
    std::cerr << "Refs: " << result.refs.size() << "\n";
    std::cerr << "Edges: " << result.edges.size() << "\n";
    if (result.truncated) {
        std::cerr << "WARNING: Extraction truncated: " << result.truncation_reason << "\n";
    }

    if (cfg.show_symbols) {
        std::cerr << "\n--- Symbols ---\n";
        for (size_t i = 0; i < result.symbols.size(); ++i) {
            auto& s = result.symbols[i];
            std::cerr << "[" << i << "] " << s.kind << " " << s.name
                      << " L" << s.start_line << "-L" << s.end_line
                      << " key=" << s.stable_key << "\n";
        }
    }

    if (cfg.show_refs) {
        std::cerr << "\n--- Refs ---\n";
        for (size_t i = 0; i < result.refs.size(); ++i) {
            auto& r = result.refs[i];
            std::cerr << "[" << i << "] " << r.kind << " " << r.name
                      << " L" << r.start_line << "\n";
        }
    }

    if (cfg.show_edges) {
        std::cerr << "\n--- Edges ---\n";
        for (size_t i = 0; i < result.edges.size(); ++i) {
            auto& e = result.edges[i];
            std::cerr << "[" << i << "] " << e.kind << " src=" << e.src_index
                      << " dst=" << e.dst_index;
            if (!e.dst_name.empty()) std::cerr << " dst_name=" << e.dst_name;
            std::cerr << "\n";
        }
    }

    return 0;
}

} // namespace codetopo
