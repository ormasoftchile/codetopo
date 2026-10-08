#pragma once

#include <string>
#include <filesystem>
#include <algorithm>
#include <cctype>

namespace codetopo {
namespace path_util {

// Lookup normalization is lexical: indexed files need not still exist on disk.
// JSON decoding happens at the transport boundary, never here.
inline std::string lookup_path(std::string path) {
#ifdef _WIN32
    for (auto& ch : path) if (ch == '\\') ch = '/';
#endif
    if (path.empty() || path.find('\0') != std::string::npos) return "";
    auto parsed = std::filesystem::path(path);
    for (const auto& component : parsed) {
        if (component == "..") return "";
    }
#ifdef _WIN32
    if (parsed.has_root_name() && !parsed.is_absolute()) return "";
#endif
    auto result = parsed.lexically_normal().generic_string();
    while (result.size() > 1 && result.back() == '/' &&
           result != parsed.root_path().generic_string()) result.pop_back();
    return result;
}

inline std::string lookup_key(const std::string& path) {
    auto result = lookup_path(path);
#ifdef _WIN32
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return result;
}

inline bool lookup_paths_equal(const std::string& left, const std::string& right) {
    auto a = lookup_key(left);
    auto b = lookup_key(right);
    return !a.empty() && a == b;
}

// An absolute lookup under a root maps to its indexed relative spelling.
inline std::string lookup_path_in_root(const std::string& path, const std::string& repo_root) {
    auto input = lookup_path(path);
    auto root = lookup_path(repo_root);
    if (input.empty() || root.empty() || !std::filesystem::path(input).is_absolute()) return {};
    auto input_key = lookup_key(input);
    auto root_key = lookup_key(root);
    if (input_key == root_key) return ".";
    if (root.back() != '/') root += '/';
    if (root_key.back() != '/') root_key += '/';
    return input_key.starts_with(root_key) ? input.substr(root.size()) : std::string();
}

// T012: Path normalization — forward slashes, relative to repo root,
// symlink resolution, traversal guard.

// Normalize a path to forward slashes, relative to repo_root.
// Returns empty string if path is outside repo root.
inline std::string normalize(const std::filesystem::path& file_path,
                              const std::filesystem::path& repo_root) {
    std::error_code ec;

    // Resolve symlinks
    auto canonical_file = std::filesystem::canonical(file_path, ec);
    if (ec) return "";

    auto canonical_root = std::filesystem::canonical(repo_root, ec);
    if (ec) return "";

    // Check file is under repo root
    auto rel = std::filesystem::relative(canonical_file, canonical_root, ec);
    if (ec) return "";

    std::string result = rel.generic_string();  // forward slashes

    // Reject paths that escape the root (start with "..")
    if (result.starts_with("..")) return "";

    // Reject leading "./"
    if (result.starts_with("./")) result = result.substr(2);

    return result;
}

// Validate an MCP-provided path: must be relative, no "..", no absolute.
// Returns the validated relative path or empty on rejection.
inline std::string validate_mcp_path(const std::string& path,
                                       const std::filesystem::path& repo_root) {
    // Reject absolute paths
    if (std::filesystem::path(path).is_absolute()) return "";

    // Reject ".." components before canonicalization
    if (path.find("..") != std::string::npos) return "";

    auto full = repo_root / path;
    std::error_code ec;
    auto canonical = std::filesystem::canonical(full, ec);
    if (ec) return "";

    auto canonical_root = std::filesystem::canonical(repo_root, ec);
    if (ec) return "";

    // Must be under repo root
    auto rel = std::filesystem::relative(canonical, canonical_root, ec);
    if (ec) return "";

    std::string result = rel.generic_string();
    if (result.starts_with("..")) return "";

    // Must be a regular file
    if (!std::filesystem::is_regular_file(canonical, ec)) return "";

    return result;
}

// Detect language from file extension.
// Returns empty string for unsupported extensions.
inline std::string detect_language(const std::filesystem::path& file_path) {
    auto ext = file_path.extension().string();

    if (ext == ".c") return "c";
    if (ext == ".h") return "cpp";  // C++ grammar is a superset of C; needed for class/namespace extraction
    if (ext == ".cc" || ext == ".cpp" || ext == ".cxx") return "cpp";
    if (ext == ".hpp" || ext == ".hh" || ext == ".hxx" || ext == ".inl") return "cpp";
    if (ext == ".cs") return "csharp";
    if (ext == ".ts" || ext == ".tsx") return "typescript";
    if (ext == ".js" || ext == ".jsx" || ext == ".mjs" || ext == ".cjs") return "javascript";
    if (ext == ".py" || ext == ".pyw") return "python";
    if (ext == ".rs") return "rust";
    if (ext == ".java") return "java";
    if (ext == ".go") return "go";
    if (ext == ".sh" || ext == ".bash") return "bash";
    if (ext == ".ps1" || ext == ".psm1" || ext == ".psd1") return "powershell";
    if (ext == ".bat" || ext == ".cmd") return "batch";
    if (ext == ".sql" || ext == ".tsql") return "sql";
    if (ext == ".yaml" || ext == ".yml") return "yaml";

    return "";
}

} // namespace path_util
} // namespace codetopo
