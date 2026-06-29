#pragma once

#include "db/connection.h"
#include "core/config.h"
#include <string>
#include <vector>
#include <cstdint>
#include <filesystem>
#include <iostream>

namespace codetopo {

// Multi-root workspace: merges extra roots directly into the main index.sqlite.
// roots table + root_id column on files allow multiple projects to coexist in
// one DB without any mode-switching.  The MCP server always opens index.sqlite.

class WorkspaceDB {
public:
    // Takes the path to the main index.sqlite (not a separate workspace.sqlite).
    explicit WorkspaceDB(const std::string& main_db_path);

    struct AddResult { int64_t root_id = 0; int64_t files = 0; int64_t symbols = 0; int64_t edges = 0; };
    struct RemoveResult { int64_t files = 0; int64_t symbols = 0; int64_t edges = 0; };
    struct RootInfo { int64_t id = 0; std::string path; int64_t files = 0; int64_t symbols = 0; int64_t edges = 0; };

    // Add a root: index it (using existing supervisor), then merge into main DB tables.
    AddResult add_root(const std::string& root_path, const Config& cfg);

    // Remove a root: cascade-delete all its records.
    RemoveResult remove_root(const std::string& root_path);

    // List all roots with stats.
    std::vector<RootInfo> list_roots();

    // Check for overlap (subdir containment) — warn but don't block.
    void check_overlap(const std::string& new_root_path);

private:
    Connection conn_;
    void ensure_schema();
    void merge_root_attached(int64_t root_id, const std::string& root_path);
    void populate_content_fts_for_root(int64_t root_id);
    void resume_pending_content_fts();
};

// Legacy path — kept only for detecting old installations.
inline std::string workspace_db_path(const std::string& root) {
    return (std::filesystem::path(root) / ".codetopo" / "workspace.sqlite").string();
}

// Check if a legacy workspace.sqlite exists (old installation).
inline bool has_legacy_workspace(const std::string& root) {
    return std::filesystem::exists(workspace_db_path(root));
}

} // namespace codetopo
