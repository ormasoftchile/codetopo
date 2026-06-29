#pragma once

#include "db/workspace.h"
#include "core/config.h"
#include "util/log.h"
#include "util/repo.h"
#include <string>
#include <vector>
#include <iostream>
#include <filesystem>

namespace codetopo {

// codetopo workspace add    --root /myproject /path/to/add [--with-content-fts]
// codetopo workspace remove --root /myproject /path/to/remove
// codetopo workspace list   --root /myproject

inline int run_workspace_add(const std::string& root_str, const std::string& target_path,
                             const Config& cfg) {
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

    try {
        WorkspaceDB ws(db_path);
        auto result = ws.add_root(target_path, cfg);
        std::cout << "Added root: " << fs::canonical(target_path).string() << "\n"
                  << "  root_id: " << result.root_id << "\n"
                  << "  files:   " << format_with_commas(result.files) << "\n"
                  << "  symbols: " << format_with_commas(result.symbols) << "\n"
                  << "  edges:   " << format_with_commas(result.edges) << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << stderr_bold_red(std::string("ERROR: ") + e.what(), stderr_is_tty()) << "\n";
        return 1;
    }
}

inline int run_workspace_remove(const std::string& root_str, const std::string& target_path) {
    namespace fs = std::filesystem;

    auto repo_root = fs::canonical(root_str).string();
    auto db_path = default_db(repo_root);

    if (!fs::exists(db_path)) {
        std::cerr << stderr_bold_red("ERROR: No index.sqlite found at " + db_path, stderr_is_tty()) << "\n";
        return 1;
    }

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
