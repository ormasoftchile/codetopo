#pragma once

namespace codetopo::db {

inline constexpr const char* owned_file_nodes_sql = R"SQL(
SELECT n.id, f.path FROM files f LEFT JOIN roots r ON r.id = f.root_id
CROSS JOIN nodes n INDEXED BY idx_nodes_stable_key
WHERE n.node_type = 'file' AND n.stable_key IN (
  CASE WHEN f.root_id IS NULL THEN f.path || '::file'
       ELSE CAST(f.root_id AS TEXT) || ':' ||
            substr(f.path, length(r.path) + 2) || '::file' END,
  CASE WHEN f.root_id IS NULL THEN 'file:' || f.path
       ELSE CAST(f.root_id AS TEXT) || ':file:' ||
            substr(f.path, length(r.path) + 2) END)
)SQL";

} // namespace codetopo::db
