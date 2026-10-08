#pragma once

#include "core/config.h"
#include "util/path.h"
#include "index/scan_progress.h"
#include <filesystem>
#include <vector>
#include <string>
#include <unordered_set>
#include <fstream>
#include <regex>
#include <functional>
#include <array>
#include <cstdio>
#include <optional>
#include <utility>
#include <unordered_map>
#include <string_view>
#include <system_error>
#include <algorithm>
#include <cctype>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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

struct ScanMetrics {
    uint64_t directory_enumerations = 0;
    uint64_t canonicalizations = 0;
    uint64_t metadata_probes = 0;
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
            if (line.empty()) continue;

            // Prefix pattern with directory context
            std::string full_pattern = prefix.empty() ? line : prefix + "/" + line;
            bool directory_only = line.back() == '/';
            if (!full_pattern.empty() && full_pattern.back() == '/') full_pattern.pop_back();
            Pattern pattern{std::move(full_pattern), negated, directory_only};
            auto star = pattern.pattern.find("**");
            if (star != std::string::npos) {
                pattern.recursive = true;
                pattern.before = pattern.pattern.substr(0, star);
                pattern.after = pattern.pattern.substr(star + 2);
                if (!pattern.after.empty() && pattern.after.front() == '/') {
                    pattern.after.erase(0, 1);
                }
            } else {
                pattern.filename_only = pattern.pattern.find('/') == std::string::npos;
                if (!pattern.filename_only && !pattern.pattern.empty() &&
                    pattern.pattern.front() == '/') {
                    pattern.pattern.erase(0, 1);
                }
            }
            patterns_.push_back(std::move(pattern));
        }
    }

    bool is_ignored(const std::string& rel_path, bool is_dir) const {
        bool ignored = false;
        auto slash = rel_path.rfind('/');
        auto filename = std::string_view(rel_path).substr(
            slash == std::string::npos ? 0 : slash + 1);
        for (const auto& p : patterns_) {
            if (p.dir_only && !is_dir) continue;

            if (matches_glob(rel_path, filename, p)) {
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
        bool recursive = false;
        bool filename_only = false;
        std::string before;
        std::string after;
    };
    std::vector<Pattern> patterns_;

    static bool matches_glob(const std::string& path, std::string_view filename,
                             const Pattern& pattern) {
        if (pattern.recursive) {
            if (pattern.before.empty() && pattern.after.empty()) return true;
            if (pattern.before.empty()) return path.find(pattern.after) != std::string::npos;
            if (pattern.after.empty()) return path.find(pattern.before) != std::string::npos;
            return path.find(pattern.before) != std::string::npos &&
                   path.find(pattern.after) != std::string::npos;
        }

        // Handle patterns without leading slash — match anywhere in path
        if (pattern.filename_only) {
            return matches_simple(filename, pattern.pattern) ||
                   matches_simple(path, pattern.pattern);
        }

        // Pattern with slashes — match from root
        return matches_simple(path, pattern.pattern);
    }

    static bool matches_simple(std::string_view str, std::string_view pattern) {
        return match_impl(str, pattern, 0, 0);
    }

    static bool match_impl(std::string_view str, std::string_view pattern,
                           size_t s, size_t p) {
        while (p < pattern.size()) {
            if (pattern[p] == '*') {
                ++p;
                while (s < str.size()) {
                    if (str[s] == '/') return false;
                    if (match_impl(str, pattern, s, p)) return true;
                    ++s;
                }
                return match_impl(str, pattern, s, p);
            }
            if (pattern[p] == '?') {
                if (s == str.size() || str[s] == '/') return false;
                ++s;
                ++p;
            } else {
                if (s == str.size() || str[s] != pattern[p]) return false;
                ++s;
                ++p;
            }
        }
        return s == str.size();
    }
};

class Scanner {
public:
    using ProgressCallback = std::function<void(const ScanProgress&)>;

    explicit Scanner(const Config& config, ProgressCallback progress = {})
        : config_(config), progress_callback_(std::move(progress)) {}

    std::vector<ScannedFile> scan() {
        metrics_ = {};
        auto root = canonical_path(config_.repo_root);
        directory_cache_.clear();

        // Fast path: use git ls-files if this is a git repo (avoids walking dirs).
        set_phase("checking_git");
        if (!config_.no_gitignore && fs::exists(root / ".git")) {
            set_phase("git_listing");
            auto files = scan_via_git(root);
            if (!files.empty()) {
                progress_.sources = files.size();
                set_phase("complete", false);
                return files;
            }
            // Fall back to manual scan if git ls-files failed
        }

        std::vector<ScannedFile> files;
        GitignoreFilter gitignore;
        if (!config_.no_gitignore) {
            set_phase("loading_gitignore");
            std::unordered_set<std::string> ancestors;
            load_gitignore_recursive(root, root, "", gitignore, ancestors);
        }

        set_phase("filesystem");
        std::unordered_set<std::string> ancestors;
        scan_directory(root, root, gitignore, files, ancestors);
        directory_cache_.clear();
        set_phase("complete", false);
        return files;
    }

    std::vector<ScannedFile> scan_paths(const std::vector<std::string>& paths,
                                        std::vector<std::string>& deleted_paths) {
        metrics_ = {};
        auto root = canonical_path(config_.repo_root);
        std::vector<ScannedFile> files;
        std::unordered_set<std::string> seen;
        set_phase("targeted");

        for (const auto& raw : paths) {
            count_entry();
            auto rel_opt = normalize_target_path(root, raw);
            if (!rel_opt) continue;
            std::string rel = *rel_opt;
            if (!seen.insert(rel).second) continue;

            if (rel.rfind(".codetopo/", 0) == 0 || rel.rfind(".git/", 0) == 0)
                continue;
            if (matches_exclude(rel, config_.exclude_patterns)) continue;

            auto abs_path = root / fs::path(rel);
            std::error_code ec;
            if (!fs::exists(abs_path, ec)) {
                deleted_paths.push_back(rel);
                continue;
            }
            if (!fs::is_regular_file(abs_path, ec)) continue;

            auto language = path_util::detect_language(fs::path(rel));
            if (language.empty()) continue;

            auto size = fs::file_size(abs_path, ec);
            ++metrics_.metadata_probes;
            if (ec) continue;
            auto mtime = fs::last_write_time(abs_path, ec);
            ++metrics_.metadata_probes;
            if (ec) continue;
            auto mtime_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                mtime.time_since_epoch()).count();

            files.push_back({
                abs_path,
                rel,
                language,
                static_cast<int64_t>(size),
                mtime_ns
            });
            ++progress_.sources;
        }

        set_phase("complete", false);
        return files;
    }

    const ScanMetrics& metrics() const { return metrics_; }

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
    ProgressCallback progress_callback_;
    ScanProgress progress_;
    std::chrono::steady_clock::time_point last_progress_;
    ScanMetrics metrics_;

    struct DirectoryEntry {
        fs::path path;
        bool directory = false;
        bool regular = false;
        bool reparse = false;
        int64_t size_bytes = 0;
        int64_t mtime_ns = 0;
    };
    std::unordered_map<fs::path, std::vector<DirectoryEntry>> directory_cache_;

    fs::path canonical_path(const fs::path& path) {
        ++metrics_.canonicalizations;
        return fs::canonical(path);
    }

    static std::string relative_in_root(const fs::path& path, const fs::path& root) {
#ifdef _WIN32
        const auto& full = path.native();
        auto prefix = root.native();
        while (prefix.size() > root.root_path().native().size() &&
               (prefix.back() == L'\\' || prefix.back() == L'/')) {
            prefix.pop_back();
        }
        if (full.size() < prefix.size() ||
            CompareStringOrdinal(full.data(), static_cast<int>(prefix.size()),
                                 prefix.data(), static_cast<int>(prefix.size()), TRUE) != CSTR_EQUAL) {
            return {};
        }
        if (full.size() == prefix.size()) return ".";
        size_t offset = prefix.size();
        if (prefix.back() != L'\\' && prefix.back() != L'/') {
            if (full[offset] != L'\\' && full[offset] != L'/') return {};
            ++offset;
        }
        return fs::path(full.substr(offset)).generic_string();
#else
        auto relative = path.lexically_relative(root);
        if (relative.empty() || relative.is_absolute()) return {};
        for (const auto& component : relative) {
            if (component == "..") return {};
        }
        return relative.generic_string();
#endif
    }

    static std::string directory_identity(const fs::path& path) {
        auto value = path.generic_string();
#ifdef _WIN32
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
        return value;
    }

    const std::vector<DirectoryEntry>& directory_entries(const fs::path& directory) {
        auto cached = directory_cache_.find(directory);
        if (cached != directory_cache_.end()) return cached->second;
        ++metrics_.directory_enumerations;
        std::vector<DirectoryEntry> entries;
#ifdef _WIN32
        WIN32_FIND_DATAW data{};
        auto search = directory / L"*";
        HANDLE raw = FindFirstFileExW(search.c_str(), FindExInfoBasic, &data,
            FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (raw == INVALID_HANDLE_VALUE) {
            DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND) {
                throw fs::filesystem_error("Cannot enumerate index root directory",
                    directory, std::error_code(static_cast<int>(error), std::system_category()));
            }
            return directory_cache_.emplace(directory, std::move(entries)).first->second;
        }
        struct FindHandle {
            HANDLE handle;
            ~FindHandle() { FindClose(handle); }
        } handle{raw};
        do {
            std::wstring_view name(data.cFileName);
            if (name == L"." || name == L"..") continue;
            DirectoryEntry entry;
            entry.path = directory / data.cFileName;
            entry.directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            entry.regular = !entry.directory;
            entry.reparse = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            entry.size_bytes = static_cast<int64_t>(
                (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow);
            if (entry.regular && !entry.reparse) {
                uint64_t timestamp =
                    (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                    data.ftLastWriteTime.dwLowDateTime;
                constexpr uint64_t windows_epoch = 116444736000000000ULL;
                int64_t since_epoch = timestamp >= windows_epoch
                    ? static_cast<int64_t>(timestamp - windows_epoch)
                    : -static_cast<int64_t>(windows_epoch - timestamp);
                using WindowsTicks = std::chrono::duration<int64_t, std::ratio<1, 10000000>>;
                auto system_time = std::chrono::sys_time<WindowsTicks>{WindowsTicks{since_epoch}};
#ifdef __GLIBCXX__
                // MinGW libstdc++ last_write_time uses stat's whole-second precision.
                // Match existing index timestamps so unchanged files keep the fast path.
                system_time = std::chrono::floor<std::chrono::seconds>(system_time);
#endif
                auto file_time = fs::file_time_type::clock::from_sys(system_time);
                entry.mtime_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    file_time.time_since_epoch()).count();
            }
            entries.push_back(std::move(entry));
        } while (FindNextFileW(handle.handle, &data));
        DWORD error = GetLastError();
        if (error != ERROR_NO_MORE_FILES) {
            throw fs::filesystem_error("Index root enumeration was interrupted",
                directory, std::error_code(static_cast<int>(error), std::system_category()));
        }
#else
        for (const auto& item : fs::directory_iterator(directory)) {
            DirectoryEntry entry;
            entry.path = item.path();
            entry.reparse = item.is_symlink();
            entry.directory = item.is_directory();
            entry.regular = item.is_regular_file();
            if (entry.regular && !entry.reparse) {
                metrics_.metadata_probes += 2;
                entry.size_bytes = static_cast<int64_t>(item.file_size());
                entry.mtime_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    item.last_write_time().time_since_epoch()).count();
            }
            entries.push_back(std::move(entry));
        }
#endif
        return directory_cache_.emplace(directory, std::move(entries)).first->second;
    }

    void set_phase(const std::string& phase, bool reset = true) {
        if (reset) progress_ = {};
        progress_.phase = phase;
        last_progress_ = std::chrono::steady_clock::now();
        if (progress_callback_) progress_callback_(progress_);
    }

    void count_entry() {
        ++progress_.entries;
        if (!progress_callback_) return;
        auto now = std::chrono::steady_clock::now();
        if (progress_.entries % 500 == 0 || now - last_progress_ >= std::chrono::seconds(1)) {
            last_progress_ = now;
            progress_callback_(progress_);
        }
    }

    static std::optional<std::string> normalize_target_path(const fs::path& root,
                                                            const std::string& raw_path) {
        if (raw_path.empty()) return std::nullopt;

        fs::path input(raw_path);
        fs::path rel;
        if (input.is_absolute()) {
            auto norm_root = root.lexically_normal();
            auto norm_input = input.lexically_normal();
            auto root_s = norm_root.generic_string();
            auto input_s = norm_input.generic_string();
            if (input_s != root_s &&
                (input_s.size() <= root_s.size() || input_s.compare(0, root_s.size(), root_s) != 0 ||
                 input_s[root_s.size()] != '/')) {
                return std::nullopt;
            }
            rel = norm_input.lexically_relative(norm_root);
        } else {
            rel = input.lexically_normal();
        }

        auto rel_s = rel.generic_string();
        while (rel_s.rfind("./", 0) == 0) rel_s.erase(0, 2);
        if (rel_s.empty() || rel_s == "." || rel_s.rfind("../", 0) == 0 || rel_s == "..")
            return std::nullopt;
        return rel_s;
    }

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
            count_entry();
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
            ++metrics_.metadata_probes;
            if (ec) continue;
            auto mtime = fs::last_write_time(abs_path, ec);
            ++metrics_.metadata_probes;
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
            ++progress_.sources;
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
                        std::vector<ScannedFile>& files,
                        std::unordered_set<std::string>& ancestors) {
        auto identity = directory_identity(dir);
        if (!ancestors.insert(identity).second) return;
        struct LeaveDirectory {
            std::unordered_set<std::string>& ancestors;
            std::string identity;
            ~LeaveDirectory() { ancestors.erase(identity); }
        } leave{ancestors, identity};
        ++progress_.directories;
        for (const auto& entry : directory_entries(dir)) {
            count_entry();
            auto name = entry.path.filename().string();

            if (entry.directory) {
                // Skip excluded directories
                if (excluded_dirs().count(name) || is_bazel_dir(name)) continue;

                auto real = entry.reparse ? canonical_path(entry.path) : entry.path;
                auto rel_dir = relative_in_root(real, root);
                if (rel_dir.empty()) continue;
                if (!config_.no_gitignore && gitignore.is_ignored(rel_dir, true)) continue;

                scan_directory(real, root, gitignore, files, ancestors);
            } else if (entry.regular) {
                auto language = path_util::detect_language(entry.path);
                if (language.empty()) continue;

                auto real = entry.reparse ? canonical_path(entry.path) : entry.path;
                auto rel_path = relative_in_root(real, root);
                if (rel_path.empty()) continue;

                // Check gitignore
                if (!config_.no_gitignore && gitignore.is_ignored(rel_path, false)) continue;

                // Check --exclude patterns
                if (matches_exclude(rel_path, config_.exclude_patterns)) continue;

                auto size = entry.size_bytes;
                auto mtime_ns = entry.mtime_ns;
                if (entry.reparse) {
                    metrics_.metadata_probes += 2;
                    size = static_cast<int64_t>(fs::file_size(real));
                    mtime_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        fs::last_write_time(real).time_since_epoch()).count();
                }

                files.push_back({
                    real,
                    rel_path,
                    language,
                    size,
                    mtime_ns
                });
                ++progress_.sources;
            }
        }
    }

    void load_gitignore_recursive(const fs::path& dir, const fs::path& root,
                                   const std::string& prefix, GitignoreFilter& filter,
                                   std::unordered_set<std::string>& ancestors) {
        auto identity = directory_identity(dir);
        if (!ancestors.insert(identity).second) return;
        struct LeaveDirectory {
            std::unordered_set<std::string>& ancestors;
            std::string identity;
            ~LeaveDirectory() { ancestors.erase(identity); }
        } leave{ancestors, identity};
        ++progress_.directories;
        const auto& entries = directory_entries(dir);
        for (const auto& entry : entries) {
            auto name = entry.path.filename();
#ifdef _WIN32
            bool ignore_file = CompareStringOrdinal(
                name.c_str(), -1, L".gitignore", -1, TRUE) == CSTR_EQUAL;
#else
            bool ignore_file = name == ".gitignore";
#endif
            if (ignore_file) {
                auto real = entry.reparse ? canonical_path(entry.path) : entry.path;
                if (!relative_in_root(real, root).empty()) filter.load(real, prefix);
                break;
            }
        }
        for (const auto& entry : entries) {
            count_entry();
            if (entry.directory && !excluded_dirs().count(entry.path.filename().string())) {
                auto real = entry.reparse ? canonical_path(entry.path) : entry.path;
                if (relative_in_root(real, root).empty()) continue;
                auto sub_prefix = prefix.empty()
                    ? entry.path.filename().string()
                    : prefix + "/" + entry.path.filename().string();
                load_gitignore_recursive(real, root, sub_prefix, filter, ancestors);
            }
        }
    }
};

} // namespace codetopo
