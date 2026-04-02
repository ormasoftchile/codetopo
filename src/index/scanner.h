#pragma once

#include "core/config.h"
#include "util/path.h"
#include <filesystem>
#include <vector>
#include <string>
#include <unordered_set>
#include <fstream>
#include <regex>
#include <functional>
#include <array>
#include <cstdio>

namespace codetopo {
namespace fs = std::filesystem;

// T025-T028: Repo scanner with file extension detection, gitignore,
// symlink resolution, and configurable directory exclusion.

struct ScannedFile {
    fs::path absolute_path;
    std::string relative_path;  // normalized: forward slashes, relative to root
    std::string language;
    int64_t size_bytes;
    int64_t mtime_ns;
};

// Simple gitignore pattern matcher (T026)
class GitignoreFilter {
public:
    void load(const fs::path& gitignore_path, const std::string& prefix = "") {
        std::ifstream f(gitignore_path);
        if (!f) return;

        std::string line;
        while (std::getline(f, line)) {
            // Strip trailing whitespace
            while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
                line.pop_back();

            if (line.empty() || line[0] == '#') continue;

            bool negated = false;
            if (line[0] == '!') {
                negated = true;
                line = line.substr(1);
            }

            // Prefix pattern with directory context
            std::string full_pattern = prefix.empty() ? line : prefix + "/" + line;

            patterns_.push_back({full_pattern, negated, line.back() == '/'});
        }
    }

    bool is_ignored(const std::string& rel_path, bool is_dir) const {
        bool ignored = false;
        for (const auto& p : patterns_) {
            if (p.dir_only && !is_dir) continue;

            std::string pattern = p.pattern;
            if (pattern.back() == '/') pattern.pop_back();

            if (matches_glob(rel_path, pattern)) {
                ignored = !p.negated;
            }
        }
        return ignored;
    }

private:
    struct Pattern {
        std::string pattern;
        bool negated;
        bool dir_only;
    };
    std::vector<Pattern> patterns_;

    static bool matches_glob(const std::string& path, const std::string& pattern) {
        // Handle ** (match any path segments)
        if (pattern.find("**") != std::string::npos) {
            // "**/" matches zero or more directories
            std::string p = pattern;
            // Replace **/ with regex .*
            // Simplified: if pattern contains **, check if path contains the non-** parts
            size_t star_pos = p.find("**");
            std::string before = p.substr(0, star_pos);
            std::string after = p.substr(star_pos + 2);
            if (!after.empty() && after[0] == '/') after = after.substr(1);

            if (before.empty() && after.empty()) return true;
            if (before.empty()) return path.find(after) != std::string::npos;
            if (after.empty()) return path.find(before) != std::string::npos;
            return path.find(before) != std::string::npos && path.find(after) != std::string::npos;
        }

        // Handle patterns without leading slash — match anywhere in path
        if (pattern.find('/') == std::string::npos) {
            // Match against filename or any path component
            size_t last_slash = path.rfind('/');
            std::string filename = (last_slash != std::string::npos)
                ? path.substr(last_slash + 1) : path;
            return matches_simple(filename, pattern) || matches_simple(path, pattern);
        }

        // Pattern with slashes — match from root
        std::string p = pattern;
        if (p[0] == '/') p = p.substr(1);

        return matches_simple(path, p);
    }

    static bool matches_simple(const std::string& str, const std::string& pattern) {
        // Simple glob: * matches any chars except /, ? matches single char
        return match_impl(str.c_str(), pattern.c_str());
    }

    static bool match_impl(const char* s, const char* p) {
        while (*p) {
            if (*p == '*') {
                p++;
                while (*s) {
                    if (*s == '/') return false;  // * doesn't cross /
                    if (match_impl(s, p)) return true;
                    s++;
                }
                return match_impl(s, p);
            }
            if (*p == '?') {
                if (!*s || *s == '/') return false;
                s++; p++;
            } else {
                if (*s != *p) return false;
                s++; p++;
            }
        }
        return *s == '\0';
    }
};

class Scanner {
public:
    explicit Scanner(const Config& config) : config_(config) {}

