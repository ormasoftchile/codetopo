#include "index/extractor.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace codetopo {

namespace {

std::string source_text(TSNode node, const std::string& source) {
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    if (start >= source.size() || end > source.size() || start >= end) return "";
    return source.substr(start, end - start);
}

bool is_js_function_like(const std::string& type) {
    return type == "function_expression" || type == "arrow_function";
}

bool is_nested_function_boundary(const std::string& type) {
    return type == "function_expression" || type == "arrow_function" ||
           type == "function_declaration" || type == "generator_function" ||
           type == "generator_function_declaration" || type == "method_definition";
}

bool starts_with_uppercase_ascii(const std::string& name) {
    return !name.empty() && std::isupper(static_cast<unsigned char>(name[0])) != 0;
}

std::string trim_copy(std::string s) {
    size_t first = 0;
    while (first < s.size() && std::isspace(static_cast<unsigned char>(s[first]))) ++first;
    size_t last = s.size();
    while (last > first && std::isspace(static_cast<unsigned char>(s[last - 1]))) --last;
    return s.substr(first, last - first);
}

bool is_identifier_char(char c) {
    unsigned char u = static_cast<unsigned char>(c);
    return std::isalnum(u) || c == '_' || c == '$';
}

bool is_simple_identifier(const std::string& text) {
    if (text.empty()) return false;
    unsigned char first = static_cast<unsigned char>(text[0]);
    if (!(std::isalpha(first) || text[0] == '_' || text[0] == '$')) return false;
    for (char c : text) {
        if (!is_identifier_char(c)) return false;
    }
    return true;
}

std::string normalize_type_hint(std::string type) {
    type = trim_copy(type);
    while (!type.empty() && (type.back() == ';' || type.back() == ',' || type.back() == '=' ||
                             type.back() == ')' || std::isspace(static_cast<unsigned char>(type.back())))) {
        type.pop_back();
    }
    while (!type.empty() && (type.front() == '*' || type.front() == '&' ||
                             std::isspace(static_cast<unsigned char>(type.front())))) {
        type.erase(type.begin());
    }
    size_t generic = type.find_first_of("<[");
    if (generic != std::string::npos) type = type.substr(0, generic);
    size_t qualifier = type.find_last_of(".:");
    if (qualifier != std::string::npos && qualifier + 1 < type.size()) {
        if (type[qualifier] == ':' && qualifier > 0 && type[qualifier - 1] == ':')
            type = type.substr(qualifier + 1);
        else if (type[qualifier] == '.')
            type = type.substr(qualifier + 1);
    }
    return trim_copy(type);
}

std::string read_type_after_colon(const std::string& line, size_t colon_pos) {
    size_t pos = colon_pos + 1;
    while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) ++pos;
    size_t start = pos;
    int generic_depth = 0;
    while (pos < line.size()) {
        char c = line[pos];
        if (c == '<' || c == '[') ++generic_depth;
        else if ((c == '>' || c == ']') && generic_depth > 0) --generic_depth;
        if (generic_depth == 0 && (c == '=' || c == ';' || c == ',' || c == ')' || c == '{')) break;
        ++pos;
    }
    return normalize_type_hint(line.substr(start, pos - start));
}

std::string lower_ascii(std::string s);

std::string declaration_name_from_receiver(std::string receiver) {
    receiver = trim_copy(std::move(receiver));
    while (!receiver.empty() && (receiver.back() == '!' || receiver.back() == '?'))
        receiver.pop_back();
    size_t bracket = receiver.find_last_of(")]}");
    if (bracket != std::string::npos && bracket + 1 == receiver.size()) return "";
    size_t pos = receiver.find_last_of(".:");
    if (pos != std::string::npos && pos + 1 < receiver.size())
        receiver = receiver.substr(pos + 1);
    while (!receiver.empty() && (receiver.back() == '!' || receiver.back() == '?'))
        receiver.pop_back();
    return is_simple_identifier(receiver) ? receiver : "";
}

bool declaration_prefix_allows_receiver(std::string before) {
    before = trim_copy(std::move(before));
    if (before.empty()) return true;
    char last = before.back();
    if (last == '(' || last == ',' || last == '[') return true;

    std::vector<std::string> tokens;
    std::string token;
    for (char c : before) {
        if (is_identifier_char(c)) {
            token.push_back(c);
        } else if (!token.empty()) {
            tokens.push_back(lower_ascii(token));
            token.clear();
        }
    }
    if (!token.empty()) tokens.push_back(lower_ascii(token));
    if (tokens.empty()) return true;

    static const std::unordered_set<std::string> allowed = {
        "const", "let", "var", "public", "private", "protected", "readonly",
        "static", "declare", "export", "abstract", "override", "accessor"
    };
    for (const auto& t : tokens) {
        if (!allowed.count(t)) return false;
    }
    return true;
}

