#pragma once

#include <string>

namespace codetopo {

// Fast language identification enum — replaces string comparisons in hot paths.
enum class LanguageId {
    Unknown,
    C,
    Cpp,
    CSharp,
    Go,
    Yaml,
    TypeScript,
    JavaScript,
    Python,
    Rust,
    Java,
    Bash,
    Sql
};

// Map file extension string to LanguageId. Extension should include the dot.
inline LanguageId lang_id_from_extension(const std::string& ext) {
    if (ext.size() < 2) return LanguageId::Unknown;

    // Fast dispatch on second character (after '.') to minimize comparisons
    switch (ext[1]) {
        case 'c':
            if (ext == ".c") return LanguageId::C;
            if (ext == ".cpp" || ext == ".cc" || ext == ".cxx") return LanguageId::Cpp;
            if (ext == ".cs") return LanguageId::CSharp;
            break;
        case 'h':
            if (ext == ".h") return LanguageId::C;
            if (ext == ".hpp" || ext == ".hxx" || ext == ".hh") return LanguageId::Cpp;
            break;
        case 't':
            if (ext == ".ts" || ext == ".tsx") return LanguageId::TypeScript;
            break;
        case 'j':
            if (ext == ".js" || ext == ".jsx") return LanguageId::JavaScript;
            if (ext == ".java") return LanguageId::Java;
            break;
        case 'p':
            if (ext == ".py" || ext == ".pyi") return LanguageId::Python;
            break;
        case 'g':
            if (ext == ".go") return LanguageId::Go;
            break;
        case 'r':
            if (ext == ".rs") return LanguageId::Rust;
            break;
        case 's':
            if (ext == ".sh") return LanguageId::Bash;
            if (ext == ".sql") return LanguageId::Sql;
            break;
        case 'b':
            if (ext == ".bash") return LanguageId::Bash;
            break;
        case 'y':
            if (ext == ".yaml" || ext == ".yml") return LanguageId::Yaml;
            break;
        case 'm':
            if (ext == ".mjs") return LanguageId::JavaScript;
            break;
    }
    return LanguageId::Unknown;
}

// Map LanguageId to the language string used by scanner/DB.
inline const char* lang_id_to_string(LanguageId id) {
    switch (id) {
        case LanguageId::C:          return "c";
        case LanguageId::Cpp:        return "cpp";
        case LanguageId::CSharp:     return "csharp";
        case LanguageId::Go:         return "go";
        case LanguageId::Yaml:       return "yaml";
        case LanguageId::TypeScript: return "typescript";
        case LanguageId::JavaScript: return "javascript";
        case LanguageId::Python:     return "python";
        case LanguageId::Rust:       return "rust";
        case LanguageId::Java:       return "java";
        case LanguageId::Bash:       return "bash";
        case LanguageId::Sql:        return "sql";
        default:                     return "";
    }
}

// Map language string to LanguageId (for interop with existing string-based code).
inline LanguageId lang_id_from_string(const std::string& lang) {
    if (lang.empty()) return LanguageId::Unknown;
    switch (lang[0]) {
        case 'c':
            if (lang == "c") return LanguageId::C;
            if (lang == "cpp") return LanguageId::Cpp;
            if (lang == "csharp") return LanguageId::CSharp;
            break;
        case 'g':
            if (lang == "go") return LanguageId::Go;
            break;
        case 'y':
            if (lang == "yaml") return LanguageId::Yaml;
            break;
        case 't':
            if (lang == "typescript") return LanguageId::TypeScript;
            break;
        case 'j':
            if (lang == "javascript") return LanguageId::JavaScript;
            if (lang == "java") return LanguageId::Java;
            break;
        case 'p':
            if (lang == "python") return LanguageId::Python;
            break;
        case 'r':
            if (lang == "rust") return LanguageId::Rust;
            break;
        case 'b':
            if (lang == "bash") return LanguageId::Bash;
            break;
        case 's':
            if (lang == "sql") return LanguageId::Sql;
            break;
    }
    return LanguageId::Unknown;
}

} // namespace codetopo
