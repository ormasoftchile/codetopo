#pragma once

#include "db/connection.h"
#include "db/schema.h"
#include "db/owned_nodes.h"
#include <cmath>
#include <cstdint>
#include <atomic>
#include <thread>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace codetopo {

struct PageRankOptions {
    double damping = 0.85;
    int max_iterations = 20;
    double tolerance = 1e-5;
    bool scope_to_owned_files = false;
};

struct EdgeWeight {
    uint32_t src_idx;
    double weight;
};

// Computes PageRank centrality over directed graph edges ('calls', 'inherits', 'references')
// and updates nodes.rank with normalized scores in [0.0, 1.0].
// Returns the number of ranked nodes.
inline int compute_and_persist_pagerank(Connection& conn,
                                        const PageRankOptions& options = {},
                                        const std::string& progress_path = "") {
    schema::ensure_nodes_rank_schema(conn);

    auto touch_progress = [&](const std::string& msg) {
        if (!progress_path.empty()) {
            std::ofstream pf(progress_path, std::ios::trunc);
            if (pf.is_open()) {
                pf << msg << '\n';
                pf.flush();
            }
        }
    };

    // Read edges from the DB.
    // Directed: caller -> callee, subclass -> base, referrer -> target.
    // dst_id receives importance from src_id.
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT src_id, dst_id, confidence FROM edges "
        "WHERE kind IN ('calls', 'inherits', 'references') AND src_id != dst_id";
    if (options.scope_to_owned_files) {
        conn.exec("DROP TABLE IF EXISTS temp.__ct_pagerank_scope");
        conn.exec("CREATE TEMP TABLE __ct_pagerank_scope(id INTEGER PRIMARY KEY)");
        conn.exec(
            "INSERT OR IGNORE INTO temp.__ct_pagerank_scope(id) "
            "SELECT n.id FROM files f CROSS JOIN nodes n INDEXED BY idx_nodes_file_id "
            "ON n.file_id=f.id");
        conn.exec(
            "INSERT OR IGNORE INTO temp.__ct_pagerank_scope(id) SELECT owned.id FROM (" +
            std::string(db::owned_file_nodes_sql) + ") owned");
        sql =
            "SELECT e.src_id,e.dst_id,e.confidence FROM temp.__ct_pagerank_scope s "
            "CROSS JOIN edges e INDEXED BY idx_edges_src "
            "WHERE e.src_id=s.id AND e.kind IN ('calls','inherits','references') "
            "AND e.src_id!=e.dst_id";
    }
    int prepare_rc = sqlite3_prepare_v2(conn.raw(), sql, -1, &stmt, nullptr);
    if (prepare_rc != SQLITE_OK) {
        auto error = std::string(sqlite3_errmsg(conn.raw()));
        if (options.scope_to_owned_files) conn.exec("DROP TABLE temp.__ct_pagerank_scope");
        throw SqliteError(prepare_rc, "PageRank edge query failed: " + error);
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

    int64_t loaded_edges = 0;
    int read_rc;
    while ((read_rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        int64_t src = sqlite3_column_int64(stmt, 0);
        int64_t dst = sqlite3_column_int64(stmt, 1);
        double conf = sqlite3_column_double(stmt, 2);
        if (conf < 0.1) conf = 0.1;
        uint32_t u = get_or_add_idx(src);
        uint32_t v = get_or_add_idx(dst);
        raw_edges.push_back({u, v, conf});
        if (++loaded_edges % 2000000 == 0) {
            touch_progress("pagerank_loading_edges: " + std::to_string(loaded_edges));
        }
    }
    sqlite3_finalize(stmt);
    if (options.scope_to_owned_files) conn.exec("DROP TABLE temp.__ct_pagerank_scope");
    if (read_rc != SQLITE_DONE) {
        throw SqliteError(read_rc,
            "PageRank edge read failed: " + std::string(sqlite3_errmsg(conn.raw())));
    }

    const size_t num_nodes = idx_to_id.size();
    if (num_nodes == 0) return 0;

    // Build flat CSR (Compressed Sparse Row) incoming graph representation.
    // Avoids vector-of-vectors heap fragmentation and enables vectorized FMA inner loop.
    std::vector<double> out_weights(num_nodes, 0.0);
    std::vector<uint32_t> incoming_count(num_nodes, 0);

    for (const auto& edge : raw_edges) {
        out_weights[edge.src] += edge.weight;
        incoming_count[edge.dst]++;
    }

    std::vector<uint32_t> head(num_nodes + 1, 0);
    for (size_t i = 0; i < num_nodes; ++i) {
        head[i + 1] = head[i] + incoming_count[i];
    }

    std::vector<uint32_t> cur_offset = head;
    std::vector<EdgeWeight> incoming_flat(raw_edges.size());
    for (const auto& edge : raw_edges) {
        incoming_flat[cur_offset[edge.dst]++] = {edge.src, edge.weight};
    }

    // Free raw_edges and lookup map memory now that flat CSR is built
    std::vector<RawEdge>().swap(raw_edges);
    std::unordered_map<int64_t, uint32_t>().swap(id_to_idx);

    std::vector<double> inv_out_weights(num_nodes, 0.0);
    for (size_t i = 0; i < num_nodes; ++i) {
        if (out_weights[i] > 0.0) {
            inv_out_weights[i] = 1.0 / out_weights[i];
        }
    }

    // Power iteration with pre-scaled rank values (FMA inner loop)
    std::vector<double> pr(num_nodes, 1.0 / static_cast<double>(num_nodes));
    std::vector<double> next_pr(num_nodes, 0.0);
    std::vector<double> pr_scaled(num_nodes);

    const double d = options.damping;
    const double inv_n = 1.0 / static_cast<double>(num_nodes);

    for (int iter = 0; iter < options.max_iterations; ++iter) {
        touch_progress("pagerank_iteration: " + std::to_string(iter));
        double dangling_sum = 0.0;
        for (size_t i = 0; i < num_nodes; ++i) {
            if (out_weights[i] == 0.0) {
                dangling_sum += pr[i];
            }
            pr_scaled[i] = pr[i] * inv_out_weights[i];
        }

        const double base = (1.0 - d) * inv_n + (d * dangling_sum) * inv_n;

        double diff = 0.0;
        for (size_t v = 0; v < num_nodes; ++v) {
            double sum_in = 0.0;
            const uint32_t start = head[v];
            const uint32_t end = head[v + 1];
            for (uint32_t idx = start; idx < end; ++idx) {
                sum_in += pr_scaled[incoming_flat[idx].src_idx] * incoming_flat[idx].weight;
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

    // Bulk persist via temporary table with direct UPDATE ... FROM join
    conn.exec("CREATE TEMP TABLE IF NOT EXISTS temp_pagerank(id INTEGER PRIMARY KEY, rank REAL)");
    conn.exec("DELETE FROM temp_pagerank");
    const bool manage_tx = (sqlite3_get_autocommit(conn.raw()) != 0);
    if (manage_tx) conn.exec("BEGIN TRANSACTION");

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
        if (i % 500000 == 0) {
            touch_progress("pagerank_persisting: " + std::to_string(i));
        }
    }
    sqlite3_finalize(ins_stmt);
    if (manage_tx) conn.exec("COMMIT");

    touch_progress("pagerank_updating_nodes");
    std::atomic<bool> update_done{false};
    std::thread pr_hb([&]() {
        while (!update_done.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            touch_progress("pagerank_updating_nodes");
        }
    });
    conn.exec(
        "UPDATE nodes SET rank = (SELECT rank FROM temp_pagerank WHERE id=nodes.id) "
        "WHERE nodes.id IN (SELECT id FROM temp_pagerank)");
    update_done.store(true, std::memory_order_relaxed);
    if (pr_hb.joinable()) pr_hb.join();
    conn.exec("DROP TABLE IF EXISTS temp_pagerank");

    return static_cast<int>(num_nodes);
}

} // namespace codetopo
