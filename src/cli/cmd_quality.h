#pragma once

#include "index/quality.h"
#include "util/repo.h"
#include "db/connection.h"
#include <unistd.h>
#include <iostream>
#include <filesystem>

namespace codetopo {

inline int run_quality(const std::string& root, const std::string& db_path_opt, bool json_output) {
    namespace fs = std::filesystem;
    std::string db_path = db_path_opt.empty() ? default_db(root) : db_path_opt;

    if (!fs::exists(db_path)) {
        std::cerr << "ERROR: Database does not exist at " << db_path << "\n"
                  << "Run 'codetopo index --root " << root << "' first to construct the code graph.\n";
        return 1;
    }

    try {
        Connection conn(db_path, true);
        GraphQuality q = compute_graph_quality(conn);

        if (json_output) {
            std::cout << format_quality_json(q) << "\n";
        } else {
            bool use_color = isatty(fileno(stdout));
            std::cout << format_quality_table(q, use_color);
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: Failed to compute graph quality: " << e.what() << "\n";
        return 1;
    }
}

} // namespace codetopo
