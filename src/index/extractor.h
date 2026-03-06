#pragma once

#include "index/parser.h"
#include "index/stable_key.h"
#include <tree_sitter/api.h>
#include <string>
#include <vector>
#include <cstdint>
#include <fstream>
#include <sstream>

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

    ExtractionResult extract(TSTree* tree, const std::string& source,
                              const std::string& language,
                              const std::string& rel_path) {
        ExtractionResult result;
        source_ = &source;
        result_ = &result;
        rel_path_ = &rel_path;
        language_ = &language;
        symbol_count_ = 0;

        TSNode root = ts_tree_root_node(tree);
        visit_node(root, "", 0);

        // Generate stable keys with collision handling
        std::vector<KeyCandidate> candidates;
        for (size_t i = 0; i < result.symbols.size(); ++i) {
            auto& sym = result.symbols[i];
            std::string base = make_stable_key(rel_path, sym.kind,
                                                sym.qualname.empty() ? sym.name : sym.qualname);
            candidates.push_back({base, sym.start_line});
        }
        auto keys = resolve_collisions(candidates);
        for (size_t i = 0; i < result.symbols.size(); ++i) {
            result.symbols[i].stable_key = keys[i];
        }

        // Add containment edges (file → top-level symbols)
        for (size_t i = 0; i < result.symbols.size(); ++i) {
            // Simplified: treat all symbols as contained by the file
            result.edges.push_back({-1, static_cast<int>(i), "", "contains", 1.0, "ast_containment"});
        }

        return result;
    }

