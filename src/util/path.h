#pragma once

#include <string>
#include <filesystem>

namespace codetopo {
namespace path_util {

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
    if (ext == ".go") return "go";
    if (ext == ".yaml" || ext == ".yml") return "yaml";

    return "";
}

} // namespace path_util
} // namespace codetopo
