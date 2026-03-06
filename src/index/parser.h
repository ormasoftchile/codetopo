#pragma once

#include <tree_sitter/api.h>
#include <string>
#include <unordered_map>
#include <memory>
#include <stdexcept>

namespace codetopo {

// T030: Tree-sitter parser wrapper with language grammar selection.

// Forward declarations of tree-sitter grammar entry points.
// These are provided by the tree-sitter grammar C libraries.
extern "C" {
    const TSLanguage* tree_sitter_c(void);
    const TSLanguage* tree_sitter_cpp(void);
    const TSLanguage* tree_sitter_c_sharp(void);
    const TSLanguage* tree_sitter_go(void);
    const TSLanguage* tree_sitter_yaml(void);
    // tree_sitter_typescript deferred — ABI incompatibility
}

class Parser {
public:
    Parser() : parser_(ts_parser_new()) {
        if (!parser_) throw std::runtime_error("Failed to create Tree-sitter parser");
    }

    ~Parser() {
        if (parser_) ts_parser_delete(parser_);
    }

    Parser(const Parser&) = delete;
    Parser& operator=(const Parser&) = delete;

    Parser(Parser&& other) noexcept : parser_(other.parser_) {
        other.parser_ = nullptr;
    }

    // Set language for the next parse.
    bool set_language(const std::string& lang) {
        const TSLanguage* ts_lang = get_language(lang);
        if (!ts_lang) return false;
        return ts_parser_set_language(parser_, ts_lang);
    }

    // Parse source code. Returns owned TSTree (caller must free).
    TSTree* parse(const std::string& source) {
        return ts_parser_parse_string(parser_, nullptr,
                                       source.c_str(),
                                       static_cast<uint32_t>(source.size()));
    }

    // Parse with existing tree (for incremental parsing).
    TSTree* parse_incremental(const std::string& source, TSTree* old_tree) {
        return ts_parser_parse_string(parser_, old_tree,
                                       source.c_str(),
                                       static_cast<uint32_t>(source.size()));
    }

    TSParser* raw() { return parser_; }

private:
    TSParser* parser_;

    static const TSLanguage* get_language(const std::string& lang) {
        if (lang == "c") return tree_sitter_c();
        if (lang == "cpp") return tree_sitter_cpp();
        if (lang == "csharp") return tree_sitter_c_sharp();
        if (lang == "typescript") return nullptr;  // Deferred — ABI incompatibility
        if (lang == "go") return tree_sitter_go();
        if (lang == "yaml") return tree_sitter_yaml();
        return nullptr;
    }
};

// RAII wrapper for TSTree
struct TreeGuard {
    TSTree* tree;
    explicit TreeGuard(TSTree* t) : tree(t) {}
    ~TreeGuard() { if (tree) ts_tree_delete(tree); }
    TreeGuard(const TreeGuard&) = delete;
    TreeGuard& operator=(const TreeGuard&) = delete;
    TreeGuard(TreeGuard&& o) noexcept : tree(o.tree) { o.tree = nullptr; }
    TSNode root() { return ts_tree_root_node(tree); }
    explicit operator bool() const { return tree != nullptr; }
};

} // namespace codetopo