std::string infer_type_from_new_expression(const std::string& text) {
    size_t pos = text.find("new");
    while (pos != std::string::npos) {
        bool left_ok = (pos == 0) || !is_identifier_char(text[pos - 1]);
        size_t after_new = pos + 3;
        bool right_ok = after_new >= text.size() ||
            std::isspace(static_cast<unsigned char>(text[after_new]));
        if (left_ok && right_ok) {
            size_t start = after_new;
            while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) ++start;
            size_t end = start;
            while (end < text.size() &&
                   (is_identifier_char(text[end]) || text[end] == '.' || text[end] == ':')) {
                ++end;
            }
            auto type = normalize_type_hint(text.substr(start, end - start));
            if (!type.empty()) return type;
        }
        pos = text.find("new", pos + 3);
    }
    return "";
}

std::string infer_type_from_declaration_line(const std::string& line, const std::string& receiver) {
    std::string declared_name = declaration_name_from_receiver(receiver);
    if (declared_name.empty()) return "";

    size_t pos = line.find(declared_name);
    while (pos != std::string::npos) {
        bool left_ok = (pos == 0) || !is_identifier_char(line[pos - 1]);
        size_t end = pos + declared_name.size();
        bool right_ok = (end >= line.size()) || !is_identifier_char(line[end]);
        if (left_ok && right_ok &&
            declaration_prefix_allows_receiver(line.substr(0, pos))) {
            size_t next = end;
            while (next < line.size() && std::isspace(static_cast<unsigned char>(line[next]))) ++next;
            if (next < line.size() && line[next] == '?') {
                ++next;
                while (next < line.size() && std::isspace(static_cast<unsigned char>(line[next]))) ++next;
            }
            if (next < line.size() && line[next] == ':') {
                auto type = read_type_after_colon(line, next);
                if (!type.empty()) return type;
            }

            if (next < line.size() && line[next] == '=') {
                auto type = infer_type_from_new_expression(line.substr(next + 1));
                if (!type.empty()) return type;
            }

            if (next >= line.size() || line[next] == ';' ||
                line[next] == ',' || line[next] == '(' || line[next] == ')') {
                size_t before = pos;
                while (before > 0 && std::isspace(static_cast<unsigned char>(line[before - 1]))) --before;
                while (before > 0 && (line[before - 1] == '*' || line[before - 1] == '&' ||
                                      std::isspace(static_cast<unsigned char>(line[before - 1])))) {
                    --before;
                }
                size_t start = before;
                while (start > 0) {
                    char c = line[start - 1];
                    if (!(is_identifier_char(c) || c == ':' || c == '.' || c == '<' || c == '>' || c == ',')) break;
                    --start;
                }
                auto type = normalize_type_hint(line.substr(start, before - start));
                static const std::unordered_set<std::string> keywords = {
                    "const", "let", "var", "auto", "return", "new", "this", "public",
                    "private", "protected", "static", "final"
                };
                if (!type.empty() && !keywords.count(type)) return type;
            }
        }
        pos = line.find(declared_name, end);
    }
    return "";
}

std::string receiver_from_callee_text(const std::string& callee_text) {
    size_t pos = std::string::npos;
    size_t sep_len = 0;
    for (const auto& sep : {std::string("->"), std::string("::"), std::string(".")}) {
        size_t p = callee_text.rfind(sep);
        if (p != std::string::npos && (pos == std::string::npos || p > pos)) {
            pos = p;
            sep_len = sep.size();
        }
    }
    (void)sep_len;
    if (pos == std::string::npos) return "";
    return trim_copy(callee_text.substr(0, pos));
}

TSNode find_argument_list_node(TSNode call_node) {
    for (const char* field : {"arguments", "argument", "parameters"}) {
        TSNode child = ts_node_child_by_field_name(call_node, field, static_cast<uint32_t>(std::strlen(field)));
        if (!ts_node_is_null(child)) {
            std::string type = ts_node_type(child);
            if (type.find("argument") != std::string::npos || type == "arguments") return child;
        }
    }

    uint32_t count = ts_node_named_child_count(call_node);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_named_child(call_node, i);
        const char* type = ts_node_type(child);
        if (!type) continue;
        std::string t(type);
        if (t == "argument_list" || t == "arguments" || t == "call_arguments") return child;
    }
    return {};
}

