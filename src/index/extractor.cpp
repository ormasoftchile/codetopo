#include "index/extractor.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <functional>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <regex>
#include <sstream>
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

static constexpr std::array<uint64_t, 16> kMinHashSeeds = {{
    0x9e3779b97f4a7c15ULL, 0x6c62272e07bb0142ULL, 0x94d049bb133111ebULL,
    0xbf58476d1ce4e5b9ULL, 0xd2a98b26625eee7bULL, 0x17f4fce5a5f5f8f0ULL,
    0x3c6ef372fe94f82bULL, 0x82cbc74a3c965d9eULL, 0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL,
    0x5be0cd19137e2179ULL, 0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL,
    0x3956c25bf348b538ULL
}};

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

std::string read_type_after_colon(std::string_view line, size_t colon_pos) {
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
    return normalize_type_hint(std::string(line.substr(start, pos - start)));
}

size_t skip_inline_whitespace(std::string_view text, size_t pos) {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
    return pos;
}

size_t previous_significant_char(std::string_view text, size_t pos) {
    while (pos > 0) {
        --pos;
        if (!std::isspace(static_cast<unsigned char>(text[pos]))) return pos;
    }
    return std::string::npos;
}

size_t declaration_context_start(std::string_view text, size_t pos) {
    while (pos > 0) {
        char c = text[pos - 1];
        if (c == '\n' || c == '\r' || c == ';' || c == '{' || c == '}') break;
        --pos;
    }
    return pos;
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
            tokens.push_back(token);
            token.clear();
        }
    }
    if (!token.empty()) tokens.push_back(token);
    if (tokens.empty()) return true;

    static const std::unordered_set<std::string> allowed = {
        "const", "let", "var", "public", "private", "protected", "readonly",
        "static", "declare", "export", "abstract", "override", "accessor"
    };
    for (const auto& t : tokens) {
        if (allowed.count(lower_ascii(t))) continue;
        if (!t.empty() && std::isupper(static_cast<unsigned char>(t[0]))) continue;
        return false;
    }
    return true;
}

std::string infer_type_from_new_expression(std::string_view text) {
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
            auto type = normalize_type_hint(std::string(text.substr(start, end - start)));
            if (!type.empty()) return type;
        }
        pos = text.find("new", pos + 3);
    }
    return "";
}

std::string infer_type_from_destructured_annotation(std::string_view text,
                                                    const std::string& declared_name,
                                                    size_t name_end) {
    size_t after_name = skip_inline_whitespace(text, name_end);
    if (after_name >= text.size() || (text[after_name] != ',' && text[after_name] != '}')) return "";

    size_t close_brace = text.find('}', after_name);
    if (close_brace == std::string::npos) return "";
    size_t colon = skip_inline_whitespace(text, close_brace + 1);
    if (colon >= text.size() || text[colon] != ':') return "";

    size_t type_literal = skip_inline_whitespace(text, colon + 1);
    if (type_literal >= text.size() || text[type_literal] != '{') return "";

    size_t prop = text.find(declared_name, type_literal + 1);
    while (prop != std::string::npos) {
        bool left_ok = (prop == 0) || !is_identifier_char(text[prop - 1]);
        size_t prop_end = prop + declared_name.size();
        bool right_ok = (prop_end >= text.size()) || !is_identifier_char(text[prop_end]);
        if (left_ok && right_ok) {
            size_t prop_colon = skip_inline_whitespace(text, prop_end);
            if (prop_colon < text.size() && text[prop_colon] == ':') {
                auto type = read_type_after_colon(text, prop_colon);
                if (!type.empty()) return type;
            }
        }
        prop = text.find(declared_name, prop_end);
    }
    return "";
}

