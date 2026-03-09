#pragma once

#include <string>
#include <filesystem>
#include <fstream>

namespace codetopo {

// Resolve default db path: <root>/.codetopo/index.sqlite
inline std::string default_db(const std::string& root) {
    return (std::filesystem::path(root) / ".codetopo" / "index.sqlite").string();
}

// Create .codetopo/ dir and auto-add to .gitignore.
// Returns true if .gitignore was modified.
inline bool ensure_codetopo_dir(const std::string& root) {
    namespace fs = std::filesystem;
    auto dir = fs::path(root) / ".codetopo";
    if (!fs::exists(dir)) fs::create_directories(dir);

    auto gitignore = fs::path(root) / ".gitignore";
    bool needs = true;
    if (fs::exists(gitignore)) {
        std::ifstream fin(gitignore);
        std::string line;
        while (std::getline(fin, line)) {
            while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
            if (line == ".codetopo/" || line == ".codetopo") { needs = false; break; }
        }
    }
    if (needs) {
        std::ofstream fout(gitignore, std::ios::app);
        if (fout) fout << "\n# codetopo index (auto-generated)\n.codetopo/\n";
    }
    return needs;
}

} // namespace codetopo