std::string lower_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string classify_argument(TSNode arg, const std::string& source) {
    const char* type = ts_node_type(arg);
    std::string t = type ? type : "";
    std::string text = trim_copy(source_text(arg, source));
    std::string lower_type = lower_ascii(t);
    std::string lower_text = lower_ascii(text);

    if (lower_type.find("string") != std::string::npos || (!text.empty() && (text[0] == '"' || text[0] == '\'' || text[0] == '`')))
        return "string";
    if (lower_type.find("number") != std::string::npos || lower_type.find("integer") != std::string::npos ||
        lower_type.find("float") != std::string::npos)
        return "number";
    if (lower_text == "true" || lower_text == "false" || lower_type.find("boolean") != std::string::npos)
        return "bool";
    if (lower_text == "null" || lower_text == "undefined" || lower_text == "nil" || lower_text == "none")
        return "null";
    if (lower_type.find("object") != std::string::npos || lower_type.find("dictionary") != std::string::npos)
        return "object";
    if (lower_type.find("array") != std::string::npos || lower_type.find("list") != std::string::npos ||
        (!text.empty() && text[0] == '['))
        return "array";
    if (lower_type.find("lambda") != std::string::npos || lower_type.find("function") != std::string::npos ||
        lower_type.find("arrow_function") != std::string::npos || lower_text.find("=>") != std::string::npos)
        return "callback";
    if (lower_type.find("new_expression") != std::string::npos || lower_text.rfind("new ", 0) == 0) {
        std::string rest = trim_copy(text.size() >= 4 ? text.substr(4) : "");
        size_t end = 0;
        while (end < rest.size() && (is_identifier_char(rest[end]) || rest[end] == '.' || rest[end] == ':')) ++end;
        std::string type_name = normalize_type_hint(rest.substr(0, end));
        return type_name.empty() ? "new" : "new:" + type_name;
    }
    if (lower_type.find("call") != std::string::npos || lower_text.find('(') != std::string::npos)
        return "call";
    if (lower_type.find("member") != std::string::npos || lower_type.find("field") != std::string::npos ||
        lower_text.find('.') != std::string::npos || lower_text.find("->") != std::string::npos)
        return "member";
    if (is_simple_identifier(text)) return "identifier";
    return "expr";
}

std::string join_patterns(const std::vector<std::string>& patterns) {
    std::string out;
    for (const auto& p : patterns) {
        if (!out.empty()) out += ",";
        out += p;
    }
    if (out.size() > 128) out = out.substr(0, 128);
    return out;
}

bool is_js_export_assignment_target(TSNode node, const std::string& source) {
    if (ts_node_is_null(node) || std::string(ts_node_type(node)) != "member_expression") return false;
    std::string text = source_text(node, source);
    return text == "module.exports" || text.rfind("exports.", 0) == 0;
}

std::string js_member_property_name(TSNode node, const std::string& source) {
    TSNode property = ts_node_child_by_field_name(node, "property", 8);
    if (!ts_node_is_null(property)) return source_text(property, source);
    uint32_t count = ts_node_named_child_count(node);
    if (count == 0) return "";
    return source_text(ts_node_named_child(node, count - 1), source);
}

bool extract_this_assignment_field(TSNode node, const std::string& source, std::string& field_name) {
    const char* type = ts_node_type(node);
    if (!type || std::string(type) != "assignment_expression") return false;

    TSNode left = ts_node_child_by_field_name(node, "left", 4);
    if (ts_node_is_null(left)) left = ts_node_child(node, 0);
    if (ts_node_is_null(left)) return false;
    if (std::string(ts_node_type(left)) != "member_expression") return false;

    TSNode object = ts_node_child_by_field_name(left, "object", 6);
    if (ts_node_is_null(object) || std::string(ts_node_type(object)) != "this") return false;

    field_name = js_member_property_name(left, source);
    return !field_name.empty();
}

struct ThisAssignmentMatch {
    std::string field_name;
    TSNode assignment_node;
};

