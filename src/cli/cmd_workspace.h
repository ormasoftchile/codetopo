#pragma once

#include "db/workspace.h"
#include "core/config.h"
#include "index/ownership.h"
#include "util/lock.h"
#include "util/log.h"
#include "util/repo.h"
#include <string>
#include <vector>
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <chrono>

namespace codetopo {

// codetopo workspace add    --root /myproject /path/to/add [--with-content-fts]
// codetopo workspace remove --root /myproject /path/to/remove
// codetopo workspace list   --root /myproject

inline bool acquire_workspace_lock(FileLock& lock, int lock_timeout_s) {
    const int timeout_s = std::max(0, lock_timeout_s);
    return lock.acquire_blocking(
        std::chrono::seconds(timeout_s),
        std::chrono::milliseconds(250),
        std::chrono::seconds(2),
        [timeout_s](int64_t pid, std::chrono::milliseconds) {
            std::cerr << "Waiting for index lock held by PID " << pid
                      << " (up to " << timeout_s << "s)...\n";
        });
}

inline void print_workspace_lock_error(const FileLock& lock, int lock_timeout_s) {
    const int timeout_s = std::max(0, lock_timeout_s);
    std::cerr << stderr_bold_red(
        "ERROR: Another codetopo process holds this index (PID " +
        std::to_string(lock.holder_pid()) + "). Stop the running "
        "MCP/watch/index process, then retry. (waited " +
        std::to_string(timeout_s) + "s)", stderr_is_tty()) << "\n";
}

inline bool validate_workspace_primary(
    const std::string& db_path,
    const std::string& repo_root) {
    try {
        Connection conn(db_path, true);
        auto resolution = index_ownership::resolve_primary_root(
            conn, repo_root, true, db_path);
        if (!resolution.metadata_present) {
            throw std::runtime_error(
                "Primary index has no repo_root ownership metadata. Complete a full "
                "primary index before changing extra workspace roots.");
        }
        return true;
    } catch (const std::exception& e) {
        std::cerr << stderr_bold_red(
            std::string("ERROR: ") + e.what(), stderr_is_tty()) << "\n";
        return false;
    }
}

inline int run_workspace_add(const std::string& root_str, const std::string& target_path,
                             const Config& cfg, int lock_timeout_s = 30) {
    namespace fs = std::filesystem;

    auto repo_root = fs::canonical(root_str).string();
    auto db_path = default_db(repo_root);
    ensure_codetopo_dir(repo_root);

    if (!fs::exists(db_path)) {
        std::cerr << stderr_bold_red("ERROR: No index.sqlite found at " + db_path +
                                     ". Run 'codetopo index --root " + repo_root + "' first.",
                                     stderr_is_tty())
                  << "\n";
        return 1;
    }
    // Acquire the same lock the indexer and MCP server use, so a running
    // `codetopo mcp --watch` (or index) can't write concurrently and corrupt
    // the DB / balloon the WAL during the merge.
    auto lock_path = db_path + ".lock";
    FileLock lock(lock_path);
    if (!acquire_workspace_lock(lock, lock_timeout_s)) {
        print_workspace_lock_error(lock, lock_timeout_s);
        return 1;
    }
    if (lock.was_stale_broken()) {
        std::cerr << "WARN: Broke stale lock from dead process\n";
    }
    if (!validate_workspace_primary(db_path, repo_root)) return 1;

    try {
        WorkspaceDB ws(db_path);
        auto result = ws.add_root(target_path, cfg);
        std::cout << "Added root: " << fs::canonical(target_path).string() << "\n"
                  << "  root_id: " << result.root_id << "\n"
                  << "  files:   " << format_with_commas(result.files) << "\n"
                  << "  symbols: " << format_with_commas(result.symbols) << "\n"
                  << "  edges:   " << format_with_commas(result.edges) << "\n"
                  << "  http:    " << format_with_commas(result.http_call_refs) << " http_call refs\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << stderr_bold_red(std::string("ERROR: ") + e.what(), stderr_is_tty()) << "\n";
        return 1;
    }
}

inline int run_workspace_remove(const std::string& root_str, const std::string& target_path,
                                int lock_timeout_s = 30) {
    namespace fs = std::filesystem;

    auto repo_root = fs::canonical(root_str).string();
    auto db_path = default_db(repo_root);

    if (!fs::exists(db_path)) {
        std::cerr << stderr_bold_red("ERROR: No index.sqlite found at " + db_path, stderr_is_tty()) << "\n";
        return 1;
    }
    auto lock_path = db_path + ".lock";
    FileLock lock(lock_path);
    if (!acquire_workspace_lock(lock, lock_timeout_s)) {
        print_workspace_lock_error(lock, lock_timeout_s);
        return 1;
    }
    if (lock.was_stale_broken()) {
        std::cerr << "WARN: Broke stale lock from dead process\n";
    }
    if (!validate_workspace_primary(db_path, repo_root)) return 1;

    try {
        WorkspaceDB ws(db_path);
        auto result = ws.remove_root(target_path);
        std::cout << "Removed root: " << target_path << "\n"
                  << "  files removed:   " << result.files << "\n"
                  << "  symbols removed: " << result.symbols << "\n"
                  << "  edges removed:   " << result.edges << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << stderr_bold_red(std::string("ERROR: ") + e.what(), stderr_is_tty()) << "\n";
        return 1;
    }
}

inline int run_workspace_list(const std::string& root_str) {
    namespace fs = std::filesystem;

    auto repo_root = fs::canonical(root_str).string();
    auto db_path = default_db(repo_root);

    if (!fs::exists(db_path)) {
        std::cout << "No index found (no index.sqlite found).\n";
        return 0;
    }
    try {
        FileLock writer(db_path + ".lock");
        if (!writer.acquire()) throw std::runtime_error("database busy: writer lock held");
        if (!validate_workspace_primary(db_path, repo_root)) return 1;
        WorkspaceDB ws(db_path);
        auto roots = ws.list_roots();

        if (roots.empty()) {
            std::cout << "No extra workspace roots configured.\n";
            return 0;
        }

        std::cout << "Workspace roots (" << roots.size() << "):\n";
        for (const auto& r : roots) {
            std::cout << "  [" << r.id << "] " << r.path
                      << " (" << r.files << " files, "
                      << r.symbols << " symbols, "
                      << r.edges << " edges)\n";
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << stderr_bold_red(std::string("ERROR: ") + e.what(), stderr_is_tty()) << "\n";
        return 1;
    }
}

} // namespace codetopo
