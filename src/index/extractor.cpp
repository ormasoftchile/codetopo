#include "index/extractor.h"
#include <cstring>
#include <fstream>
#include <sstream>
#include <filesystem>

namespace codetopo {

// --- Extractor public ---

ExtractionResult Extractor::extract(TSTree* tree, const std::string& source,
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
        result.edges.push_back({-1, static_cast<int>(i), "", "contains", 1.0, "ast_containment"});
    }

    return result;
}

// --- Extractor private helpers ---

std::string Extractor::node_text(TSNode node) {
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    if (start >= source_->size() || end > source_->size() || start >= end)
        return "";
    return source_->substr(start, end - start);
}

std::string Extractor::get_name_from_child(TSNode node, const char* field_name) {
    TSNode child = ts_node_child_by_field_name(node, field_name, static_cast<uint32_t>(strlen(field_name)));
    if (ts_node_is_null(child)) return "";
    return node_text(child);
}

std::string Extractor::get_declarator_identifier(TSNode declarator_parent, const char* field_name) {
    TSNode decl = ts_node_child_by_field_name(declarator_parent, field_name,
                                               static_cast<uint32_t>(strlen(field_name)));
    if (ts_node_is_null(decl)) return "";
    return find_identifier_recursive(decl, 3);
}

std::string Extractor::find_identifier_recursive(TSNode node, int depth) {
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

std::string Extractor::get_callee_name(TSNode func_node) {
    const char* type = ts_node_type(func_node);
    if (!type) return node_text(func_node);

    std::string t(type);
    if (t == "identifier") {
        return node_text(func_node);
    }
    if (t == "field_expression" || t == "member_expression") {
        TSNode field = ts_node_child_by_field_name(func_node, "field",
                                                    static_cast<uint32_t>(strlen("field")));
        if (!ts_node_is_null(field)) return node_text(field);
        uint32_t count = ts_node_named_child_count(func_node);
        if (count > 0) return node_text(ts_node_named_child(func_node, count - 1));
    }
    if (t == "qualified_identifier" || t == "scoped_identifier") {
        TSNode name = ts_node_child_by_field_name(func_node, "name",
                                                   static_cast<uint32_t>(strlen("name")));
        if (!ts_node_is_null(name)) return node_text(name);
        uint32_t count = ts_node_named_child_count(func_node);
        if (count > 0) return node_text(ts_node_named_child(func_node, count - 1));
    }
    if (t == "template_function") {
        TSNode name = ts_node_child_by_field_name(func_node, "name",
                                                   static_cast<uint32_t>(strlen("name")));
        if (!ts_node_is_null(name)) return get_callee_name(name);
    }
    return node_text(func_node);
}

std::string Extractor::extract_leading_comment(TSNode node) {
    std::string comment;
    TSNode prev = ts_node_prev_sibling(node);
    std::vector<std::string> parts;
    while (!ts_node_is_null(prev)) {
        const char* prev_type = ts_node_type(prev);
        if (!prev_type) break;
        std::string pt(prev_type);
        if (pt == "comment" || pt == "line_comment" || pt == "block_comment"
            || pt == "documentation_comment" || pt == "doc_comment") {
            std::string text = node_text(prev);
            if (text.size() > 2) {
                if (text.substr(0, 3) == "///") text = text.substr(3);
                else if (text.substr(0, 2) == "//") text = text.substr(2);
                else if (text.size() > 4 && text.substr(0, 2) == "/*"
                         && text.substr(text.size()-2) == "*/")
                    text = text.substr(2, text.size()-4);
                auto pos = text.find_first_not_of(" \t");
                if (pos != std::string::npos) text = text.substr(pos);
            }
            if (!text.empty()) parts.push_back(text);
            prev = ts_node_prev_sibling(prev);
        } else {
            break;
        }
    }
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (!comment.empty()) comment += "\n";
        comment += *it;
    }
    if (comment.size() > 500) comment = comment.substr(0, 500);
    return comment;
}