std::vector<ThisAssignmentMatch> collect_this_assignments(TSNode function_node, const std::string& source) {
    std::vector<ThisAssignmentMatch> matches;
    TSNode body = ts_node_child_by_field_name(function_node, "body", 4);
    if (ts_node_is_null(body)) return matches;

    std::unordered_set<std::string> seen_fields;
    std::vector<TSNode> stack{body};
    while (!stack.empty()) {
        TSNode current = stack.back();
        stack.pop_back();

        const char* current_type = ts_node_type(current);
        if (!current_type) continue;
        std::string current_type_str(current_type);

        if (current.id != body.id && is_nested_function_boundary(current_type_str)) continue;

        std::string field_name;
        if (extract_this_assignment_field(current, source, field_name) &&
            seen_fields.insert(field_name).second) {
            matches.push_back({field_name, current});
        }

        uint32_t count = ts_node_named_child_count(current);
        for (uint32_t i = 0; i < count; ++i) {
            stack.push_back(ts_node_named_child(current, i));
        }
    }

    return matches;
}

} // namespace

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
    node_count_ = 0;
    symbol_stack_.clear();
    deadline_ = (timeout_s_ > 0)
        ? std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s_)
        : std::chrono::steady_clock::time_point::max();

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
    std::string sig = signature;
    if (sig.empty() && (kind == "function" || kind == "method" || kind == "constructor_fn")) {
        sig = node_text(node);
        size_t body = sig.find('{');
        if (body != std::string::npos) sig = sig.substr(0, body);
        std::istringstream in(sig);
        std::string collapsed;
        std::string word;
        while (in >> word) {
            if (!collapsed.empty()) collapsed += ' ';
            collapsed += word;
            if (collapsed.size() > 512) {
                collapsed.resize(512);
                break;
            }
        }
        sig = collapsed;
    }

    result_->symbols.push_back({
        kind, name, qualname.empty() ? name : qualname,
        sig,
        static_cast<int>(start.row + 1), static_cast<int>(start.column),
        static_cast<int>(end.row + 1), static_cast<int>(end.column),
        true, visibility, doc_comment, ""
    });
}

void Extractor::add_ref(const std::string& kind, const std::string& name, TSNode node,
                         const std::string& evidence, int arg_count,
                         const std::string& arg_pattern,
                         const std::string& receiver_type_hint) {
    TSPoint start = ts_node_start_point(node);
    TSPoint end = ts_node_end_point(node);
    int containing = symbol_stack_.empty() ? -1 : symbol_stack_.back();

    result_->refs.push_back({
        kind, name,
        static_cast<int>(start.row + 1), static_cast<int>(start.column),
        static_cast<int>(end.row + 1), static_cast<int>(end.column),
        evidence, containing, arg_count, arg_pattern, receiver_type_hint
    });
}

void Extractor::add_call_ref(const std::string& name, TSNode node,
                             const std::string& evidence) {
    int arg_count = -1;
    std::string arg_pattern;
    TSNode args = find_argument_list_node(node);
    if (!ts_node_is_null(args)) {
        arg_count = static_cast<int>(ts_node_named_child_count(args));
        std::vector<std::string> patterns;
        patterns.reserve(static_cast<size_t>(std::max(arg_count, 0)));
        for (uint32_t i = 0; i < ts_node_named_child_count(args); ++i) {
            patterns.push_back(classify_argument(ts_node_named_child(args, i), *source_));
        }
        arg_pattern = join_patterns(patterns);
    }

    std::string receiver_type_hint;
    std::string receiver = receiver_from_callee_text(name);
    std::string declaration_name = declaration_name_from_receiver(receiver);
    if (!declaration_name.empty() && receiver != "this" && receiver != "self") {
        uint32_t call_start = ts_node_start_byte(node);
        size_t window_start = call_start > 65536 ? call_start - 65536 : 0;
        std::string prefix = source_->substr(window_start, call_start - window_start);
        std::istringstream lines(prefix);
        std::string line;
        while (std::getline(lines, line)) {
            auto type_hint = infer_type_from_declaration_line(line, receiver);
            if (!type_hint.empty()) receiver_type_hint = type_hint;
        }
    }

    add_ref("call", name, node, evidence, arg_count, arg_pattern, receiver_type_hint);
}

void Extractor::add_call_edge(int caller_idx, const std::string& callee_name, double confidence) {
    if (confidence < 0.3) return;  // FR-044: confidence floor
    result_->edges.push_back({
        caller_idx, -1, callee_name, "calls", confidence, "name_match"
    });
}