    std::vector<ScannedFile> scan() {
        auto root = fs::canonical(config_.repo_root);

        // Fast path: use git ls-files if this is a git repo (avoids walking dirs)
        if (!config_.no_gitignore && fs::exists(root / ".git")) {
            auto files = scan_via_git(root);
            if (!files.empty()) {
                if (config_.max_files > 0 && files.size() > static_cast<size_t>(config_.max_files))
                    files.resize(config_.max_files);
                return files;
            }
            // Fall back to manual scan if git ls-files failed
        }

        std::vector<ScannedFile> files;
        GitignoreFilter gitignore;
        if (!config_.no_gitignore) {
            load_gitignore_recursive(root, "", gitignore);
        }

        scan_directory(root, root, gitignore, files);
        if (config_.max_files > 0 && files.size() > static_cast<size_t>(config_.max_files))
            files.resize(config_.max_files);
        return files;
    }

    // Check if a relative path matches any --exclude pattern.
    // Patterns support: ** (any path segments), * (any chars except /), ? (single char).
    // A pattern without / is matched against the filename only.
    static bool matches_exclude(const std::string& rel_path,
                                const std::vector<std::string>& patterns) {
        if (patterns.empty()) return false;
        // Extract filename for patterns without path separators
        size_t last_slash = rel_path.rfind('/');
        std::string filename = (last_slash != std::string::npos)
            ? rel_path.substr(last_slash + 1) : rel_path;
        for (const auto& pat : patterns) {
            if (pat.find('/') == std::string::npos && pat.find("**") == std::string::npos) {
                // Filename-only pattern
                if (glob_match(filename, pat)) return true;
            } else {
                // Path pattern — handle **
                if (glob_match_path(rel_path, pat)) return true;
            }
        }
        return false;
    }

private:
    const Config& config_;

    // Simple glob: * matches any chars except /, ? matches single char
    static bool glob_match(const std::string& str, const std::string& pattern) {
        return glob_impl(str.c_str(), pattern.c_str());
    }

    static bool glob_impl(const char* s, const char* p) {
        while (*p) {
            if (*p == '*') {
                p++;
                while (*s) {
                    if (*s == '/') return false;
                    if (glob_impl(s, p)) return true;
                    s++;
                }
                return glob_impl(s, p);
            }
            if (*p == '?') {
                if (!*s || *s == '/') return false;
                s++; p++;
            } else {
                if (*s != *p) return false;
                s++; p++;
            }
        }
        return *s == '\0';
    }

    // Path-aware glob with ** support
    static bool glob_match_path(const std::string& path, const std::string& pattern) {
        // Handle ** — split pattern on first ** and check both parts
        auto dstar = pattern.find("**");
        if (dstar != std::string::npos) {
            std::string before = (dstar > 0 && pattern[dstar - 1] == '/')
                ? pattern.substr(0, dstar - 1) : pattern.substr(0, dstar);
            std::string after = pattern.substr(dstar + 2);
            if (!after.empty() && after[0] == '/') after = after.substr(1);

            if (before.empty() && after.empty()) return true;
            if (before.empty()) {
                // **/something — match against every suffix
                for (size_t i = 0; i <= path.size(); ++i) {
                    if (i == 0 || path[i - 1] == '/') {
                        if (glob_match(path.substr(i), after)) return true;
                    }
                }
                return false;
            }
            if (after.empty()) return path.find(before) != std::string::npos;
            // before/**/after
            for (size_t i = 0; i <= path.size(); ++i) {
                if (i == 0 || path[i - 1] == '/') {
                    if (glob_match(path.substr(0, i > 0 ? i - 1 : 0), before) &&
                        glob_match_path(path.substr(i), after)) return true;
                }
            }
            return false;
        }
        // No ** — direct match
        std::string p = pattern;
        if (!p.empty() && p[0] == '/') p = p.substr(1);
        return glob_match(path, p);
    }

