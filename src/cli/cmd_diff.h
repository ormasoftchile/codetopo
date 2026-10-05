#pragma once

#include "index/diff.h"
#include "util/repo.h"
#include "db/connection.h"
#include <iostream>
#include <filesystem>
#include <unistd.h>

namespace codetopo {

inline int run_diff(const std::string& root,
                    const std::string& db_path_opt,
                    const std::string& base_ref,
                    const std::string& target_ref,
                    bool json_output,
                    const std::string& file_pattern = "") {
    namespace fs = std::filesystem;
    std::string db_path = db_path_opt.empty() ? default_db(root) : db_path_opt;

    if (!fs::exists(db_path)) {
        std::cerr << "ERROR: Database does not exist at " << db_path << "\n"
                  << "Run 'codetopo index --root " << root << "' first to construct the code graph.\n";
        return 1;
    }

    try {
        Connection conn(db_path, true);
        GraphDiffReport report = compute_semantic_diff(conn, root, base_ref, target_ref, file_pattern);

        if (json_output) {
            std::cout << format_diff_json(report) << "\n";
        } else {
            bool use_color = isatty(fileno(stdout));
            std::cout << format_diff_table(report, use_color);
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: Failed to compute semantic diff: " << e.what() << "\n";
        return 1;
    }
}

} // namespace codetopo
