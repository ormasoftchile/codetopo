#pragma once

#include "index/parser.h"
#include "index/stable_key.h"
#include <tree_sitter/api.h>
#include <string>
#include <vector>
#include <cstdint>
#include <filesystem>
#include <chrono>

namespace codetopo {

// T031-T037: Symbol, edge, and ref extraction from Tree-sitter AST.

struct ExtractedSymbol {
    std::string kind;
    std::string name;
    std::string qualname;
    std::string signature;
    int start_line = 0;
    int start_col = 0;
    int end_line = 0;
    int end_col = 0;
    bool is_definition = true;
    std::string visibility;  // public, protected, private, or empty
    std::string doc;
    std::string stable_key;  // Filled in after extraction
};

struct ExtractedRef {
    std::string kind;     // call, type_ref, include, inherit, field_access, other
    std::string name;
    int start_line = 0;
    int start_col = 0;
    int end_line = 0;
    int end_col = 0;
    std::string evidence;
    int containing_symbol_index = -1;  // Index into symbols array of enclosing method/function
};

struct ExtractedEdge {
    int src_index = -1;  // Index into symbols array (-1 = file node)
    int dst_index = -1;  // Index into symbols array
    std::string dst_name; // For cross-file resolution (name to resolve later)
    std::string kind;     // calls, includes, inherits, references, contains
    double confidence = 1.0;
    std::string evidence;
};

struct ExtractionResult {
    std::vector<ExtractedSymbol> symbols;
    std::vector<ExtractedRef> refs;
    std::vector<ExtractedEdge> edges;
    bool truncated = false;
    std::string truncation_reason;
};

class Extractor {
public:
    Extractor(int max_symbols, int max_depth)
        : max_symbols_(max_symbols), max_depth_(max_depth) {}

    Extractor(int max_symbols, int max_depth, int timeout_s)
        : max_symbols_(max_symbols), max_depth_(max_depth),
          timeout_s_(timeout_s) {}

    Extractor(int max_symbols, int max_depth, int timeout_s, size_t* cancel_flag)
        : max_symbols_(max_symbols), max_depth_(max_depth),
          timeout_s_(timeout_s), cancel_flag_(cancel_flag) {}

    ExtractionResult extract(TSTree* tree, const std::string& source,
                              const std::string& language,
                              const std::string& rel_path);

private:
    int max_symbols_;
    int max_depth_;
    int symbol_count_ = 0;
    int node_count_ = 0;
    int timeout_s_ = 0;
    size_t* cancel_flag_ = nullptr;
    std::chrono::steady_clock::time_point deadline_;
    std::vector<int> symbol_stack_;// Stack of enclosing symbol indices
    const std::string* source_ = nullptr;
    ExtractionResult* result_ = nullptr;
    const std::string* rel_path_ = nullptr;
    const std::string* language_ = nullptr;
    LanguageId lang_id_ = LanguageId::Unknown;

    std::string node_text(TSNode node);
    std::string get_name_from_child(TSNode node, const char* field_name);
    std::string get_declarator_identifier(TSNode declarator_parent, const char* field_name);
    std::string find_identifier_recursive(TSNode node, int depth);
    std::string get_callee_name(TSNode func_node);
    std::string extract_leading_comment(TSNode node);

    void add_symbol(const std::string& kind, const std::string& name,
                    TSNode node, const std::string& qualname = "",
                    const std::string& signature = "",
                    const std::string& visibility = "");
    void add_ref(const std::string& kind, const std::string& name, TSNode node,
                 const std::string& evidence = "");
    void add_call_edge(int caller_idx, const std::string& callee_name, double confidence = 0.7);

    void visit_node(TSNode node, const std::string& parent_qualname, int depth);
    void extract_c_cpp(TSNode node, const std::string& type, const std::string& parent_qn);
    void extract_csharp(TSNode node, const std::string& type, const std::string& parent_qn);
    void extract_typescript(TSNode node, const std::string& type, const std::string& parent_qn);
    void extract_go(TSNode node, const std::string& type, const std::string& parent_qn);
    void extract_yaml(TSNode node, const std::string& type, const std::string& parent_qn);
    void extract_python(TSNode node, const std::string& type, const std::string& parent_qn);
    void extract_rust(TSNode node, const std::string& type, const std::string& parent_qn);
    void extract_java(TSNode node, const std::string& type, const std::string& parent_qn);
    void extract_bash(TSNode node, const std::string& type, const std::string& parent_qn);
    void extract_sql(TSNode node, const std::string& type, const std::string& parent_qn);
};

// Read file content from disk
std::string read_file_content(const std::filesystem::path& path);

} // namespace codetopo
