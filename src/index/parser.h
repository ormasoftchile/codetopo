#pragma once

#include <tree_sitter/api.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string>
#include <unordered_map>
#include <memory>
#include <stdexcept>
#include <mutex>

namespace codetopo {

// T030: Tree-sitter parser wrapper with language grammar selection.

// No-op stubs used to replace NULL external scanner function pointers.
// tree-sitter v0.25.10 (vcpkg) calls external_scanner.deserialize
// unconditionally for grammars that enter an external-lex state, and the
// optimizer may eliminate the null check even for grammars with
// external_token_count=0. Patching with no-ops prevents the NULL call.
static inline void*    ts_noop_create()                                        { return nullptr; }
static inline void     ts_noop_destroy(void*)                                  {}
static inline bool     ts_noop_scan(void*, void*, const bool*)                 { return false; }
static inline unsigned ts_noop_serialize(void*, char*)                         { return 0; }
static inline void     ts_noop_deserialize(void*, const char*, unsigned)       {}

// Mirror of the TSLanguage struct layout (from tree-sitter's internal parser.h).
// Used only to patch NULL external-scanner function pointers at runtime.
// If tree-sitter's struct layout changes, this must be updated to match.
struct TSLanguageLayout {
    uint32_t abi_version;
    uint32_t symbol_count;
    uint32_t alias_count;
    uint32_t token_count;
    uint32_t external_token_count;
    uint32_t state_count;
    uint32_t large_state_count;
    uint32_t production_id_count;
    uint32_t field_count;
    uint16_t max_alias_sequence_length;
    // pointers (8 bytes each on 64-bit)
    const void *parse_table;
    const void *small_parse_table;
    const void *small_parse_table_map;
    const void *parse_actions;
    const void *symbol_names;
    const void *field_names;
    const void *field_map_slices;
    const void *field_map_entries;
    const void *symbol_metadata;
    const void *public_symbol_map;
    const void *alias_map;
    const void *alias_sequences;
    const void *lex_modes;
    void       *lex_fn;
    void       *keyword_lex_fn;
    uint16_t    keyword_capture_token;
    // padding (6 bytes to align external_scanner to 8-byte boundary)
    uint8_t     _pad[6];
    struct {
        const void *states;
        const void *symbol_map;
        void *(*create)(void);
        void  (*destroy)(void *);
        bool  (*scan)(void *, void *, const bool *);
        unsigned (*serialize)(void *, char *);
        void  (*deserialize)(void *, const char *, unsigned);
    } external_scanner;
    // remaining fields not needed for this patch
};

// Return a patched TSLanguage* with no-op stubs for any NULL external-scanner
// function pointers. The patched struct is cached per original language pointer
// (one copy per grammar per process lifetime).
inline const TSLanguage* patch_language_if_needed(const TSLanguage* lang) {
    if (!lang) return lang;

    // Quick check via raw layout: external_token_count is at offset 0x10.
    // If non-zero the grammar has a real external scanner — assume it's correct.
    const auto* layout = reinterpret_cast<const TSLanguageLayout*>(lang);
    if (layout->external_token_count > 0) return lang;

    // Grammar has no external scanner — all function pointers should be NULL.
    // If any are non-null the grammar is correctly set up; if all are null we
    // need to patch them to prevent tree-sitter from calling through NULL.
    if (layout->external_scanner.deserialize != nullptr &&
        layout->external_scanner.scan        != nullptr) {
        return lang;  // already set up correctly
    }

    // Cache: one patched copy per original language pointer.
    static std::mutex s_mutex;
    static std::unordered_map<const TSLanguage*, TSLanguageLayout> s_cache;

    std::lock_guard<std::mutex> lk(s_mutex);
    auto it = s_cache.find(lang);
    if (it != s_cache.end()) {
        return reinterpret_cast<const TSLanguage*>(&it->second);
    }

    // Create patched copy with no-op stubs.
    TSLanguageLayout patched = *layout;
    if (!patched.external_scanner.create)     patched.external_scanner.create     = ts_noop_create;
    if (!patched.external_scanner.destroy)    patched.external_scanner.destroy    = ts_noop_destroy;
    if (!patched.external_scanner.scan)       patched.external_scanner.scan       = ts_noop_scan;
    if (!patched.external_scanner.serialize)  patched.external_scanner.serialize  = ts_noop_serialize;
    if (!patched.external_scanner.deserialize)patched.external_scanner.deserialize= ts_noop_deserialize;
    if (!patched.external_scanner.states)     patched.external_scanner.states     =
        reinterpret_cast<const void*>(&patched.external_scanner.states); // non-null sentinel
    s_cache[lang] = patched;
    return reinterpret_cast<const TSLanguage*>(&s_cache[lang]);
}


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
    const TSLanguage* tree_sitter_powershell(void);
    const TSLanguage* tree_sitter_batch(void);
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
        // Patch NULL external-scanner function pointers with no-ops.
        // tree-sitter v0.25.10 calls external_scanner.deserialize unconditionally
        // for grammars with external_token_count==0 (Go, Java, etc.) without a
        // null check, causing SIGSEGV. Replacing NULLs with no-op stubs prevents
        // the crash without modifying the tree-sitter library.
        const TSLanguage* safe_lang = patch_language_if_needed(ts_lang);
        return ts_parser_set_language(parser_, safe_lang);
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
    // Returns nullptr if timeout expires or parse was cancelled.
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
        if (lang == "powershell") return tree_sitter_powershell();
        if (lang == "batch") return tree_sitter_batch();
        if (lang == "sql") return nullptr; // Deferred — grammar MSVC issues
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
    TreeGuard& operator=(TreeGuard&& o) noexcept {
        if (this != &o) { if (tree) ts_tree_delete(tree); tree = o.tree; o.tree = nullptr; }
        return *this;
    }
    TSNode root() { return ts_tree_root_node(tree); }
    explicit operator bool() const { return tree != nullptr; }
};

} // namespace codetopo
