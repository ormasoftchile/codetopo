#pragma once

#include "db/connection.h"
#include "core/config.h"
#include <string>
#include <vector>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <utility>
#include <functional>
#include <array>

namespace codetopo {

inline std::string workspace_root_metadata_key(int64_t root_id, const std::string& key) {
    return "workspace_root:" + std::to_string(root_id) + ":" + key;
}

inline constexpr std::array<const char*, 5> workspace_revision_keys = {
    "git_head", "git_branch", "last_index_time", "index_generation", "index_state"
};

// Ownership is independent of numeric IDs, including legacy offset-allocated rows.
inline std::string workspace_node_ids_sql(const std::string& root_id) {
    return "SELECT n.id FROM files f CROSS JOIN nodes n INDEXED BY idx_nodes_file_id "
           "ON n.file_id=f.id WHERE f.root_id=" + root_id +
           " UNION SELECT n.id FROM nodes n INDEXED BY idx_nodes_stable_key "
           "WHERE n.stable_key >= CAST(" + root_id + " AS TEXT)||':' "
           "AND n.stable_key < CAST(" + root_id + " AS TEXT)||';' AND n.node_type='file'";
}

inline std::string workspace_edge_count_sql(const std::string& root_id) {
    return "(SELECT COUNT(*) FROM edges WHERE src_id IN (" +
        workspace_node_ids_sql(root_id) + "))";
}

struct WorkspaceRootInfo {
    int64_t id = 0;
    std::string path;
    int64_t files = 0;
    int64_t symbols = 0;
    int64_t edges = 0;
    bool primary = false;
    bool graph_counts_checked = true;
};

inline std::string workspace_roots_sql() {
    return "SELECT r.id, r.path, "
        "(SELECT COUNT(*) FROM files INDEXED BY idx_files_root WHERE root_id=r.id), "
        "(SELECT COUNT(*) FROM files f INDEXED BY idx_files_root "
        "CROSS JOIN nodes n INDEXED BY idx_nodes_file_id ON n.file_id=f.id "
        "WHERE f.root_id=r.id), " +
        workspace_edge_count_sql("r.id") + " FROM roots r ORDER BY r.id";
}

// No schema migration, writer admission, or content backfill occurs on this read path.
std::vector<WorkspaceRootInfo> read_workspace_roots(Connection& conn);
std::vector<WorkspaceRootInfo> read_workspace_inventory(
    Connection& conn, const std::string& primary_root);

// Multi-root workspace: merges extra roots directly into the main index.sqlite.
// roots table + root_id column on files allow multiple projects to coexist in
// one DB without any mode-switching.  The MCP server always opens index.sqlite.

class WorkspaceDB {
public:
    // Takes the path to the main index.sqlite (not a separate workspace.sqlite).
    explicit WorkspaceDB(const std::string& main_db_path);

    struct AddResult {
        int64_t root_id = 0;
        int64_t files = 0;
        int64_t symbols = 0;
        int64_t edges = 0;
        int64_t http_call_refs = 0;
        int64_t roots_total = 0;
        int64_t files_total = 0;
        int64_t symbols_total = -1; // Not collected: global graph scans are not merge health checks.
        int64_t edges_total = -1;
    };
    struct RemoveResult { int64_t files = 0; int64_t symbols = 0; int64_t edges = 0; };
    using RootInfo = WorkspaceRootInfo;

    // Add a root: index it (using existing supervisor), then merge into main DB tables.
    AddResult add_root(const std::string& root_path, const Config& cfg,
                       bool refresh = false,
                       const std::function<void(const std::string&)>& phase = {});

    // Remove a root: cascade-delete all its records.
    RemoveResult remove_root(const std::string& root_path);

    // List persisted additional roots with stats; user-facing inventory also includes primary.
    std::vector<RootInfo> list_roots();
    bool has_root(const std::string& path);

    // Check for overlap (subdir containment) — warn but don't block.
    void check_overlap(const std::string& new_root_path);

private:
    Connection conn_;
    void ensure_schema();
    void merge_root_attached(int64_t root_id, const std::string& root_path, bool color_output);
    std::pair<int64_t, int64_t> resolve_workspace_refs(int64_t added_root_id);
    void populate_content_fts_for_root(int64_t root_id, bool color_output);
    void resume_pending_content_fts();
    void clear_root_rows(int64_t root_id);
    void stage_refresh_relationships(int64_t root_id);
    void restore_refresh_relationships();
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
