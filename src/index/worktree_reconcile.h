#pragma once

#include "core/config.h"
#include "db/connection.h"
#include "db/schema.h"
#include "db/cow_seed.h"
#include "util/git.h"
#include "util/repo.h"
#include "util/path.h"
#include "util/log.h"
#include "cli/cmd_index.h"
#include <filesystem>
#include <string>
#include <vector>
#include <unordered_set>
#include <chrono>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <iostream>

namespace codetopo {

struct WorktreeReconcileResult {
    bool success = false;
    int files_changed = 0;
    std::string base_head;
    std::string current_head;
    std::string error;
};

// Split git output into line-by-line paths
inline std::vector<std::string> split_worktree_paths(const std::string& output) {
    std::vector<std::string> paths;
    std::istringstream in(output);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) paths.push_back(line);
    }
    return paths;
}

// Collect all files modified, added, deleted, or untracked in worktree relative to base_commit
inline std::vector<std::string> collect_worktree_changed_paths(
    const std::string& worktree_root,
    const std::string& base_commit) {
    std::vector<std::string> paths;
    std::unordered_set<std::string> seen;

    auto is_relevant = [](const std::string& rel) {
        if (rel.rfind(".codetopo/", 0) == 0 || rel.rfind(".git/", 0) == 0) return false;
        return !path_util::detect_language(std::filesystem::path(rel)).empty();
    };

    if (!base_commit.empty()) {
        // Tracked files changed between base_commit and current working tree
        auto diff_out = git_command(worktree_root, "diff --name-only " + base_commit);
        auto diff_paths = split_worktree_paths(diff_out);
        for (const auto& p : diff_paths) {
            if (is_relevant(p) && seen.insert(p).second) paths.push_back(p);
        }
    } else {
        auto diff_out = git_command(worktree_root, "diff --name-only HEAD");
        auto diff_paths = split_worktree_paths(diff_out);
        for (const auto& p : diff_paths) {
            if (is_relevant(p) && seen.insert(p).second) paths.push_back(p);
        }
    }

    // Capture untracked files from git status --porcelain -u
    std::string status_out = git_command(worktree_root, "status --porcelain -u");
    std::istringstream ss(status_out);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.size() >= 4 && line[0] == '?' && line[1] == '?') {
            std::string p = line.substr(3);
            while (!p.empty() && (p.back() == '\r' || p.back() == '\n' || p.back() == ' '))
                p.pop_back();
            if (!p.empty() && is_relevant(p) && seen.insert(p).second) {
                paths.push_back(p);
            }
        }
    }

    return paths;
}

// Bulk update mtime_ns for unchanged files so subsequent full scans hit mtime fast path
inline void align_unchanged_files_mtime(Connection& conn,
                                        const std::filesystem::path& worktree_root,
                                        const std::unordered_set<std::string>& changed_paths) {
    namespace fs = std::filesystem;
    sqlite3_stmt* select_stmt = nullptr;
    if (sqlite3_prepare_v2(conn.raw(),
            "SELECT id, path FROM files WHERE root_id IS NULL",
            -1, &select_stmt, nullptr) != SQLITE_OK) {
        return;
    }

    struct Item {
        int64_t id;
        int64_t mtime_ns;
    };
    std::vector<Item> updates;

    while (sqlite3_step(select_stmt) == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(select_stmt, 0);
        const char* p = reinterpret_cast<const char*>(sqlite3_column_text(select_stmt, 1));
        if (!p) continue;
        std::string rel_path(p);
        if (changed_paths.count(rel_path)) continue; // skip files that changed

        std::error_code ec;
        auto abs_path = worktree_root / fs::path(rel_path);
        auto mtime = fs::last_write_time(abs_path, ec);
        if (ec) continue;

        int64_t mtime_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            mtime.time_since_epoch()).count();
        updates.push_back({id, mtime_ns});
    }
    sqlite3_finalize(select_stmt);

    if (updates.empty()) return;

    conn.exec("BEGIN TRANSACTION");
    sqlite3_stmt* update_stmt = nullptr;
    if (sqlite3_prepare_v2(conn.raw(),
            "UPDATE files SET mtime_ns = ?1 WHERE id = ?2",
            -1, &update_stmt, nullptr) == SQLITE_OK) {
        for (const auto& item : updates) {
            sqlite3_bind_int64(update_stmt, 1, item.mtime_ns);
            sqlite3_bind_int64(update_stmt, 2, item.id);
            sqlite3_step(update_stmt);
            sqlite3_reset(update_stmt);
        }
        sqlite3_finalize(update_stmt);
    }
    conn.exec("COMMIT");
}

