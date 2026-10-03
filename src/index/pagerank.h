#pragma once

#include "db/connection.h"
#include "db/schema.h"
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace codetopo {

struct PageRankOptions {
    double damping = 0.85;
    int max_iterations = 20;
    double tolerance = 1e-5;
};

struct EdgeWeight {
    uint32_t src_idx;
    double weight;
};

// Computes PageRank centrality over directed graph edges ('calls', 'inherits', 'references')
// and updates nodes.rank with normalized scores in [0.0, 1.0].
// Returns the number of ranked nodes.
inline int compute_and_persist_pagerank(Connection& conn, const PageRankOptions& options = {}) {
    schema::ensure_nodes_rank_schema(conn);

    // Read edges from the DB.
    // Directed: caller -> callee, subclass -> base, referrer -> target.
    // dst_id receives importance from src_id.
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT src_id, dst_id, confidence FROM edges "
        "WHERE kind IN ('calls', 'inherits', 'references') AND src_id != dst_id";
    if (sqlite3_prepare_v2(conn.raw(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }

    std::unordered_map<int64_t, uint32_t> id_to_idx;
    std::vector<int64_t> idx_to_id;

    auto get_or_add_idx = [&](int64_t id) -> uint32_t {
        auto it = id_to_idx.find(id);
        if (it != id_to_idx.end()) return it->second;
        uint32_t idx = static_cast<uint32_t>(idx_to_id.size());
        id_to_idx[id] = idx;
        idx_to_id.push_back(id);
        return idx;
    };

    struct RawEdge {
        uint32_t src;
        uint32_t dst;
        double weight;
    };
    std::vector<RawEdge> raw_edges;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t src = sqlite3_column_int64(stmt, 0);
        int64_t dst = sqlite3_column_int64(stmt, 1);
        double conf = sqlite3_column_double(stmt, 2);
        if (conf < 0.1) conf = 0.1;
        uint32_t u = get_or_add_idx(src);
        uint32_t v = get_or_add_idx(dst);
        raw_edges.push_back({u, v, conf});
    }
    sqlite3_finalize(stmt);

    const size_t num_nodes = idx_to_id.size();
    if (num_nodes == 0) return 0;

    std::vector<double> out_weights(num_nodes, 0.0);
    std::vector<std::vector<EdgeWeight>> incoming(num_nodes);

    for (const auto& edge : raw_edges) {
        out_weights[edge.src] += edge.weight;
        incoming[edge.dst].push_back({edge.src, edge.weight});
    }

    // Power iteration
    std::vector<double> pr(num_nodes, 1.0 / static_cast<double>(num_nodes));
    std::vector<double> next_pr(num_nodes, 0.0);

    const double d = options.damping;
    const double inv_n = 1.0 / static_cast<double>(num_nodes);

    for (int iter = 0; iter < options.max_iterations; ++iter) {
        double dangling_sum = 0.0;
        for (size_t i = 0; i < num_nodes; ++i) {
            if (out_weights[i] == 0.0) {
                dangling_sum += pr[i];
            }
        }

        const double base = (1.0 - d) * inv_n + (d * dangling_sum) * inv_n;

        double diff = 0.0;
        for (size_t v = 0; v < num_nodes; ++v) {
            double sum_in = 0.0;
            for (const auto& in_edge : incoming[v]) {
                sum_in += pr[in_edge.src_idx] * (in_edge.weight / out_weights[in_edge.src_idx]);
            }
            double val = base + d * sum_in;
            next_pr[v] = val;
            diff += std::abs(val - pr[v]);
        }

        pr = next_pr;
        if (diff < options.tolerance) {
            break;
        }
    }

    // Normalization: find max rank and scale to (0.0, 1.0]
    double max_pr = 0.0;
    for (double score : pr) {
        if (score > max_pr) max_pr = score;
    }

    if (max_pr <= 0.0) return 0;

    // Bulk persist via temporary table
    conn.exec("CREATE TEMP TABLE IF NOT EXISTS temp_pagerank(id INTEGER PRIMARY KEY, rank REAL)");
    conn.exec("DELETE FROM temp_pagerank");
    conn.exec("BEGIN TRANSACTION");

    sqlite3_stmt* ins_stmt = nullptr;
    sqlite3_prepare_v2(conn.raw(),
        "INSERT INTO temp_pagerank(id, rank) VALUES(?, ?)", -1, &ins_stmt, nullptr);

    for (size_t i = 0; i < num_nodes; ++i) {
        double normalized = pr[i] / max_pr;
        // Clamp to 6 decimal places to avoid floating noise
        normalized = std::round(normalized * 1000000.0) / 1000000.0;
        sqlite3_reset(ins_stmt);
        sqlite3_bind_int64(ins_stmt, 1, idx_to_id[i]);
        sqlite3_bind_double(ins_stmt, 2, normalized);
        sqlite3_step(ins_stmt);
    }
    sqlite3_finalize(ins_stmt);
    conn.exec("COMMIT");

    conn.exec(
        "UPDATE nodes SET rank = (SELECT rank FROM temp_pagerank WHERE temp_pagerank.id = nodes.id) "
        "WHERE id IN (SELECT id FROM temp_pagerank)");
    conn.exec("DROP TABLE IF EXISTS temp_pagerank");

    return static_cast<int>(num_nodes);
}

} // namespace codetopo
