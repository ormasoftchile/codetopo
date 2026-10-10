#pragma once

#include "core/config.h"
#include "index/worktree_reconcile.h"
#include "util/git.h"
#include "util/repo.h"
#include <iostream>
#include <filesystem>
#include <string>

namespace codetopo {

inline int run_worktree_init(const std::string& root_str,
                             const std::string& from_str = "") {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto root = fs::canonical(root_str, ec);
    if (ec) root = fs::path(root_str).lexically_normal();

    auto wt_info = resolve_worktree_info(root);
    if (!wt_info.is_worktree) {
        std::cerr << "ERROR: '" << root.string() << "' is not a linked Git worktree.\n";
        return 1;
    }

    fs::path primary_root = from_str.empty() ? wt_info.primary_repo_root : fs::path(from_str);
    std::cout << "Bootstrapping CodeTopo intelligence for worktree:\n";
    std::cout << "  Worktree root:     " << root.string() << "\n";
    std::cout << "  Primary repo:      " << primary_root.string() << "\n";

    Config cfg;
    auto res = bootstrap_worktree_index(root, primary_root, cfg);
    if (!res.success) {
        std::cerr << "ERROR: " << res.error << "\n";
        return 1;
    }

    std::cout << "  Base commit:       " << res.base_head << "\n";
    std::cout << "  Current commit:    " << res.current_head << "\n";
    std::cout << "  Reconciled files:  " << res.files_changed << " changed\n";
    std::cout << "✓ Worktree index ready at: " << default_db(root.string()) << "\n";
    return 0;
}

inline int run_worktree_sync(const std::string& root_str) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto root = fs::canonical(root_str, ec);
    if (ec) root = fs::path(root_str).lexically_normal();

    auto wt_info = resolve_worktree_info(root);
    if (!wt_info.is_worktree) {
        std::cerr << "ERROR: '" << root.string() << "' is not a linked Git worktree.\n";
        return 1;
    }

    std::cout << "Syncing CodeTopo index for worktree: " << root.string() << "\n";
    Config cfg;
    auto res = reconcile_worktree_index(root, cfg);
    if (!res.success) {
        std::cerr << "ERROR: " << res.error << "\n";
        return 1;
    }

    std::cout << "  Base commit:       " << res.base_head << "\n";
    std::cout << "  Current commit:    " << res.current_head << "\n";
    std::cout << "  Reconciled files:  " << res.files_changed << " changed\n";
    std::cout << "✓ Worktree index synchronized.\n";
    return 0;
}

inline int run_worktree_status(const std::string& root_str) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto root = fs::canonical(root_str, ec);
    if (ec) root = fs::path(root_str).lexically_normal();

    auto wt_info = resolve_worktree_info(root);
    std::cout << "Worktree Status for: " << root.string() << "\n";
    std::cout << "  Git Repository:    " << (wt_info.is_git_repo ? "yes" : "no") << "\n";
    std::cout << "  Is Worktree:       " << (wt_info.is_worktree ? "yes" : "no") << "\n";

    if (!wt_info.is_git_repo) {
        return 0;
    }

    if (wt_info.is_worktree) {
        std::cout << "  Primary Repo:      " << wt_info.primary_repo_root.string() << "\n";
        std::cout << "  Common Git Dir:    " << wt_info.common_git_dir.string() << "\n";
        std::cout << "  Worktree Git Dir:  " << wt_info.git_dir.string() << "\n";
    }

    std::string db_path = default_db(root.string());
    bool db_exists = fs::exists(db_path, ec);
    std::cout << "  Index Database:    " << (db_exists ? "present" : "missing") << " (" << db_path << ")\n";

    if (db_exists) {
        try {
            Connection conn(db_path, true);
            auto stored_root = schema::get_kv(conn, "repo_root", "");
            auto indexed_head = schema::get_kv(conn, "git_head", "");
            auto indexed_branch = schema::get_kv(conn, "git_branch", "");
            auto seeded_from = schema::get_kv(conn, "worktree_parent_root", "");
            auto state = schema::get_kv(conn, "index_state", "");

            std::cout << "  Database Root:     " << stored_root << "\n";
            std::cout << "  Index State:       " << state << "\n";
            std::cout << "  Indexed Head:      " << indexed_head << "\n";
            std::cout << "  Indexed Branch:    " << indexed_branch << "\n";
            if (!seeded_from.empty()) {
                std::cout << "  Seeded From:       " << seeded_from << "\n";
            }

            auto current_head = get_git_head(root.string());
            auto changed = collect_worktree_changed_paths(root.string(), indexed_head);
            std::cout << "  Current Head:      " << current_head << "\n";
            std::cout << "  Divergent Files:   " << changed.size() << "\n";
        } catch (const std::exception& e) {
            std::cout << "  Error inspecting database: " << e.what() << "\n";
        }
    }

    return 0;
}

} // namespace codetopo