void Extractor::add_symbol(const std::string& kind, const std::string& name,
                            TSNode node, const std::string& qualname,
                            const std::string& signature,
                            const std::string& visibility) {
    if (name.empty()) return;
    if (++symbol_count_ > max_symbols_) {
        result_->truncated = true;
        result_->truncation_reason = "exceeded max_symbols_per_file";
        return;
    }

    TSPoint start = ts_node_start_point(node);
    TSPoint end = ts_node_end_point(node);

    std::string doc_comment = extract_leading_comment(node);

    result_->symbols.push_back({
        kind, name, qualname.empty() ? name : qualname,
        signature,
        static_cast<int>(start.row + 1), static_cast<int>(start.column),
        static_cast<int>(end.row + 1), static_cast<int>(end.column),
        true, visibility, doc_comment, ""
    });
}

void Extractor::add_ref(const std::string& kind, const std::string& name, TSNode node,
                         const std::string& evidence) {
    TSPoint start = ts_node_start_point(node);
    TSPoint end = ts_node_end_point(node);

    result_->refs.push_back({
        kind, name,
        static_cast<int>(start.row + 1), static_cast<int>(start.column),
        static_cast<int>(end.row + 1), static_cast<int>(end.column),
        evidence
    });
}

void Extractor::add_call_edge(int caller_idx, const std::string& callee_name, double confidence) {
    if (confidence < 0.3) return;  // FR-044: confidence floor
    result_->edges.push_back({
        caller_idx, -1, callee_name, "calls", confidence, "name_match"
    });
}

void Extractor::visit_node(TSNode node, const std::string& parent_qualname, int depth) {
    if (ts_node_is_null(node) || depth > max_depth_) return;
    if (result_->truncated) return;

    const char* type = ts_node_type(node);
    if (!type) return;

    std::string type_str(type);

    if (*language_ == "c" || *language_ == "cpp") {
        extract_c_cpp(node, type_str, parent_qualname);
    }
    else if (*language_ == "csharp") {
        extract_csharp(node, type_str, parent_qualname);
    }
    else if (*language_ == "typescript" || *language_ == "javascript") {
        extract_typescript(node, type_str, parent_qualname);
    }
    else if (*language_ == "go") {
        extract_go(node, type_str, parent_qualname);
    }
    else if (*language_ == "yaml") {
        extract_yaml(node, type_str, parent_qualname);
    }
    else if (*language_ == "python") {
        extract_python(node, type_str, parent_qualname);
    }
    else if (*language_ == "rust") {
        extract_rust(node, type_str, parent_qualname);
    }
    else if (*language_ == "java") {
        extract_java(node, type_str, parent_qualname);
    }
    else if (*language_ == "bash") {
        extract_bash(node, type_str, parent_qualname);
    }
    else if (*language_ == "sql") {
        extract_sql(node, type_str, parent_qualname);
    }

    uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; ++i) {
        visit_node(ts_node_child(node, i), parent_qualname, depth + 1);
    }
}