std::string infer_type_from_declaration_text(std::string_view text, const std::string& receiver) {
    std::string declared_name = declaration_name_from_receiver(receiver);
    if (declared_name.empty()) return "";

    size_t pos = text.find(declared_name);
    while (pos != std::string::npos) {
        bool left_ok = (pos == 0) || !is_identifier_char(text[pos - 1]);
        size_t end = pos + declared_name.size();
        bool right_ok = (end >= text.size()) || !is_identifier_char(text[end]);
        if (left_ok && right_ok) {
            size_t context_start = declaration_context_start(text, pos);
            std::string_view before = text.substr(context_start, pos - context_start);
            bool prefix_allowed = declaration_prefix_allows_receiver(std::string(before));
            size_t prev_sig = previous_significant_char(text, pos);
            bool empty_prefix_has_anchor =
                prev_sig != std::string::npos &&
                (text[prev_sig] == '(' || text[prev_sig] == ',' || text[prev_sig] == '[');

            if (prefix_allowed && (!trim_copy(std::string(before)).empty() || empty_prefix_has_anchor)) {
                size_t next = skip_inline_whitespace(text, end);
                if (next < text.size() && text[next] == '?') {
                    next = skip_inline_whitespace(text, next + 1);
                }
                if (next < text.size() && text[next] == ':') {
                    auto type = read_type_after_colon(text, next);
                    if (!type.empty()) return type;
                }
                if (next < text.size() && text[next] == '=') {
                    auto type = infer_type_from_new_expression(text.substr(next + 1));
                    if (!type.empty()) return type;
                }
            }

            auto destructured = infer_type_from_destructured_annotation(text, declared_name, end);
            if (!destructured.empty()) return destructured;
        }
        pos = text.find(declared_name, end);
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

bool is_function_like_kind(const std::string& kind) {
    return kind == "function" || kind == "method" || kind == "constructor_fn";
}

TSNode find_body_by_field(TSNode node) {
    static constexpr const char* kFields[] = {"body", "consequence"};
    for (const char* field : kFields) {
        TSNode body = ts_node_child_by_field_name(node, field, static_cast<uint32_t>(std::strlen(field)));
        if (!ts_node_is_null(body)) return body;
    }
    return TSNode{};
}

TSNode find_function_body_node(TSNode node) {
    if (ts_node_is_null(node)) return TSNode{};

    TSNode body = find_body_by_field(node);
    if (!ts_node_is_null(body)) return body;

    std::string type = ts_node_type(node);
    uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_named_child(node, i);
        if (!ts_node_is_named(child)) continue;

        std::string child_type = ts_node_type(child);
        if (child_type == "statement_block" || child_type == "compound_statement" ||
            child_type == "block" || child_type == "body" ||
            child_type == "declaration_list") {
            return child;
        }
    }

    if (type == "function_expression" || type == "arrow_function") {
        return node;
    }

    return TSNode{};
}

std::string normalize_leaf_type(TSNode node) {
    std::string type = lower_ascii(ts_node_type(node));
    if (type.find("identifier") != std::string::npos || type == "shorthand_property_identifier_pattern")
        return "I";
    if (type.find("string") != std::string::npos)
        return "S";
    if (type.find("number") != std::string::npos || type.find("integer") != std::string::npos ||
        type.find("float") != std::string::npos)
        return "N";
    if (type.find("type") != std::string::npos)
        return "T";
    return type.substr(0, std::min<size_t>(4, type.size()));
}

void collect_leaf_tokens(TSNode node, std::vector<std::string>& tokens) {
    // Iterative DFS to avoid stack overflow on deeply nested ASTs.
    // A recursive version crashes on large C# methods with deep generic nesting.
    struct Frame { TSNode node; uint32_t next_child; };
    std::vector<Frame> stack;
    stack.reserve(256);
    if (!ts_node_is_null(node) && ts_node_is_named(node))
        stack.push_back({node, 0});

    while (!stack.empty()) {
        auto& top = stack.back();
        uint32_t named_count = ts_node_named_child_count(top.node);
        if (named_count == 0) {
            tokens.push_back(normalize_leaf_type(top.node));
            stack.pop_back();
            continue;
        }
        if (top.next_child >= named_count) {
            stack.pop_back();
            continue;
        }
        TSNode child = ts_node_named_child(top.node, top.next_child++);
        if (!ts_node_is_null(child) && ts_node_is_named(child))
            stack.push_back({child, 0});
    }
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

bool is_string_literal_node(TSNode node) {
    if (ts_node_is_null(node)) return false;
    std::string type = ts_node_type(node);
    return type == "string" ||
           type == "string_literal" ||
           type == "interpreted_string_literal" ||
           type == "raw_string_literal" ||
           type == "template_string";
}

std::string decode_string_literal(TSNode node, const std::string& source) {
    if (!is_string_literal_node(node)) return "";
    std::string text = trim_copy(source_text(node, source));
    if (text.size() < 2) return "";

    char quote = text.front();
    if ((quote == '"' || quote == '\'' || quote == '`') && text.back() == quote) {
        if (quote == '`' && text.find("${") != std::string::npos) return "";
        return text.substr(1, text.size() - 2);
    }
    return "";
}

std::string normalize_http_path(std::string url) {
    url = trim_copy(std::move(url));
    if (url.empty()) return "";

    size_t scheme = url.find("://");
    if (scheme != std::string::npos) {
        size_t path_pos = url.find('/', scheme + 3);
        url = path_pos == std::string::npos ? "/" : url.substr(path_pos);
    } else if (url.rfind("//", 0) == 0) {
        size_t path_pos = url.find('/', 2);
        url = path_pos == std::string::npos ? "/" : url.substr(path_pos);
    }

    size_t query = url.find_first_of("?#");
    if (query != std::string::npos) url = url.substr(0, query);
    if (url.empty()) return "";
    if (url.front() != '/') {
        if (url.find('/') == std::string::npos) return "";
        url = "/" + url;
    }
    return url;
}

bool is_http_verb(const std::string& name) {
    static const std::unordered_set<std::string> verbs = {
        "get", "post", "put", "delete", "do"
    };
    return verbs.count(name) != 0;
}

bool receiver_looks_like_http_client(const std::string& receiver) {
    std::string lower = lower_ascii(receiver);
    if (lower == "http" || lower == "axios" || lower == "fetch") return true;
    if (lower == "client" || lower == "httpclient") return true;
    if (lower.find(".client") != std::string::npos) return true;
    if (lower.find(".http") != std::string::npos) return true;
    if (lower.size() > 6 && lower.rfind("client") == lower.size() - 6) return true;
    return false;
}

bool match_http_call_pattern(const std::string& language,
                             const std::string& callee_text) {
    std::string callee = trim_copy(callee_text);
    if (callee.empty()) return false;

    std::string lower = lower_ascii(callee);
    if (language == "typescript" || language == "javascript") {
        if (lower == "fetch") return true;

        size_t dot = lower.rfind('.');
        if (dot == std::string::npos) return false;
        std::string receiver = lower.substr(0, dot);
        std::string method = lower.substr(dot + 1);
        if (!is_http_verb(method)) return false;
        if (receiver == "axios" || receiver == "this.http") return true;
        if (receiver.size() > 5 && receiver.rfind(".http") == receiver.size() - 5) return true;
        return false;
    }

    if (language == "go") {
        size_t dot = lower.rfind('.');
        if (dot == std::string::npos) return false;
        std::string receiver = lower.substr(0, dot);
        std::string method = lower.substr(dot + 1);
        if (!is_http_verb(method)) return false;
        return receiver_looks_like_http_client(receiver);
    }

    return false;
}

void maybe_add_http_call_ref(ExtractionResult* result,
                             const std::string& language,
                             const std::string& callee_text,
                             TSNode call_node,
                             const std::string& source,
                             const std::vector<int>& symbol_stack) {
    if (!match_http_call_pattern(language, callee_text)) return;

    TSNode args = find_argument_list_node(call_node);
    if (ts_node_is_null(args) || ts_node_named_child_count(args) == 0) return;

    TSNode first_arg = ts_node_named_child(args, 0);
    std::string literal = decode_string_literal(first_arg, source);
    std::string path = normalize_http_path(literal);
    if (path.empty()) return;

    TSPoint start = ts_node_start_point(call_node);
    TSPoint end = ts_node_end_point(call_node);
    int containing = symbol_stack.empty() ? -1 : symbol_stack.back();
    result->refs.push_back({
        "http_call", path,
        static_cast<int>(start.row + 1), static_cast<int>(start.column),
        static_cast<int>(end.row + 1), static_cast<int>(end.column),
        "http_client_call", containing, -1, "", ""
    });
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

bool extract_this_member_field(TSNode node, const std::string& source, std::string& field_name) {
    const char* type = ts_node_type(node);
    if (!type || std::string(type) != "member_expression") return false;

    TSNode object = ts_node_child_by_field_name(node, "object", 6);
    if (ts_node_is_null(object) || std::string(ts_node_type(object)) != "this") return false;

    field_name = js_member_property_name(node, source);
    return !field_name.empty();
}

bool extract_this_assignment_field(TSNode node, const std::string& source, std::string& field_name) {
    const char* type = ts_node_type(node);
    if (!type || std::string(type) != "assignment_expression") return false;

    TSNode left = ts_node_child_by_field_name(node, "left", 4);
    if (ts_node_is_null(left)) left = ts_node_child(node, 0);
    if (ts_node_is_null(left)) return false;
    return extract_this_member_field(left, source, field_name);
}

bool this_member_looks_assigned_in_source(TSNode node, const std::string& source) {
    uint32_t pos = ts_node_end_byte(node);
    while (pos < source.size() && std::isspace(static_cast<unsigned char>(source[pos]))) ++pos;
    if (pos >= source.size()) return false;

    if (source[pos] == '=') {
        if (pos + 1 < source.size() && (source[pos + 1] == '=' || source[pos + 1] == '>')) return false;
        return true;
    }

    if (pos + 1 < source.size()) {
        switch (source[pos]) {
            case '+':
            case '-':
            case '*':
            case '/':
            case '%':
            case '&':
            case '|':
            case '^':
                return source[pos + 1] == '=';
            default:
                break;
        }
    }
    return false;
}

struct ThisAssignmentMatch {
    std::string field_name;
    TSNode assignment_node;
};

std::vector<ThisAssignmentMatch> collect_this_assignments(TSNode function_node, const std::string& source) {
    std::vector<ThisAssignmentMatch> matches;
    TSNode body = find_function_body_node(function_node);
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
        } else if (current_type_str == "member_expression" &&
                   extract_this_member_field(current, source, field_name) &&
                   this_member_looks_assigned_in_source(current, source) &&
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

struct LegacyJsConstructorMatch {
    std::string name;
    std::string fallback_kind;
    size_t header_start = 0;
    size_t body_open = 0;
    size_t body_close = 0;
};

TSPoint point_for_offset(const std::string& source, size_t offset) {
    offset = (std::min)(offset, source.size());
    TSPoint point{0, 0};
    for (size_t i = 0; i < offset; ++i) {
        if (source[i] == '\n') {
            ++point.row;
            point.column = 0;
        } else {
            ++point.column;
        }
    }
    return point;
}

bool find_matching_brace(const std::string& source, size_t open_pos, size_t& close_pos) {
    if (open_pos >= source.size() || source[open_pos] != '{') return false;

    int depth = 0;
    bool in_single = false;
    bool in_double = false;
    bool in_template = false;
    bool in_line_comment = false;
    bool in_block_comment = false;
    bool escaped = false;

    for (size_t i = open_pos; i < source.size(); ++i) {
        char c = source[i];
        char next = (i + 1 < source.size()) ? source[i + 1] : '\0';

        if (in_line_comment) {
            if (c == '\n') in_line_comment = false;
            continue;
        }
        if (in_block_comment) {
            if (c == '*' && next == '/') {
                in_block_comment = false;
                ++i;
            }
            continue;
        }
        if (in_single) {
            if (!escaped && c == '\'') in_single = false;
            escaped = (!escaped && c == '\\');
            continue;
        }
        if (in_double) {
            if (!escaped && c == '"') in_double = false;
            escaped = (!escaped && c == '\\');
            continue;
        }
        if (in_template) {
            if (!escaped && c == '`') in_template = false;
            escaped = (!escaped && c == '\\');
            continue;
        }

        escaped = false;
        if (c == '/' && next == '/') {
            in_line_comment = true;
            ++i;
            continue;
        }
        if (c == '/' && next == '*') {
            in_block_comment = true;
            ++i;
            continue;
        }
        if (c == '\'') {
            in_single = true;
            continue;
        }
        if (c == '"') {
            in_double = true;
            continue;
        }
        if (c == '`') {
            in_template = true;
            continue;
        }
        if (c == '{') {
            ++depth;
            continue;
        }
        if (c == '}') {
            --depth;
            if (depth == 0) {
                close_pos = i;
                return true;
            }
        }
    }
    return false;
}

template <typename MatchHandler>
void for_each_regex_match(const std::string& source, const std::regex& pattern, MatchHandler&& handler) {
    for (std::sregex_iterator it(source.begin(), source.end(), pattern), end; it != end; ++it) {
        handler(*it);
    }
}

std::vector<LegacyJsConstructorMatch> find_legacy_js_constructor_matches(const std::string& source) {
    static const std::regex kVarExportCtor(
        R"((^|[;\n])\s*(?:let|var|const)\s+([A-Za-z_$][A-Za-z0-9_$]*)\s*=\s*module\.exports\s*=\s*function(?:\s+([A-Za-z_$][A-Za-z0-9_$]*))?\s*\([^)]*\)\s*\{)",
        std::regex::ECMAScript);
    static const std::regex kVarCtor(
        R"((^|[;\n])\s*(?:let|var|const)\s+([A-Za-z_$][A-Za-z0-9_$]*)\s*=\s*function(?:\s+([A-Za-z_$][A-Za-z0-9_$]*))?\s*\([^)]*\)\s*\{)",
        std::regex::ECMAScript);
    static const std::regex kAssignCtor(
        R"((^|[;\n])\s*([A-Za-z_$][A-Za-z0-9_$]*)\s*=\s*function(?:\s+([A-Za-z_$][A-Za-z0-9_$]*))?\s*\([^)]*\)\s*\{)",
        std::regex::ECMAScript);
    static const std::regex kModuleExportCtor(
        R"((^|[;\n])\s*module\.exports\s*=\s*function\s+([A-Za-z_$][A-Za-z0-9_$]*)\s*\([^)]*\)\s*\{)",
        std::regex::ECMAScript);

    std::vector<LegacyJsConstructorMatch> matches;
    std::unordered_set<size_t> seen_headers;

    auto add_matches = [&](const std::regex& pattern, const std::string& fallback_kind, auto&& name_for) {
        for_each_regex_match(source, pattern, [&](const std::smatch& match) {
            const size_t start = static_cast<size_t>(match.position(0));
            const size_t open = start + static_cast<size_t>(match.length(0)) - 1;
            if (!seen_headers.insert(start).second) return;

            size_t close = 0;
            if (!find_matching_brace(source, open, close)) return;

            std::string name = name_for(match);
            matches.push_back({name, fallback_kind, start, open, close});
        });
    };

    add_matches(kVarExportCtor, "variable", [](const std::smatch& match) {
        return match[2].str();
    });
    add_matches(kVarCtor, "variable", [](const std::smatch& match) {
        return match[2].str();
    });
    add_matches(kAssignCtor, "", [](const std::smatch& match) {
        return match[2].str();
    });
    add_matches(kModuleExportCtor, "", [](const std::smatch& match) {
        return match[2].str();
    });

    std::sort(matches.begin(), matches.end(), [](const auto& a, const auto& b) {
        return a.header_start < b.header_start;
    });
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
    if (*language_ == "javascript") {
        add_javascript_constructor_fallbacks(source);
    }

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

void Extractor::add_javascript_constructor_fallbacks(const std::string& source) {
    std::unordered_set<std::string> existing_symbols;
    existing_symbols.reserve(result_->symbols.size() * 2 + 8);
    for (const auto& symbol : result_->symbols) {
        existing_symbols.insert(symbol.kind + "\n" + symbol.qualname);
    }

    auto append_symbol = [&](const std::string& kind,
                             const std::string& name,
                             const std::string& qualname,
                             size_t start_offset,
                             size_t end_offset) -> int {
        std::string key = kind + "\n" + qualname;
        auto [_, inserted] = existing_symbols.insert(key);
        if (!inserted) return -1;

        TSPoint start = point_for_offset(source, start_offset);
        TSPoint end = point_for_offset(source, end_offset);

        ExtractedSymbol symbol;
        symbol.kind = kind;
        symbol.name = name;
        symbol.qualname = qualname;
        symbol.start_line = static_cast<int>(start.row + 1);
        symbol.start_col = static_cast<int>(start.column);
        symbol.end_line = static_cast<int>(end.row + 1);
        symbol.end_col = static_cast<int>(end.column);
        result_->symbols.push_back(std::move(symbol));
        return static_cast<int>(result_->symbols.size()) - 1;
    };

    static const std::regex kThisAssignment(
        R"(\bthis\.([A-Za-z_$][A-Za-z0-9_$]*)\s*=)",
        std::regex::ECMAScript);

    for (const auto& match : find_legacy_js_constructor_matches(source)) {
        std::string body = source.substr(match.body_open + 1, match.body_close - match.body_open - 1);
        std::vector<std::pair<std::string, std::pair<size_t, size_t>>> fields;
        std::unordered_set<std::string> seen_fields;
        for_each_regex_match(body, kThisAssignment, [&](const std::smatch& field_match) {
            std::string field_name = field_match[1].str();
            if (!seen_fields.insert(field_name).second) return;

            size_t rel_start = static_cast<size_t>(field_match.position(0));
            size_t global_start = match.body_open + 1 + rel_start;
            size_t global_end = global_start + static_cast<size_t>(field_match.length(0));
            fields.push_back({field_name, {global_start, global_end}});
        });

        if (fields.empty()) {
            if (!match.fallback_kind.empty()) {
                append_symbol(match.fallback_kind, match.name, match.name,
                              match.header_start, match.body_close + 1);
            }
            continue;
        }
        if (!starts_with_uppercase_ascii(match.name)) continue;

        int constructor_idx = append_symbol("constructor_fn", match.name, match.name,
                                            match.header_start, match.body_close + 1);
        if (constructor_idx < 0) {
            for (size_t i = 0; i < result_->symbols.size(); ++i) {
                if (result_->symbols[i].kind == "constructor_fn" &&
                    result_->symbols[i].qualname == match.name) {
                    constructor_idx = static_cast<int>(i);
                    break;
                }
            }
            if (constructor_idx < 0) continue;
        }

        for (const auto& field : fields) {
            const std::string& field_name = field.first;
            std::string qualname = match.name + "._fields." + field_name;
            int field_idx = append_symbol("field", field_name, qualname,
                                          field.second.first, field.second.second);
            if (field_idx >= 0) {
                result_->edges.push_back({
                    constructor_idx, field_idx, "", "contains", 1.0, "constructor_field"
                });
            }
        }
    }
}

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
                            const std::string& visibility,
                            TSNode fingerprint_node) {
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
    if (sig.empty() && is_function_like_kind(kind)) {
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

    std::string fingerprint;
    if (is_function_like_kind(kind)) {
        fingerprint = compute_fingerprint(ts_node_is_null(fingerprint_node) ? node : fingerprint_node);
    }

    result_->symbols.push_back({
        kind, name, qualname.empty() ? name : qualname,
        sig, fingerprint,
        static_cast<int>(start.row + 1), static_cast<int>(start.column),
        static_cast<int>(end.row + 1), static_cast<int>(end.column),
        true, visibility, doc_comment, ""
    });
}

std::string Extractor::compute_fingerprint(TSNode node) {
    TSNode body = find_function_body_node(node);
    if (ts_node_is_null(body)) return "";

    std::vector<std::string> tokens;
    collect_leaf_tokens(body, tokens);
    if (tokens.size() < 30) return "";

    // Use FNV-1a hash with per-seed XOR — zero heap allocations.
    // Each MinHash value = min over all trigrams of hash(trigram, seed).
    static constexpr uint32_t FNV_PRIME  = 0x01000193u;
    static constexpr uint32_t FNV_OFFSET = 0x811c9dc5u;

    auto fnv1a_mix = [](uint32_t h, const char* s, size_t len) -> uint32_t {
        for (size_t i = 0; i < len; i++) {
            h ^= static_cast<uint8_t>(s[i]);
            h *= FNV_PRIME;
        }
        return h;
    };

    std::array<uint32_t, kMinHashSeeds.size()> mins;
    mins.fill(std::numeric_limits<uint32_t>::max());

    const size_t n = tokens.size();
    for (size_t i = 0; i + 2 < n; ++i) {
        const std::string& t0 = tokens[i];
        const std::string& t1 = tokens[i + 1];
        const std::string& t2 = tokens[i + 2];

        for (size_t si = 0; si < kMinHashSeeds.size(); ++si) {
            // Hash: seed bytes → t0 → separator → t1 → separator → t2
            uint32_t seed32 = static_cast<uint32_t>(kMinHashSeeds[si] & 0xFFFFFFFFu);
            uint32_t h = FNV_OFFSET ^ seed32;
            h = fnv1a_mix(h, t0.data(), t0.size());
            h ^= '|'; h *= FNV_PRIME;
            h = fnv1a_mix(h, t1.data(), t1.size());
            h ^= '|'; h *= FNV_PRIME;
            h = fnv1a_mix(h, t2.data(), t2.size());
            mins[si] = std::min(mins[si], h);
        }
    }

    char buf[kMinHashSeeds.size() * 8 + 1];
    char* p = buf;
    for (uint32_t value : mins) {
        static const char hex[] = "0123456789abcdef";
        *p++ = hex[(value >> 28) & 0xF]; *p++ = hex[(value >> 24) & 0xF];
        *p++ = hex[(value >> 20) & 0xF]; *p++ = hex[(value >> 16) & 0xF];
        *p++ = hex[(value >> 12) & 0xF]; *p++ = hex[(value >>  8) & 0xF];
        *p++ = hex[(value >>  4) & 0xF]; *p++ = hex[(value >>  0) & 0xF];
    }
    *p = '\0';
    return std::string(buf, kMinHashSeeds.size() * 8);
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
        // 8KB window: type declarations are almost always within 200 lines of the call.
        // 64KB caused cumulative heap pressure crashing the indexer after ~2400 TS files.
        static constexpr size_t kReceiverTypeWindow = 8192;
        size_t window_start = call_start > kReceiverTypeWindow ? call_start - kReceiverTypeWindow : 0;
        std::string_view prefix(source_->data() + window_start, call_start - window_start);
        receiver_type_hint = infer_type_from_declaration_text(prefix, receiver);
        if (receiver_type_hint.empty() && declaration_name[0] != '_') {
            std::string alt_receiver = receiver;
            std::string underscored = "_" + declaration_name;
            size_t last_dot = alt_receiver.rfind('.');
            if (last_dot != std::string::npos) {
                alt_receiver = alt_receiver.substr(0, last_dot + 1) + underscored;
            } else {
                alt_receiver = underscored;
            }
            receiver_type_hint = infer_type_from_declaration_text(prefix, alt_receiver);
        }
        if (receiver_type_hint.empty() && declaration_name[0] == '_' && declaration_name.size() > 1) {
            std::string alt_receiver = receiver;
            std::string without_underscore = declaration_name.substr(1);
            size_t last_dot = alt_receiver.rfind('.');
            if (last_dot != std::string::npos) {
                alt_receiver = alt_receiver.substr(0, last_dot + 1) + without_underscore;
            } else {
                alt_receiver = without_underscore;
            }
            receiver_type_hint = infer_type_from_declaration_text(prefix, alt_receiver);
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
        add_symbol("constructor_fn", name, symbol_node, constructor_qn, "", "", function_node);
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
        if (!ts_node_is_null(func)) {
            std::string callee = node_text(func);
            add_call_ref(callee, node, "call_expression");
            maybe_add_http_call_ref(result_, *language_, callee, node, *source_, symbol_stack_);
        }
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

void Extractor::extract_go(TSNode node, const std::string& type, const std::string& /*parent_qn*/) {
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
        if (!ts_node_is_null(func)) {
            std::string callee = node_text(func);
            add_call_ref(callee, node, "call");
            maybe_add_http_call_ref(result_, *language_, callee, node, *source_, symbol_stack_);
        }
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

void Extractor::extract_bash(TSNode node, const std::string& type, const std::string& /*parent_qn*/) {
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

void Extractor::extract_sql(TSNode node, const std::string& type, const std::string& /*parent_qn*/) {
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
