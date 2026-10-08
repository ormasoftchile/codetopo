#pragma once

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace codetopo::call_binding {

inline std::string trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.remove_suffix(1);
    return std::string(value);
}

inline std::optional<std::vector<std::string>> split_parameters(const std::string& text) {
    std::vector<std::string> parts;
    std::string current;
    int paren = 0, angle = 0, brace = 0, bracket = 0;
    char quote = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (quote) {
            current += c;
            if (c == quote && (i == 0 || text[i - 1] != '\\')) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'' || c == '`') { quote = c; current += c; continue; }
        if (c == '(') ++paren;
        else if (c == ')' && paren > 0) --paren;
        else if (c == '<') ++angle;
        else if (c == '>' && angle > 0) --angle;
        else if (c == '{') ++brace;
        else if (c == '}' && brace > 0) --brace;
        else if (c == '[') ++bracket;
        else if (c == ']' && bracket > 0) --bracket;
        if (c == ',' && paren == 0 && angle == 0 && brace == 0 && bracket == 0) {
            parts.push_back(trim(current));
            current.clear();
        } else current += c;
    }
    if (paren || angle || brace || bracket || quote) return std::nullopt;
    auto tail = trim(current);
    if (!tail.empty() || !parts.empty()) parts.push_back(tail);
    if (parts.size() == 1) {
        auto only = parts[0];
        std::transform(only.begin(), only.end(), only.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (only.empty() || only == "void") parts.clear();
    }
    return parts;
}

inline std::optional<std::vector<std::string>> parameters(
    const std::string& signature, const std::string& name) {
    size_t name_pos = signature.find(name);
    size_t open = signature.find('(', name_pos == std::string::npos ? 0 : name_pos + name.size());
    if (open == std::string::npos) return std::nullopt;
    int depth = 1;
    char quote = 0;
    size_t close = open + 1;
    while (close < signature.size() && depth) {
        char c = signature[close];
        if (quote) {
            if (c == quote && signature[close - 1] != '\\') quote = 0;
        } else if (c == '"' || c == '\'' || c == '`') quote = c;
        else if (c == '(') ++depth;
        else if (c == ')') --depth;
        if (depth) ++close;
    }
    if (depth) return std::nullopt;
    return split_parameters(signature.substr(open + 1, close - open - 1));
}

inline bool has_top_level_char(const std::string& text, char needle) {
    int paren = 0, angle = 0, brace = 0, bracket = 0;
    char quote = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (quote) {
            if (c == quote && (i == 0 || text[i - 1] != '\\')) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'' || c == '`') { quote = c; continue; }
        if (c == '(') ++paren;
        else if (c == ')' && paren > 0) --paren;
        else if (c == '<') ++angle;
        else if (c == '>' && angle > 0) --angle;
        else if (c == '{') ++brace;
        else if (c == '}' && brace > 0) --brace;
        else if (c == '[') ++bracket;
        else if (c == ']' && bracket > 0) --bracket;
        else if (c == needle && paren == 0 && angle == 0 && brace == 0 && bracket == 0)
            return true;
    }
    return false;
}

inline bool optional_parameter(const std::string& param) {
    if (has_top_level_char(param, '=')) return true;
    auto colon = param.find(':');
    auto question = param.find('?');
    return question != std::string::npos && (colon == std::string::npos || question < colon);
}

struct Arity {
    int minimum = -1;
    int maximum = -1;
    bool known = false;
};

inline Arity signature_arity(const std::string& signature, const std::string& name) {
    auto params = parameters(signature, name);
    if (!params) return {};
    Arity arity{0, 0, true};
    for (const auto& param : *params) {
        if (param.find("...") != std::string::npos || param.starts_with("params ") ||
            param.starts_with('*')) {
            arity.maximum = -1;
            continue;
        }
        if (arity.maximum >= 0) ++arity.maximum;
        if (!optional_parameter(param)) ++arity.minimum;
    }
    return arity;
}

inline std::string without_templates(std::string_view value) {
    std::string result;
    int depth = 0;
    for (char c : value) {
        auto operator_pos = result.rfind("operator");
        bool operator_name = operator_pos != std::string::npos &&
            result.find("::", operator_pos) == std::string::npos &&
            result.find('.', operator_pos) == std::string::npos;
        if (c == '<' && !operator_name) ++depth;
        else if (c == '>' && depth) --depth;
        else if (!depth && !std::isspace(static_cast<unsigned char>(c))) result += c;
    }
    return result;
}