// Bootstrap an index in a Git worktree from the primary repository using CoW cloning
// and fast targeted reconciliation.
inline WorktreeReconcileResult bootstrap_worktree_index(
    const std::filesystem::path& worktree_root,
    const std::filesystem::path& primary_repo_override = {},
    const Config& base_cfg = {}) {
    namespace fs = std::filesystem;
    WorktreeReconcileResult result;

    std::error_code ec;
    auto canon_wt = fs::canonical(worktree_root, ec);
    if (ec) canon_wt = worktree_root.lexically_normal();

    auto wt_info = resolve_worktree_info(canon_wt);
    if (!wt_info.is_worktree) {
        result.error = "Directory is not a linked Git worktree: " + canon_wt.string();
        return result;
    }

    fs::path primary_root = primary_repo_override.empty()
        ? wt_info.primary_repo_root
        : primary_repo_override;
    auto canon_primary = fs::canonical(primary_root, ec);
    if (ec) canon_primary = primary_root.lexically_normal();

    std::string src_db = default_db(canon_primary.string());
    if (!fs::exists(src_db, ec)) {
        result.error = "Primary repository index not found at: " + src_db +
                       ". Run 'codetopo index' or 'codetopo init' in the primary repository first.";
        return result;
    }

    std::string dst_db = default_db(canon_wt.string());
    ensure_codetopo_dir(canon_wt.string());

    // 1. CoW clone the database (< 5ms on APFS / Linux Btrfs)
    if (!clone_database_cow(src_db, dst_db)) {
        result.error = "Failed to clone database from " + src_db + " to " + dst_db;
        return result;
    }

    // 2. Read base git_head and re-stamp ownership metadata in the cloned DB
    std::string base_head;
    {
        Connection conn(dst_db);
        base_head = schema::get_kv(conn, "git_head", "");
        schema::set_kv(conn, "repo_root", canon_wt.string());
        schema::set_kv(conn, "worktree_parent_root", canon_primary.string());

        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf{};
#ifdef _WIN32
        gmtime_s(&tm_buf, &t);
#else
        gmtime_r(&t, &tm_buf);
#endif
        std::ostringstream ss;
        ss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%SZ");
        schema::set_kv(conn, "worktree_seeded_at", ss.str());
    }

    result.base_head = base_head;
    result.current_head = get_git_head(canon_wt.string());

    // 3. Find changed files relative to base commit
    auto changed_files = collect_worktree_changed_paths(canon_wt.string(), base_head);
    result.files_changed = static_cast<int>(changed_files.size());
    std::unordered_set<std::string> changed_set(changed_files.begin(), changed_files.end());

    // 4. Align mtimes of unchanged files
    {
        Connection conn(dst_db);
        align_unchanged_files_mtime(conn, canon_wt, changed_set);
    }

    // 5. Reconcile changes (if any files changed, or write final metadata)
    Config cfg = base_cfg;
    cfg.repo_root = canon_wt;
    cfg.db_path = dst_db;
    cfg.only_files = changed_files;

    int rc = run_index(cfg);
    if (rc != 0 && rc != 1) {
        result.error = "Targeted reconciliation failed with exit code: " + std::to_string(rc);
        return result;
    }

    result.success = true;
    return result;
}

// Fast reconcile an existing worktree index against current working tree / commit
inline WorktreeReconcileResult reconcile_worktree_index(
    const std::filesystem::path& worktree_root,
    const Config& base_cfg = {}) {
    namespace fs = std::filesystem;
    WorktreeReconcileResult result;

    std::error_code ec;
    auto canon_wt = fs::canonical(worktree_root, ec);
    if (ec) canon_wt = worktree_root.lexically_normal();

    std::string db_path = default_db(canon_wt.string());
    if (!fs::exists(db_path, ec)) {
        // If index doesn't exist, bootstrap it
        return bootstrap_worktree_index(canon_wt, {}, base_cfg);
    }

    std::string base_head;
    {
        Connection conn(db_path, true);
        base_head = schema::get_kv(conn, "git_head", "");
    }

    result.base_head = base_head;
    result.current_head = get_git_head(canon_wt.string());

    auto changed_files = collect_worktree_changed_paths(canon_wt.string(), base_head);
    result.files_changed = static_cast<int>(changed_files.size());

    Config cfg = base_cfg;
    cfg.repo_root = canon_wt;
    cfg.db_path = db_path;
    cfg.only_files = changed_files;

    int rc = run_index(cfg);
    if (rc != 0 && rc != 1) {
        result.error = "Reconciliation failed with exit code: " + std::to_string(rc);
        return result;
    }

    result.success = true;
    return result;
}

} // namespace codetopo