void Extractor::extract_c_cpp(TSNode node, const std::string& type, const std::string& parent_qn) {
    if (type == "function_definition" || type == "function_declarator") {
        auto name = get_declarator_identifier(node, "declarator");
        if (name.empty()) name = get_name_from_child(node, "name");
        if (name.empty()) {
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
        auto path_node = ts_node_child(node, 1);
        if (!ts_node_is_null(path_node)) {
            std::string inc_path = node_text(path_node);
            add_ref("include", inc_path, node, "preproc_include");
        }
    }
    else if (type == "call_expression") {
        auto func = ts_node_child(node, 0);
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
        auto name = get_declarator_identifier(node, "declarator");
        if (name.empty()) {
            name = get_name_from_child(node, "declarator");
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

void Extractor::extract_csharp(TSNode node, const std::string& type, const std::string& parent_qn) {
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

void Extractor::extract_typescript(TSNode node, const std::string& type, const std::string& parent_qn) {
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

void Extractor::extract_go(TSNode node, const std::string& type, const std::string& parent_qn) {
    if (type == "function_declaration") {
        auto name = get_name_from_child(node, "name");
        if (!name.empty()) add_symbol("function", name, node);
    }
    else if (type == "method_declaration") {
        auto name = get_name_from_child(node, "name");
        if (!name.empty()) add_symbol("method", name, node);
    }
    else if (type == "type_declaration") {
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

void Extractor::extract_yaml(TSNode node, const std::string& type, const std::string& parent_qn) {
    if (type == "block_mapping_pair") {
        auto key_node = ts_node_child_by_field_name(node, "key", 3);
        if (!ts_node_is_null(key_node)) {
            std::string key = node_text(key_node);
            if (!key.empty()) add_symbol("mapping_key", key, node, parent_qn.empty() ? key : parent_qn + "." + key);
        }
    }
    else if (type == "flow_mapping") {
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

void Extractor::extract_python(TSNode node, const std::string& type, const std::string& parent_qn) {
    if (type == "function_definition") {
        auto name = get_name_from_child(node, "name");
        if (!name.empty()) add_symbol("function", name, node, parent_qn.empty() ? name : parent_qn + "." + name);
    }
    else if (type == "class_definition") {
        auto name = get_name_from_child(node, "name");
        if (!name.empty()) add_symbol("class", name, node);
    }
    else if (type == "decorated_definition") {
        // Decorated functions/classes — recurse into the definition child
    }
    else if (type == "assignment") {
        auto left = ts_node_child_by_field_name(node, "left", 4);
        if (!ts_node_is_null(left)) {
            std::string lt(ts_node_type(left));
            if (lt == "identifier") {
                auto name = node_text(left);
                if (!name.empty()) add_symbol("variable", name, node);
            }
        }
    }
    else if (type == "call") {
        auto func = ts_node_child_by_field_name(node, "function", 8);
        if (!ts_node_is_null(func)) add_ref("call", node_text(func), node, "call");
    }
    else if (type == "import_statement" || type == "import_from_statement") {
        add_ref("include", node_text(node), node, "import");
    }
}

void Extractor::extract_rust(TSNode node, const std::string& type, const std::string& parent_qn) {
    if (type == "function_item") {
        auto name = get_name_from_child(node, "name");
        if (!name.empty()) add_symbol("function", name, node, parent_qn.empty() ? name : parent_qn + "::" + name);
    }
    else if (type == "struct_item") {
        auto name = get_name_from_child(node, "name");
        if (!name.empty()) add_symbol("struct", name, node);
    }
    else if (type == "enum_item") {
        auto name = get_name_from_child(node, "name");
        if (!name.empty()) add_symbol("enum", name, node);
    }
    else if (type == "impl_item") {
        auto name = get_name_from_child(node, "type");
        if (!name.empty()) add_symbol("impl", name, node);
    }
    else if (type == "trait_item") {
        auto name = get_name_from_child(node, "name");
        if (!name.empty()) add_symbol("trait", name, node);
    }
    else if (type == "mod_item") {
        auto name = get_name_from_child(node, "name");
        if (!name.empty()) add_symbol("module", name, node);
    }
    else if (type == "type_item") {
        auto name = get_name_from_child(node, "name");
        if (!name.empty()) add_symbol("type_alias", name, node);
    }
    else if (type == "const_item" || type == "static_item") {
        auto name = get_name_from_child(node, "name");
        if (!name.empty()) add_symbol("variable", name, node);
    }
    else if (type == "macro_definition") {
        auto name = get_name_from_child(node, "name");
        if (!name.empty()) add_symbol("macro", name, node);
    }
    else if (type == "call_expression") {
        auto func = ts_node_child_by_field_name(node, "function", 8);
        if (!ts_node_is_null(func)) add_ref("call", node_text(func), node, "call");
    }
    else if (type == "use_declaration") {
        add_ref("include", node_text(node), node, "use");
    }
}

void Extractor::extract_java(TSNode node, const std::string& type, const std::string& parent_qn) {
    if (type == "class_declaration") {
        auto name = get_name_from_child(node, "name");
        if (!name.empty()) add_symbol("class", name, node);
    }
    else if (type == "interface_declaration") {
        auto name = get_name_from_child(node, "name");
        if (!name.empty()) add_symbol("interface", name, node);
    }
    else if (type == "method_declaration") {
        auto name = get_name_from_child(node, "name");
        if (!name.empty()) add_symbol("method", name, node, parent_qn.empty() ? name : parent_qn + "." + name);
    }
    else if (type == "constructor_declaration") {
        auto name = get_name_from_child(node, "name");
        if (!name.empty()) add_symbol("function", name, node);
    }
    else if (type == "enum_declaration") {
        auto name = get_name_from_child(node, "name");
        if (!name.empty()) add_symbol("enum", name, node);
    }
    else if (type == "field_declaration") {
        uint32_t count = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < count; ++i) {
            TSNode child = ts_node_named_child(node, i);
            if (std::string(ts_node_type(child)) == "variable_declarator") {
                auto name = get_name_from_child(child, "name");
                if (!name.empty()) add_symbol("field", name, child);
            }
        }
    }
    else if (type == "annotation_type_declaration") {
        auto name = get_name_from_child(node, "name");
        if (!name.empty()) add_symbol("annotation", name, node);
    }
    else if (type == "method_invocation") {
        auto name = get_name_from_child(node, "name");
        if (!name.empty()) add_ref("call", name, node, "call");
    }
    else if (type == "import_declaration") {
        add_ref("include", node_text(node), node, "import");
    }
}

void Extractor::extract_bash(TSNode node, const std::string& type, const std::string& parent_qn) {
    if (type == "function_definition") {
        auto name = get_name_from_child(node, "name");
        if (!name.empty()) add_symbol("function", name, node);
    }
    else if (type == "variable_assignment") {
        auto name = get_name_from_child(node, "name");
        if (!name.empty()) add_symbol("variable", name, node);
    }
    else if (type == "command") {
        auto name_node = ts_node_child(node, 0);
        if (!ts_node_is_null(name_node)) {
            std::string cmd = node_text(name_node);
            if (!cmd.empty() && cmd != "echo" && cmd != "cd" && cmd != "exit")
                add_ref("call", cmd, node, "command");
        }
    }
    else if (type == "source_command") {
        add_ref("include", node_text(node), node, "source");
    }
}

void Extractor::extract_sql(TSNode node, const std::string& type, const std::string& parent_qn) {
    if (type == "create_function_statement") {
        auto name = get_name_from_child(node, "name");
        if (!name.empty()) add_symbol("function", name, node);
    }
    else if (type == "create_table_statement") {
        auto name = get_name_from_child(node, "name");
        if (!name.empty()) add_symbol("class", name, node);
    }
    else if (type == "create_view_statement") {
        auto name = get_name_from_child(node, "name");
        if (!name.empty()) add_symbol("class", name, node);
    }
    else if (type == "create_procedure_statement" || type == "create_or_replace_procedure_statement") {
        auto name = get_name_from_child(node, "name");
        if (!name.empty()) add_symbol("function", name, node);
    }
    else if (type == "create_trigger_statement") {
        auto name = get_name_from_child(node, "name");
        if (!name.empty()) add_symbol("function", name, node);
    }
    else if (type == "create_index_statement") {
        auto name = get_name_from_child(node, "name");
        if (!name.empty()) add_symbol("variable", name, node);
    }
    else if (type == "create_type_statement") {
        auto name = get_name_from_child(node, "name");
        if (!name.empty()) add_symbol("typedef", name, node);
    }
    else if (type == "function_call") {
        auto name = get_name_from_child(node, "name");
        if (!name.empty()) add_ref("call", name, node, "call");
    }
}

// Free function
std::string read_file_content(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace codetopo