inline std::string bare_name(const std::string& value) {
    auto normalized = without_templates(value);
    auto position = normalized.find_last_of(".:");
    auto arrow = normalized.rfind("->");
    size_t start = position == std::string::npos ? 0 : position + 1;
    if (arrow != std::string::npos) start = (std::max)(start, arrow + 2);
    return normalized.substr(start);
}

inline std::string type_name(std::string_view value) {
    auto name = trim(value);
    for (const auto* prefix : {"const ", "volatile ", "struct ", "class "}) {
        if (name.starts_with(prefix)) name = trim(std::string_view(name).substr(std::char_traits<char>::length(prefix)));
    }
    while (!name.empty() && (name.back() == '*' || name.back() == '&' ||
                            std::isspace(static_cast<unsigned char>(name.back())))) name.pop_back();
    if (name == "auto" || name == "decltype(auto)") return {};
    return without_templates(name);
}

inline std::string owner_scope(const std::string& qualname, const std::string& name = {}) {
    auto qualified = without_templates(qualname);
    auto unqualified = without_templates(name);
    if (!unqualified.empty() && qualified.ends_with(unqualified)) {
        auto end = qualified.size() - unqualified.size();
        if (end >= 2 && qualified.substr(end - 2, 2) == "::") return qualified.substr(0, end - 2);
        if (end >= 1 && qualified[end - 1] == '.') return qualified.substr(0, end - 1);
        return {};
    }
    auto scope_end = qualified.rfind("::");
    auto dot_end = qualified.rfind('.');
    if (dot_end != std::string::npos &&
        (scope_end == std::string::npos || dot_end > scope_end)) scope_end = dot_end;
    return scope_end == std::string::npos ? std::string() : qualified.substr(0, scope_end);
}

struct Target {
    std::string kind;
    std::string name;
    std::string qualname;
    std::string language;
    Arity arity;
};

inline Target target(std::string kind, std::string name, std::string qualname,
                     const std::string& signature, std::string language) {
    auto arity = signature_arity(signature, name);
    return {std::move(kind), std::move(name), std::move(qualname), std::move(language), arity};
}

inline bool same_owner(const std::string& owner, const std::string& requested, bool absolute = false) {
    if (owner.empty() || requested.empty()) return false;
    return owner == requested || (!absolute &&
        (owner.ends_with("::" + requested) || owner.ends_with("." + requested)));
}

inline bool compatible(const Target& target, const std::string& callee,
                       const std::string& receiver_type, int arguments,
                       const std::string& caller_language,
                       const std::unordered_set<std::string>& class_names,
                       const std::string& caller_qualname = {},
                       const std::unordered_set<std::string>& namespace_names = {}) {
    if (target.kind != "function" && target.kind != "method" &&
        target.kind != "constructor_fn") return false;
    if (!target.language.empty() && !caller_language.empty() &&
        target.language != caller_language &&
        !((target.language == "c" && caller_language == "cpp") ||
          (target.language == "cpp" && caller_language == "c"))) return false;
    const auto& arity = target.arity;
    bool implicit_self_language = target.language == "python" || target.language == "rust";
    if (!implicit_self_language && arguments >= 0 && arity.known &&
        (arguments < arity.minimum || (arity.maximum >= 0 && arguments > arity.maximum))) return false;
    auto owner = owner_scope(target.qualname, target.name);
    bool member = target.kind == "method" || class_names.contains(owner);
    bool cpp = caller_language == "cpp" || caller_language == "c";
    auto hint = type_name(receiver_type);
    if (!hint.empty() && (!owner.empty() || cpp)) return same_owner(owner, hint);
    auto call = without_templates(callee);
    bool member_call = call.find('.') != std::string::npos || call.find("->") != std::string::npos;
    if (member_call) return !cpp;
    auto call_scope = call.rfind("::");
    if (call_scope != std::string::npos) {
        if (call_scope == 0) return owner.empty() && !member;
        auto requested_owner = call.substr(0, call_scope);
        bool absolute = requested_owner.starts_with("::");
        while (requested_owner.starts_with("::")) requested_owner.erase(0, 2);
        return same_owner(owner, requested_owner, absolute);
    }
    auto caller_scope = owner_scope(caller_qualname);
    if (member) return same_owner(owner, caller_scope, true);
    if (cpp && !owner.empty())
        return owner == caller_scope ||
            (namespace_names.contains(owner) && caller_scope.starts_with(owner + "::"));
    return true;
}

} // namespace codetopo::call_binding
