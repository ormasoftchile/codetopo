#pragma once
// T060-T069: MCP tool declarations
// Each tool is a function returning a JSON string.

#include "util/json.h"
#include "db/connection.h"
#include "db/queries.h"
#include <string>

namespace codetopo {
namespace tools {

// T060: server_info
std::string server_info(yyjson_val* params, Connection& conn,
                        QueryCache& cache, const std::string& repo_root);

// T061: repo_stats
std::string repo_stats(yyjson_val* params, Connection& conn,
                       QueryCache& cache, const std::string& repo_root);

// T080: file_search — search file paths by GLOB pattern
std::string file_search(yyjson_val* params, Connection& conn,
                        QueryCache& cache, const std::string& repo_root);

// T081: dir_list — list files in a directory (one level)
std::string dir_list(yyjson_val* params, Connection& conn,
                     QueryCache& cache, const std::string& repo_root);

// T081b: dir_tree — return a directory subtree up to a given depth
std::string dir_tree(yyjson_val* params, Connection& conn,
                     QueryCache& cache, const std::string& repo_root);

// T062: symbol_search
std::string symbol_search(yyjson_val* params, Connection& conn,
                          QueryCache& cache, const std::string& repo_root);

// T062b: symbol_list — list/filter symbols without FTS
std::string symbol_list(yyjson_val* params, Connection& conn,
                        QueryCache& cache, const std::string& repo_root);

// T062c: symbols_in_path — list symbols under a directory subtree
std::string symbols_in_path(yyjson_val* params, Connection& conn,
                            QueryCache& cache, const std::string& repo_root);

// Helper: read source snippet from disk
std::string read_source_snippet(const std::string& repo_root,
                                const std::string& rel_path,
                                int start_line, int end_line);

// T063: symbol_get
std::string symbol_get(yyjson_val* params, Connection& conn,
                       QueryCache& cache, const std::string& repo_root);

// T064: symbol_get_batch — multi-ID lookup
std::string symbol_get_batch(yyjson_val* params, Connection& conn,
                             QueryCache& cache, const std::string& repo_root);

// T066: callers_approx
std::string callers_approx(yyjson_val* params, Connection& conn,
                           QueryCache& cache, const std::string& repo_root);

// T067: callees_approx
std::string callees_approx(yyjson_val* params, Connection& conn,
                           QueryCache& cache, const std::string& repo_root);

// T065: references
std::string references(yyjson_val* params, Connection& conn,
                       QueryCache& cache, const std::string& repo_root);

// T069: file_summary
std::string file_summary(yyjson_val* params, Connection& conn,
                         QueryCache& cache, const std::string& repo_root);

// T069b: file_overview
std::string file_overview(yyjson_val* params, Connection& conn,
                          QueryCache& cache, const std::string& repo_root);

// T068: context_for (one-shot symbol understanding)
std::string context_for(yyjson_val* params, Connection& conn,
                        QueryCache& cache, const std::string& repo_root);

// T068b: context_by_name — resolve by symbol name, then return context
std::string context_by_name(yyjson_val* params, Connection& conn,
                            QueryCache& cache, const std::string& repo_root);

// T084: entrypoints — find natural starting points
std::string entrypoints(yyjson_val* params, Connection& conn,
                        QueryCache& cache, const std::string& repo_root);

// T085: impact_of — transitive dependents via BFS
std::string impact_of(yyjson_val* params, Connection& conn,
                      QueryCache& cache, const std::string& repo_root);

// T094: detect_changes — git diff blast-radius analysis
std::string detect_changes(yyjson_val* params, Connection& conn,
                           QueryCache& cache, const std::string& repo_root);

// T086: file_deps — include relationships for a file
std::string file_deps(yyjson_val* params, Connection& conn,
                      QueryCache& cache, const std::string& repo_root);

// T087: subgraph — multi-seed BFS extraction
std::string subgraph(yyjson_val* params, Connection& conn,
                     QueryCache& cache, const std::string& repo_root);

// T088: shortest_path — BFS between two nodes
std::string shortest_path(yyjson_val* params, Connection& conn,
                          QueryCache& cache, const std::string& repo_root);

// T089: find_implementations — find types that inherit from a base
std::string find_implementations(yyjson_val* params, Connection& conn,
                                 QueryCache& cache, const std::string& repo_root);

// T090: method_fields — field accesses and calls made by a method
std::string method_fields(yyjson_val* params, Connection& conn,
                          QueryCache& cache, const std::string& repo_root);

// T091: dependency_cluster — group methods by shared field access, weighted by read/write
std::string dependency_cluster(yyjson_val* params, Connection& conn,
                               QueryCache& cache, const std::string& repo_root);

// T092: source_at — read raw source lines from a file
std::string source_at(yyjson_val* params, Connection& conn,
                      QueryCache& cache, const std::string& repo_root);

// T093: code_search — search source file contents using trigram FTS index
std::string code_search(yyjson_val* params, Connection& conn,
                        QueryCache& cache, const std::string& repo_root);

// Workspace tools — multi-root management
std::string workspace_add(yyjson_val* params, Connection& conn,
                          QueryCache& cache, const std::string& repo_root);
std::string workspace_remove(yyjson_val* params, Connection& conn,
                             QueryCache& cache, const std::string& repo_root);
std::string workspace_list(yyjson_val* params, Connection& conn,
                           QueryCache& cache, const std::string& repo_root);

} // namespace tools
} // namespace codetopo