private:
    int max_symbols_;
    int max_depth_;
    int symbol_count_ = 0;
    const std::string* source_ = nullptr;
    ExtractionResult* result_ = nullptr;
    const std::string* rel_path_ = nullptr;
    const std::string* language_ = nullptr;

    std::string node_text(TSNode node) {
        uint32_t start = ts_node_start_byte(node);
        uint32_t end = ts_node_end_byte(node);
        if (start >= source_->size() || end > source_->size() || start >= end)
            return "";
        return source_->substr(start, end - start);
    }

    std::string get_name_from_child(TSNode node, const char* field_name) {
        TSNode child = ts_node_child_by_field_name(node, field_name, static_cast<uint32_t>(strlen(field_name)));
        if (ts_node_is_null(child)) return "";
        return node_text(child);
    }

    // C/C++ specific: extract the clean identifier from a declarator node.
    // Tree-sitter declarator text can include pointer stars, references, arrays:
    //   "*myPtr"  "**ppObj"  "&ref"  "arr[10]"  "(*funcPtr)(int)"
    // We want just the identifier: "myPtr", "ppObj", "ref", "arr", "funcPtr".
    // Strategy: walk the declarator subtree to find the identifier child.
    std::string get_declarator_identifier(TSNode declarator_parent, const char* field_name) {
        TSNode decl = ts_node_child_by_field_name(declarator_parent, field_name,
                                                   static_cast<uint32_t>(strlen(field_name)));
        if (ts_node_is_null(decl)) return "";
        return find_identifier_recursive(decl, 3);
    }

    // Recursively walk a node to find the first identifier child.
    // Handles nested declarators like pointer_declarator → identifier.
    std::string find_identifier_recursive(TSNode node, int depth) {
        if (depth <= 0) return "";
        const char* type = ts_node_type(node);
        if (!type) return "";
        if (std::string(type) == "identifier" || std::string(type) == "field_identifier"
            || std::string(type) == "type_identifier") {
            return node_text(node);
        }
        uint32_t count = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < count; ++i) {
            auto result = find_identifier_recursive(ts_node_named_child(node, i), depth - 1);
            if (!result.empty()) return result;
        }
        return "";
    }

    // C/C++ specific: extract the callee name from a call_expression's function node.
    // Handles:  func(...)        → "func"
    //           obj.method(...)  → "method"
    //           obj->method(...) → "method"
    //           ns::func(...)    → "func"
    //           ptr->ns::func    → "func"
    // Strategy: walk the function subtree to find the rightmost identifier.
    std::string get_callee_name(TSNode func_node) {
        const char* type = ts_node_type(func_node);
        if (!type) return node_text(func_node);

        std::string t(type);
        if (t == "identifier") {
            return node_text(func_node);
        }
        if (t == "field_expression" || t == "member_expression") {
            // obj.method or obj->method — take the field/member name
            TSNode field = ts_node_child_by_field_name(func_node, "field",
                                                        static_cast<uint32_t>(strlen("field")));
            if (!ts_node_is_null(field)) return node_text(field);
            // Fallback: last named child
            uint32_t count = ts_node_named_child_count(func_node);
            if (count > 0) return node_text(ts_node_named_child(func_node, count - 1));
        }
        if (t == "qualified_identifier" || t == "scoped_identifier") {
            // ns::func — take the name part (last child)
            TSNode name = ts_node_child_by_field_name(func_node, "name",
                                                       static_cast<uint32_t>(strlen("name")));
            if (!ts_node_is_null(name)) return node_text(name);
            uint32_t count = ts_node_named_child_count(func_node);
            if (count > 0) return node_text(ts_node_named_child(func_node, count - 1));
        }
        if (t == "template_function") {
            // template_func<T>(...) — get the function name
            TSNode name = ts_node_child_by_field_name(func_node, "name",
                                                       static_cast<uint32_t>(strlen("name")));
            if (!ts_node_is_null(name)) return get_callee_name(name);
        }
        // Fallback: raw text
        return node_text(func_node);
    }

    void add_symbol(const std::string& kind, const std::string& name,
                    TSNode node, const std::string& qualname = "",
                    const std::string& signature = "",
                    const std::string& visibility = "") {
        if (name.empty()) return;
        if (++symbol_count_ > max_symbols_) {
            result_->truncated = true;
            result_->truncation_reason = "exceeded max_symbols_per_file";
            return;
        }

        TSPoint start = ts_node_start_point(node);
        TSPoint end = ts_node_end_point(node);

        result_->symbols.push_back({
            kind, name, qualname.empty() ? name : qualname,
            signature,
            static_cast<int>(start.row + 1), static_cast<int>(start.column),
            static_cast<int>(end.row + 1), static_cast<int>(end.column),
            true, visibility, "", ""
        });
    }

    void add_ref(const std::string& kind, const std::string& name, TSNode node,
                 const std::string& evidence = "") {
        TSPoint start = ts_node_start_point(node);
        TSPoint end = ts_node_end_point(node);

        result_->refs.push_back({
            kind, name,
            static_cast<int>(start.row + 1), static_cast<int>(start.column),
            static_cast<int>(end.row + 1), static_cast<int>(end.column),
            evidence
        });
    }

    void add_call_edge(int caller_idx, const std::string& callee_name, double confidence = 0.7) {
        if (confidence < 0.3) return;  // FR-044: confidence floor
        result_->edges.push_back({
            caller_idx, -1, callee_name, "calls", confidence, "name_match"
        });
    }

    void visit_node(TSNode node, const std::string& parent_qualname, int depth) {
        if (ts_node_is_null(node) || depth > max_depth_) return;
        if (result_->truncated) return;

        const char* type = ts_node_type(node);
        if (!type) return;

        std::string type_str(type);

        // C/C++ symbol extraction (T031)
        if (*language_ == "c" || *language_ == "cpp") {
            extract_c_cpp(node, type_str, parent_qualname);
        }
        // T032: C# extraction
        else if (*language_ == "csharp") {
            extract_csharp(node, type_str, parent_qualname);
        }
        // T033: TypeScript extraction
        else if (*language_ == "typescript") {
            extract_typescript(node, type_str, parent_qualname);
        }
        // T034: Go extraction
        else if (*language_ == "go") {
            extract_go(node, type_str, parent_qualname);
        }
        // T035: YAML extraction
        else if (*language_ == "yaml") {
            extract_yaml(node, type_str, parent_qualname);
        }

        // Recurse into children
        uint32_t child_count = ts_node_child_count(node);
        for (uint32_t i = 0; i < child_count; ++i) {
            visit_node(ts_node_child(node, i), parent_qualname, depth + 1);
        }
    }

    void extract_c_cpp(TSNode node, const std::string& type, const std::string& parent_qn) {
        if (type == "function_definition" || type == "function_declarator") {
            // Use tree-walking to find the identifier, not raw declarator text
            auto name = get_declarator_identifier(node, "declarator");
            if (name.empty()) name = get_name_from_child(node, "name");
            if (name.empty()) {
                // Walk direct children to find identifier
                uint32_t count = ts_node_named_child_count(node);
                for (uint32_t i = 0; i < count; ++i) {
                    TSNode child = ts_node_named_child(node, i);
                    const char* ctype = ts_node_type(child);
                    if (ctype && std::string(ctype) == "identifier") {
                        name = node_text(child);
                        break;
                    }
                }
            }
            if (!name.empty()) {
                add_symbol("function", name, node, parent_qn.empty() ? name : parent_qn + "::" + name);
            }
        }
        else if (type == "class_specifier" || type == "struct_specifier") {
            auto name = get_name_from_child(node, "name");
            std::string kind = (type == "class_specifier") ? "class" : "struct";
            if (!name.empty()) {
                add_symbol(kind, name, node, parent_qn.empty() ? name : parent_qn + "::" + name);
            }
        }
        else if (type == "enum_specifier") {
            auto name = get_name_from_child(node, "name");
            if (!name.empty()) {
                add_symbol("enum", name, node, parent_qn.empty() ? name : parent_qn + "::" + name);
            }
        }
        else if (type == "namespace_definition") {
            auto name = get_name_from_child(node, "name");
            if (!name.empty()) {
                add_symbol("namespace", name, node, parent_qn.empty() ? name : parent_qn + "::" + name);
            }
        }
        else if (type == "field_declaration") {
            auto name = get_declarator_identifier(node, "declarator");
            if (name.empty()) name = get_name_from_child(node, "declarator");
            if (!name.empty()) {
                add_symbol("field", name, node, parent_qn.empty() ? name : parent_qn + "::" + name);
            }
        }
        else if (type == "preproc_include") {
            auto path_node = ts_node_child(node, 1);  // The path string
            if (!ts_node_is_null(path_node)) {
                std::string inc_path = node_text(path_node);
                add_ref("include", inc_path, node, "preproc_include");
            }
        }
        else if (type == "call_expression") {
            auto func = ts_node_child(node, 0);  // Function being called
            if (!ts_node_is_null(func)) {
                std::string callee = get_callee_name(func);
                add_ref("call", callee, node, "call_expression");
            }
        }
        else if (type == "type_identifier") {
            std::string name = node_text(node);
            if (!name.empty()) {
                add_ref("type_ref", name, node, "type_identifier");
            }
        }
        else if (type == "base_class_clause") {
            // Inheritance
            uint32_t count = ts_node_named_child_count(node);
            for (uint32_t i = 0; i < count; ++i) {
                TSNode child = ts_node_named_child(node, i);
                std::string child_type(ts_node_type(child));
                if (child_type == "type_identifier" || child_type == "qualified_identifier") {
                    std::string base_name = node_text(child);
                    add_ref("inherit", base_name, node, "base_class_clause");
                }
            }
        }
        else if (type == "preproc_def") {
            auto name = get_name_from_child(node, "name");
            if (!name.empty()) {
                add_symbol("macro", name, node);
            }
        }
        else if (type == "type_definition") {
            auto name = get_name_from_child(node, "declarator");
            if (name.empty()) name = get_name_from_child(node, "name");
            if (!name.empty()) {
                add_symbol("typedef", name, node);
            }
        }
        else if (type == "declaration") {
            // Could be a variable or function declaration
            auto name = get_declarator_identifier(node, "declarator");
            if (name.empty()) {
                name = get_name_from_child(node, "declarator");
                // Strip pointer/ref syntax if the tree walk failed
                if (!name.empty() && (name[0] == '*' || name[0] == '&')) {
                    size_t pos = name.find_first_not_of("*& ");
                    if (pos != std::string::npos) name = name.substr(pos);
                }
            }
            if (!name.empty() && name.find('(') == std::string::npos) {
                add_symbol("variable", name, node, parent_qn.empty() ? name : parent_qn + "::" + name);
            }
        }
    }

    // T032: C# symbol extraction
    void extract_csharp(TSNode node, const std::string& type, const std::string& parent_qn) {
        if (type == "class_declaration") {
            auto name = get_name_from_child(node, "name");
            if (!name.empty()) add_symbol("class", name, node, parent_qn.empty() ? name : parent_qn + "." + name);
        }
        else if (type == "interface_declaration") {
            auto name = get_name_from_child(node, "name");
            if (!name.empty()) add_symbol("interface", name, node, parent_qn.empty() ? name : parent_qn + "." + name);
        }
        else if (type == "method_declaration") {
            auto name = get_name_from_child(node, "name");
            if (!name.empty()) add_symbol("method", name, node, parent_qn.empty() ? name : parent_qn + "." + name);
        }
        else if (type == "property_declaration") {
            auto name = get_name_from_child(node, "name");
            if (!name.empty()) add_symbol("property", name, node, parent_qn.empty() ? name : parent_qn + "." + name);
        }
        else if (type == "event_declaration") {
            auto name = get_name_from_child(node, "name");
            if (!name.empty()) add_symbol("event", name, node);
        }
        else if (type == "delegate_declaration") {
            auto name = get_name_from_child(node, "name");
            if (!name.empty()) add_symbol("delegate", name, node);
        }
        else if (type == "namespace_declaration") {
            auto name = get_name_from_child(node, "name");
            if (!name.empty()) add_symbol("namespace", name, node);
        }
        else if (type == "enum_declaration") {
            auto name = get_name_from_child(node, "name");
            if (!name.empty()) add_symbol("enum", name, node);
        }
        else if (type == "struct_declaration") {
            auto name = get_name_from_child(node, "name");
            if (!name.empty()) add_symbol("struct", name, node);
        }
        else if (type == "field_declaration") {
            auto name = get_name_from_child(node, "declarator");
            if (!name.empty()) add_symbol("field", name, node);
        }
        else if (type == "invocation_expression") {
            auto func = ts_node_child(node, 0);
            if (!ts_node_is_null(func)) add_ref("call", node_text(func), node, "invocation");
        }
    }

    // T033: TypeScript symbol extraction
    void extract_typescript(TSNode node, const std::string& type, const std::string& parent_qn) {
        if (type == "function_declaration") {
            auto name = get_name_from_child(node, "name");
            if (!name.empty()) add_symbol("function", name, node, parent_qn.empty() ? name : parent_qn + "." + name);
        }
        else if (type == "class_declaration") {
            auto name = get_name_from_child(node, "name");
            if (!name.empty()) add_symbol("class", name, node);
        }
        else if (type == "interface_declaration") {
            auto name = get_name_from_child(node, "name");
            if (!name.empty()) add_symbol("interface", name, node);
        }
        else if (type == "method_definition") {
            auto name = get_name_from_child(node, "name");
            if (!name.empty()) add_symbol("method", name, node);
        }
        else if (type == "enum_declaration") {
            auto name = get_name_from_child(node, "name");
            if (!name.empty()) add_symbol("enum", name, node);
        }
        else if (type == "type_alias_declaration") {
            auto name = get_name_from_child(node, "name");
            if (!name.empty()) add_symbol("type_alias", name, node);
        }
        else if (type == "variable_declarator") {
            auto name = get_name_from_child(node, "name");
            if (!name.empty()) add_symbol("variable", name, node);
        }
        else if (type == "call_expression") {
            auto func = ts_node_child(node, 0);
            if (!ts_node_is_null(func)) add_ref("call", node_text(func), node, "call_expression");
        }
        else if (type == "import_statement") {
            add_ref("include", node_text(node), node, "import");
        }
    }

    // T034: Go symbol extraction
    void extract_go(TSNode node, const std::string& type, const std::string& parent_qn) {
        if (type == "function_declaration") {
            auto name = get_name_from_child(node, "name");
            if (!name.empty()) add_symbol("function", name, node);
        }
        else if (type == "method_declaration") {
            auto name = get_name_from_child(node, "name");
            if (!name.empty()) add_symbol("method", name, node);
        }
        else if (type == "type_declaration") {
            // Could be struct, interface, or type alias
            uint32_t count = ts_node_named_child_count(node);
            for (uint32_t i = 0; i < count; ++i) {
                TSNode child = ts_node_named_child(node, i);
                std::string child_type(ts_node_type(child));
                if (child_type == "type_spec") {
                    auto name = get_name_from_child(child, "name");
                    auto type_node = ts_node_child_by_field_name(child, "type", 4);
                    if (!name.empty()) {
                        if (!ts_node_is_null(type_node)) {
                            std::string tt(ts_node_type(type_node));
                            if (tt == "struct_type") add_symbol("struct", name, child);
                            else if (tt == "interface_type") add_symbol("interface", name, child);
                            else add_symbol("type_alias", name, child);
                        } else {
                            add_symbol("type_alias", name, child);
                        }
                    }
                }
            }
        }
        else if (type == "package_clause") {
            auto name = get_name_from_child(node, "name");
            if (name.empty()) {
                // Try direct child
                uint32_t count = ts_node_named_child_count(node);
                for (uint32_t i = 0; i < count; ++i) {
                    TSNode child = ts_node_named_child(node, i);
                    if (std::string(ts_node_type(child)) == "package_identifier") {
                        name = node_text(child);
                        break;
                    }
                }
            }
            if (!name.empty()) add_symbol("package", name, node);
        }
        else if (type == "call_expression") {
            auto func = ts_node_child(node, 0);
            if (!ts_node_is_null(func)) add_ref("call", node_text(func), node, "call");
        }
        else if (type == "import_declaration") {
            add_ref("include", node_text(node), node, "import");
        }
    }

    // T035: YAML symbol extraction (mapping keys)
    void extract_yaml(TSNode node, const std::string& type, const std::string& parent_qn) {
        if (type == "block_mapping_pair") {
            auto key_node = ts_node_child_by_field_name(node, "key", 3);
            if (!ts_node_is_null(key_node)) {
                std::string key = node_text(key_node);
                if (!key.empty()) add_symbol("mapping_key", key, node, parent_qn.empty() ? key : parent_qn + "." + key);
            }
        }
        else if (type == "flow_mapping") {
            // Flow mapping pairs
            uint32_t count = ts_node_named_child_count(node);
            for (uint32_t i = 0; i < count; ++i) {
                TSNode child = ts_node_named_child(node, i);
                if (std::string(ts_node_type(child)) == "flow_pair") {
                    auto key_node = ts_node_child_by_field_name(child, "key", 3);
                    if (!ts_node_is_null(key_node)) {
                        std::string key = node_text(key_node);
                        if (!key.empty()) add_symbol("mapping_key", key, child);
                    }
                }
            }
        }
    }
};

// Read file content from disk
inline std::string read_file_content(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace codetopo