void Extractor::visit_node(TSNode root_node, const std::string& root_qualname, int /*depth*/) {
    // Iterative DFS using an explicit stack to avoid stack overflow on deeply
    // nested ASTs (the #1 cause of 0xC0000409 crashes on large files).
    // Each frame mirrors what the old recursive visit_node did per call.
    struct Frame {
        TSNode node;
        uint32_t child_count;
        uint32_t next_child;   // next child index to push
        bool pushed_scope;     // whether this frame pushed onto symbol_stack_
    };

    thread_local static std::vector<Frame> stack;
    stack.clear();

    auto process_node = [&](TSNode node) -> bool {
        if (ts_node_is_null(node)) return false;
        if (result_->truncated) return false;
        if (static_cast<int>(stack.size()) > max_depth_) return false;

        // Check deadline every 4096 nodes (~60ns overhead per check)
        if ((++node_count_ & 0xFFF) == 0 &&
            ((cancel_flag_ && *cancel_flag_ != 0) ||
             std::chrono::steady_clock::now() > deadline_)) {
            result_->truncated = true;
            result_->truncation_reason = "extraction timeout";
            return false;
        }

        const char* type = ts_node_type(node);
        if (!type) return false;

        std::string type_str(type);
        const std::string& parent_qualname = root_qualname;

        int sym_before = static_cast<int>(result_->symbols.size());

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

        bool pushed = static_cast<int>(result_->symbols.size()) > sym_before;
        if (pushed) symbol_stack_.push_back(sym_before);

        stack.push_back({node, ts_node_child_count(node), 0, pushed});
        return true;
    };

    if (!process_node(root_node)) return;

    while (!stack.empty()) {
        if (result_->truncated) break;

        auto& top = stack.back();
        if (top.next_child < top.child_count) {
            TSNode child = ts_node_child(top.node, top.next_child);
            top.next_child++;
            process_node(child);  // pushes a new frame if valid
        } else {
            // All children visited — pop this frame
            if (top.pushed_scope) symbol_stack_.pop_back();
            stack.pop_back();
        }
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
            add_call_ref(callee, node, "call_expression");
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
        if (!ts_node_is_null(func)) add_call_ref(node_text(func), node, "invocation");
    }
    else if (type == "using_directive") {
        auto name_node = ts_node_child_by_field_name(node, "name", 4);
        if (ts_node_is_null(name_node)) {
            // Fallback: try extracting from named children
            name_node = get_name_from_child(node, "name").empty() ? name_node : name_node;
            uint32_t count = ts_node_named_child_count(node);
            for (uint32_t i = 0; i < count; ++i) {
                TSNode child = ts_node_named_child(node, i);
                std::string ct(ts_node_type(child));
                if (ct == "identifier" || ct == "qualified_name") {
                    add_ref("include", node_text(child), node, "using_directive");
                    break;
                }
            }
        } else {
            add_ref("include", node_text(name_node), node, "using_directive");
        }
    }
    else if (type == "base_list") {
        uint32_t count = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < count; ++i) {
            TSNode child = ts_node_named_child(node, i);
            std::string child_type(ts_node_type(child));
            if (child_type == "identifier" || child_type == "qualified_name" ||
                child_type == "generic_name" || child_type == "simple_base_type") {
                add_ref("inherit", node_text(child), node, "base_list");
            }
        }
    }
    else if (type == "object_creation_expression") {
        auto type_node = ts_node_child(node, 1);
        if (!ts_node_is_null(type_node)) {
            add_call_ref(node_text(type_node), node, "object_creation");
        }
    }
}

