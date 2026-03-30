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

// T062: symbol_search
std::string symbol_search(yyjson_val* params, Connection& conn,
                          QueryCache& cache, const std::string& repo_root);

// T062b: symbol_list — list/filter symbols without FTS
std::string symbol_list(yyjson_val* params, Connection& conn,
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

// T068: context_for (one-shot symbol understanding)
std::string context_for(yyjson_val* params, Connection& conn,
                        QueryCache& cache, const std::string& repo_root);

// T084: entrypoints — find natural starting points
std::string entrypoints(yyjson_val* params, Connection& conn,
                        QueryCache& cache, const std::string& repo_root);

// T085: impact_of — transitive dependents via BFS
std::string impact_of(yyjson_val* params, Connection& conn,
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

} // namespace tools
} // namespace codetopo
