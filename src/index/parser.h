#pragma once

#include <tree_sitter/api.h>
#include <string>
#include <unordered_map>
#include <memory>
#include <stdexcept>
#include "index/language_id.h"

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
    const TSLanguage* tree_sitter_typescript(void);
    const TSLanguage* tree_sitter_javascript(void);
    const TSLanguage* tree_sitter_python(void);
    const TSLanguage* tree_sitter_rust(void);
    const TSLanguage* tree_sitter_java(void);
    const TSLanguage* tree_sitter_bash(void);
    // tree_sitter_sql deferred — grammar has MSVC compilation issues
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

    // Set language via LanguageId enum (avoids string comparisons).
    bool set_language(LanguageId id) {
        const TSLanguage* ts_lang = get_language_by_id(id);
        if (!ts_lang) return false;
        return ts_parser_set_language(parser_, ts_lang);
    }

    // Set per-parse timeout. 0 = no limit.
    void set_timeout(uint64_t micros) {
        ts_parser_set_timeout_micros(parser_, micros);
    }

    // Set cancellation flag pointer. Tree-sitter checks this frequently
    // during parsing. Set *flag to non-zero from another thread to
    // hard-cancel the current parse (returns nullptr).
    void set_cancellation_flag(const size_t* flag) {
        ts_parser_set_cancellation_flag(parser_, flag);
    }

    // Parse source code. Returns owned TSTree (caller must free).
    // Returns nullptr if timeout expires.
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
        if (lang == "typescript") return tree_sitter_typescript();
        if (lang == "javascript") return tree_sitter_javascript();
        if (lang == "python") return tree_sitter_python();
        if (lang == "rust") return tree_sitter_rust();
        if (lang == "java") return tree_sitter_java();
        if (lang == "bash") return tree_sitter_bash();
        if (lang == "sql") return nullptr; // Deferred — grammar MSVC issues
        if (lang == "go") return tree_sitter_go();
        if (lang == "yaml") return tree_sitter_yaml();
        return nullptr;
    }

    static const TSLanguage* get_language_by_id(LanguageId id) {
        switch (id) {
            case LanguageId::C:          return tree_sitter_c();
            case LanguageId::Cpp:        return tree_sitter_cpp();
            case LanguageId::CSharp:     return tree_sitter_c_sharp();
            case LanguageId::TypeScript: return tree_sitter_typescript();
            case LanguageId::JavaScript: return tree_sitter_javascript();
            case LanguageId::Python:     return tree_sitter_python();
            case LanguageId::Rust:       return tree_sitter_rust();
            case LanguageId::Java:       return tree_sitter_java();
            case LanguageId::Bash:       return tree_sitter_bash();
            case LanguageId::Sql:        return nullptr;
            case LanguageId::Go:         return tree_sitter_go();
            case LanguageId::Yaml:       return tree_sitter_yaml();
            default:                     return nullptr;
        }
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
    TreeGuard& operator=(TreeGuard&& o) noexcept {
        if (this != &o) { if (tree) ts_tree_delete(tree); tree = o.tree; o.tree = nullptr; }
        return *this;
    }
    TSNode root() { return ts_tree_root_node(tree); }
    explicit operator bool() const { return tree != nullptr; }
};

} // namespace codetopo