void Extractor::extract_typescript(TSNode node, const std::string& type, const std::string& parent_qn) {
    auto maybe_add_constructor = [&](const std::string& name, TSNode symbol_node, TSNode function_node) -> bool {
        if (name.empty() || !starts_with_uppercase_ascii(name)) return false;

        auto this_assignments = collect_this_assignments(function_node, *source_);
        if (this_assignments.empty()) return false;

        std::string constructor_qn = parent_qn.empty() ? name : parent_qn + "." + name;
        int constructor_idx = static_cast<int>(result_->symbols.size());
        add_symbol("constructor_fn", name, symbol_node, constructor_qn);
        if (static_cast<int>(result_->symbols.size()) <= constructor_idx) return true;

        for (const auto& assignment : this_assignments) {
            int field_idx = static_cast<int>(result_->symbols.size());
            add_symbol("field", assignment.field_name, assignment.assignment_node,
                       constructor_qn + "._fields." + assignment.field_name);
            if (static_cast<int>(result_->symbols.size()) > field_idx) {
                result_->edges.push_back({
                    constructor_idx, field_idx, "", "contains", 1.0, "constructor_field"
                });
            }
        }
        return true;
    };

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
        TSNode value = ts_node_child_by_field_name(node, "value", 5);
        std::string value_type = ts_node_is_null(value) ? "" : ts_node_type(value);
        if (!name.empty() && is_js_function_like(value_type) && maybe_add_constructor(name, node, value)) {
            return;
        }
        if (!name.empty() && value_type == "assignment_expression") {
            TSNode right = ts_node_child_by_field_name(value, "right", 5);
            std::string right_type = ts_node_is_null(right) ? "" : ts_node_type(right);
            if (is_js_function_like(right_type) && maybe_add_constructor(name, node, right)) {
                return;
            }
        }
        if (!name.empty()) add_symbol("variable", name, node);
    }
    else if (type == "assignment_expression") {
        TSNode right = ts_node_child_by_field_name(node, "right", 5);
        std::string right_type = ts_node_is_null(right) ? "" : ts_node_type(right);
        if (!is_js_function_like(right_type)) return;

        TSNode left = ts_node_child_by_field_name(node, "left", 4);
        std::string name;
        if (!ts_node_is_null(left) && std::string(ts_node_type(left)) == "identifier") {
            name = node_text(left);
        } else if (!ts_node_is_null(left) && is_js_export_assignment_target(left, *source_) &&
                   right_type == "function_expression") {
            name = get_name_from_child(right, "name");
        } else if (right_type == "function_expression") {
            name = get_name_from_child(right, "name");
        }

        if (name.empty()) return;
        if (maybe_add_constructor(name, node, right)) return;
    }
    else if (type == "call_expression") {
        auto func = ts_node_child(node, 0);
        if (!ts_node_is_null(func)) add_call_ref(node_text(func), node, "call_expression");
    }
    else if (type == "member_expression") {
        // Check if object is `this`
        auto object = ts_node_child_by_field_name(node, "object", 6);
        if (!ts_node_is_null(object) && std::string(ts_node_type(object)) == "this") {
            auto property = ts_node_child_by_field_name(node, "property", 8);
            if (!ts_node_is_null(property)) {
                auto parent = ts_node_parent(node);
                const char* parent_type = ts_node_type(parent);
                bool is_write = parent_type && (
                    std::string(parent_type) == "assignment_expression" ||
                    std::string(parent_type) == "augmented_assignment_expression"
                ) && ts_node_child(parent, 0).id == node.id; // left side
                std::string evidence = is_write ? "this_member_write" : "this_member_read";
                add_ref("field_access", node_text(property), node, evidence);
            }
        }
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
        if (!ts_node_is_null(func)) add_call_ref(node_text(func), node, "call");
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
        if (!ts_node_is_null(func)) add_call_ref(node_text(func), node, "call");
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
        if (!ts_node_is_null(func)) add_call_ref(node_text(func), node, "call");
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
        if (!name.empty()) add_call_ref(name, node, "call");
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
        if (!name.empty()) add_call_ref(name, node, "call");
    }
}

// Free function — pre-sized read avoids O(N log N) buffer doubling
std::string read_file_content(const std::filesystem::path& path) {
#ifdef _WIN32
    // Use FILE_FLAG_SEQUENTIAL_SCAN to enable aggressive OS read-ahead.
    // On cold cache, this can save 5-10ms/file by hinting the NTFS cache manager.
    HANDLE hFile = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN | FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return "";

    LARGE_INTEGER li_size;
    if (!GetFileSizeEx(hFile, &li_size)) {
        CloseHandle(hFile);
        return "";
    }
    auto size = static_cast<size_t>(li_size.QuadPart);
    if (size == 0) {
        CloseHandle(hFile);
        return "";
    }

    std::string content(size, '\0');
    DWORD bytes_read = 0;
    // For files > 4GB, would need a loop, but source files are capped by max_file_size
    BOOL ok = ReadFile(hFile, content.data(), static_cast<DWORD>(size), &bytes_read, nullptr);
    CloseHandle(hFile);

    if (!ok || bytes_read != static_cast<DWORD>(size)) {
        return "";
    }
    return content;
#else
    // POSIX: use open() with posix_fadvise for sequential read-ahead
    auto size = std::filesystem::file_size(path);
    if (size == 0) return "";

    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return "";

    #ifdef POSIX_FADV_SEQUENTIAL
    posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
    #endif

    std::string content(size, '\0');
    auto nread = ::read(fd, content.data(), size);
    ::close(fd);

    if (nread != static_cast<ssize_t>(size)) return "";
    return content;
#endif
}

} // namespace codetopo