    // Fast scan using git ls-files (respects .gitignore automatically)
    std::vector<ScannedFile> scan_via_git(const fs::path& root) {
        std::vector<ScannedFile> files;
        std::string cmd = "git -C \"" + root.string() + "\" ls-files -z --cached --others --exclude-standard";

#ifdef _WIN32
        FILE* pipe = _popen(cmd.c_str(), "rb");
#else
        FILE* pipe = popen(cmd.c_str(), "r");
#endif
        if (!pipe) return {};

        // Read null-delimited file list
        std::string buffer;
        char buf[4096];
        while (size_t n = fread(buf, 1, sizeof(buf), pipe)) {
            buffer.append(buf, n);
        }

#ifdef _WIN32
        int rc = _pclose(pipe);
#else
        int rc = pclose(pipe);
#endif
        if (rc != 0) return {};  // git failed, fall back to manual scan

        // Parse null-delimited paths
        size_t start = 0;
        while (start < buffer.size()) {
            size_t end = buffer.find('\0', start);
            if (end == std::string::npos) end = buffer.size();
            std::string rel(buffer, start, end - start);
            start = end + 1;

            if (rel.empty()) continue;

            auto language = path_util::detect_language(fs::path(rel));
            if (language.empty()) continue;

            auto abs_path = root / rel;
            std::error_code ec;
            auto size = fs::file_size(abs_path, ec);
            if (ec) continue;
            auto mtime = fs::last_write_time(abs_path, ec);
            if (ec) continue;
            auto mtime_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                mtime.time_since_epoch()).count();

            // Normalize path separators
            std::string norm_rel = rel;
            for (auto& c : norm_rel) { if (c == '\\') c = '/'; }

            // Check --exclude patterns
            if (matches_exclude(norm_rel, config_.exclude_patterns)) continue;

            files.push_back({
                abs_path,
                norm_rel,
                language,
                static_cast<int64_t>(size),
                mtime_ns
            });
        }
        return files;
    }

    // Default excluded directories
    static const std::unordered_set<std::string>& excluded_dirs() {
        static const std::unordered_set<std::string> dirs = {
            ".git", "build", "out", "node_modules",
            "vcpkg", "vcpkg_installed", "vendor", "third_party",
            "__pycache__", ".venv", "target"
        };
        return dirs;
    }

    static bool is_bazel_dir(const std::string& name) {
        return name.size() >= 6 && name.substr(0, 6) == "bazel-";
    }

    void scan_directory(const fs::path& dir, const fs::path& root,
                        const GitignoreFilter& gitignore,
                        std::vector<ScannedFile>& files) {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            auto name = entry.path().filename().string();

            if (entry.is_directory(ec)) {
                // Skip excluded directories
                if (excluded_dirs().count(name) || is_bazel_dir(name)) continue;

                // T027: Symlink resolution — skip symlinks pointing outside root
                if (entry.is_symlink(ec)) {
                    auto real = fs::canonical(entry.path(), ec);
                    if (ec) continue;
                    auto rel = fs::relative(real, root, ec);
                    if (ec || rel.string().starts_with("..")) continue;
                }

                // Check gitignore for directory
                auto rel_dir = fs::relative(entry.path(), root, ec).generic_string();
                if (!config_.no_gitignore && gitignore.is_ignored(rel_dir, true)) continue;

                scan_directory(entry.path(), root, gitignore, files);
            } else if (entry.is_regular_file(ec)) {
                // T027: Skip symlinks to outside repo
                if (entry.is_symlink(ec)) {
                    auto real = fs::canonical(entry.path(), ec);
                    if (ec) continue;
                    auto rel = fs::relative(real, root, ec);
                    if (ec || rel.string().starts_with("..")) continue;
                }

                auto language = path_util::detect_language(entry.path());
                if (language.empty()) continue;

                auto rel_path = path_util::normalize(entry.path(), root);
                if (rel_path.empty()) continue;

                // Check gitignore
                if (!config_.no_gitignore && gitignore.is_ignored(rel_path, false)) continue;

                // Check --exclude patterns
                if (matches_exclude(rel_path, config_.exclude_patterns)) continue;

                // Check file size limit
                auto size = entry.file_size(ec);
                if (ec) continue;

                auto mtime = entry.last_write_time(ec);
                if (ec) continue;
                auto mtime_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    mtime.time_since_epoch()).count();

                files.push_back({
                    fs::canonical(entry.path()),
                    rel_path,
                    language,
                    static_cast<int64_t>(size),
                    mtime_ns
                });
            }
        }
    }

    void load_gitignore_recursive(const fs::path& dir, const std::string& prefix,
                                   GitignoreFilter& filter) {
        auto gi_path = dir / ".gitignore";
        if (fs::exists(gi_path)) {
            filter.load(gi_path, prefix);
        }

        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (entry.is_directory(ec) && !excluded_dirs().count(entry.path().filename().string())) {
                auto sub_prefix = prefix.empty()
                    ? entry.path().filename().string()
                    : prefix + "/" + entry.path().filename().string();
                load_gitignore_recursive(entry.path(), sub_prefix, filter);
            }
        }
    }
};

} // namespace codetopo
