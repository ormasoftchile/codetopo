#pragma once

#include "db/connection.h"
#include <sqlite3.h>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <cstring>
#include <algorithm>

namespace codetopo {

struct LanguageQuality {
    std::string language;
    int64_t total_calls = 0;
    int64_t resolved_calls = 0;
    int64_t approx_calls = 0;
    int64_t unresolved_calls = 0;
    double resolved_pct = 0.0;
    double approx_pct = 0.0;
    double unresolved_pct = 0.0;
};

struct EdgeKindQuality {
    std::string kind;
    int64_t count = 0;
    double avg_confidence = 0.0;
    int64_t exact_count = 0;
    int64_t approx_count = 0;
};

struct ConfidenceBucket {
    std::string label;
    int64_t count = 0;
    double pct = 0.0;
};

struct ProvenanceQuality {
    int64_t static_count = 0;
    int64_t runtime_count = 0;
    int64_t protocol_count = 0;
    int64_t semantic_count = 0;
    int64_t inferred_count = 0;
    int64_t total_traces = 0;
    int64_t total_trace_calls = 0;
};

struct GraphQuality {
    int64_t total_files = 0;
    int64_t files_ok = 0;
    int64_t files_partial = 0;
    int64_t files_failed = 0;
    int64_t total_symbols = 0;
    int64_t ranked_symbols = 0;
    int64_t total_edges = 0;
    int64_t resolved_edges = 0;
    int64_t approx_edges = 0;
    int64_t total_call_refs = 0;
    int64_t resolved_call_refs = 0;
    int64_t unresolved_call_refs = 0;
    int64_t ambiguous_call_refs = 0;
    int64_t dangling_call_refs = 0;
    double call_resolution_rate = 0.0;

    std::vector<LanguageQuality> languages;
    std::vector<EdgeKindQuality> edge_kinds;
    std::vector<ConfidenceBucket> confidence_distribution;
    ProvenanceQuality provenance;
};

inline GraphQuality compute_graph_quality(Connection& conn) {
    GraphQuality q;
    sqlite3* db = conn.raw();

    // 1. Files
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "SELECT count(*), "
            "sum(CASE WHEN parse_status = 'ok' THEN 1 ELSE 0 END), "
            "sum(CASE WHEN parse_status = 'partial' THEN 1 ELSE 0 END), "
            "sum(CASE WHEN parse_status = 'failed' THEN 1 ELSE 0 END) "
            "FROM files", -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                q.total_files = sqlite3_column_int64(stmt, 0);
                q.files_ok = sqlite3_column_int64(stmt, 1);
                q.files_partial = sqlite3_column_int64(stmt, 2);
                q.files_failed = sqlite3_column_int64(stmt, 3);
            }
            sqlite3_finalize(stmt);
        }
    }

    // 2. Symbols
    {
        bool has_rank = false;
        sqlite3_stmt* check_rank = nullptr;
        if (sqlite3_prepare_v2(db, "PRAGMA table_info(nodes)", -1, &check_rank, nullptr) == SQLITE_OK) {
            while (sqlite3_step(check_rank) == SQLITE_ROW) {
                const char* col = reinterpret_cast<const char*>(sqlite3_column_text(check_rank, 1));
                if (col && std::strcmp(col, "rank") == 0) {
                    has_rank = true;
                    break;
                }
            }
            sqlite3_finalize(check_rank);
        }

        const char* sql = has_rank
            ? "SELECT count(*), coalesce(sum(CASE WHEN rank > 0.0 THEN 1 ELSE 0 END), 0) FROM nodes WHERE node_type = 'symbol'"
            : "SELECT count(*), 0 FROM nodes WHERE node_type = 'symbol'";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                q.total_symbols = sqlite3_column_int64(stmt, 0);
                q.ranked_symbols = sqlite3_column_int64(stmt, 1);
            }
            sqlite3_finalize(stmt);
        }
    }

    // 3. Edges overall
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "SELECT count(*), "
            "sum(CASE WHEN confidence >= 1.0 AND (evidence IS NULL OR evidence = 'ast') THEN 1 ELSE 0 END), "
            "sum(CASE WHEN confidence < 1.0 OR evidence = 'name-match' THEN 1 ELSE 0 END) "
            "FROM edges", -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                q.total_edges = sqlite3_column_int64(stmt, 0);
                q.resolved_edges = sqlite3_column_int64(stmt, 1);
                q.approx_edges = sqlite3_column_int64(stmt, 2);
            }
            sqlite3_finalize(stmt);
        }
    }

    // 4. Edges by kind
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "SELECT kind, count(*), coalesce(avg(confidence), 0.0), "
            "sum(CASE WHEN confidence >= 1.0 AND (evidence IS NULL OR evidence = 'ast') THEN 1 ELSE 0 END), "
            "sum(CASE WHEN confidence < 1.0 OR evidence = 'name-match' THEN 1 ELSE 0 END) "
            "FROM edges GROUP BY kind ORDER BY count(*) DESC", -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                EdgeKindQuality ek;
                const char* k = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                ek.kind = k ? k : "";
                ek.count = sqlite3_column_int64(stmt, 1);
                ek.avg_confidence = sqlite3_column_double(stmt, 2);
                ek.exact_count = sqlite3_column_int64(stmt, 3);
                ek.approx_count = sqlite3_column_int64(stmt, 4);
                q.edge_kinds.push_back(ek);
            }
            sqlite3_finalize(stmt);
        }
    }

    // 5. Confidence Distribution
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "SELECT "
            "sum(CASE WHEN confidence >= 1.0 THEN 1 ELSE 0 END), "
            "sum(CASE WHEN confidence >= 0.8 AND confidence < 1.0 THEN 1 ELSE 0 END), "
            "sum(CASE WHEN confidence >= 0.5 AND confidence < 0.8 THEN 1 ELSE 0 END), "
            "sum(CASE WHEN confidence < 0.5 THEN 1 ELSE 0 END) "
            "FROM edges", -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                int64_t c1 = sqlite3_column_int64(stmt, 0);
                int64_t c2 = sqlite3_column_int64(stmt, 1);
                int64_t c3 = sqlite3_column_int64(stmt, 2);
                int64_t c4 = sqlite3_column_int64(stmt, 3);
                double total = q.total_edges > 0 ? static_cast<double>(q.total_edges) : 1.0;
                q.confidence_distribution.push_back({"1.0 (exact)", c1, (c1 * 100.0) / total});
                q.confidence_distribution.push_back({"[0.8, 1.0) (likely)", c2, (c2 * 100.0) / total});
                q.confidence_distribution.push_back({"[0.5, 0.8) (approx)", c3, (c3 * 100.0) / total});
                q.confidence_distribution.push_back({"[0.3, 0.5) (low)", c4, (c4 * 100.0) / total});
            }
            sqlite3_finalize(stmt);
        }
    }

    // 6. Language Call Resolution Breakdown
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "SELECT f.language, count(*) AS total_calls, "
            "sum(CASE WHEN r.resolved_node_id IS NOT NULL AND (r.evidence IS NULL OR r.evidence != 'name-match') THEN 1 ELSE 0 END) AS resolved_calls, "
            "sum(CASE WHEN r.resolved_node_id IS NOT NULL AND r.evidence = 'name-match' THEN 1 ELSE 0 END) AS approx_calls, "
            "sum(CASE WHEN r.resolved_node_id IS NULL THEN 1 ELSE 0 END) AS unresolved_calls "
            "FROM refs r "
            "JOIN files f ON f.id = r.file_id "
            "WHERE r.kind = 'call' "
            "GROUP BY f.language "
            "ORDER BY total_calls DESC", -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                LanguageQuality lq;
                const char* lang = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                lq.language = lang ? lang : "unknown";
                lq.total_calls = sqlite3_column_int64(stmt, 1);
                lq.resolved_calls = sqlite3_column_int64(stmt, 2);
                lq.approx_calls = sqlite3_column_int64(stmt, 3);
                lq.unresolved_calls = sqlite3_column_int64(stmt, 4);
                if (lq.total_calls > 0) {
                    lq.resolved_pct = (static_cast<double>(lq.resolved_calls) * 100.0) / lq.total_calls;
                    lq.approx_pct = (static_cast<double>(lq.approx_calls) * 100.0) / lq.total_calls;
                    lq.unresolved_pct = (static_cast<double>(lq.unresolved_calls) * 100.0) / lq.total_calls;
                }
                q.total_call_refs += lq.total_calls;
                q.resolved_call_refs += (lq.resolved_calls + lq.approx_calls);
                q.unresolved_call_refs += lq.unresolved_calls;
                q.languages.push_back(lq);
            }
            sqlite3_finalize(stmt);
        }
        if (q.total_call_refs > 0) {
            q.call_resolution_rate = (static_cast<double>(q.resolved_call_refs) * 100.0) / q.total_call_refs;
        }
    }

    // 7. Dangling and Ambiguous Call References
    {
        // Dangling references: unresolved call references where the symbol name is not defined in any file
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "SELECT count(*) FROM refs r "
            "WHERE r.kind = 'call' AND r.resolved_node_id IS NULL "
            "AND NOT EXISTS (SELECT 1 FROM nodes n WHERE n.name = r.name AND n.node_type = 'symbol')",
            -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                q.dangling_call_refs = sqlite3_column_int64(stmt, 0);
            }
            sqlite3_finalize(stmt);
        }
    }
    {
        // Ambiguous references: unresolved call references where >= 2 definitions of that symbol name exist
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "SELECT count(*) FROM ("
            "  SELECT r.id FROM refs r "
            "  JOIN nodes n ON n.name = r.name AND n.node_type = 'symbol' AND n.is_definition = 1 "
            "  WHERE r.kind = 'call' AND r.resolved_node_id IS NULL "
            "  GROUP BY r.id HAVING count(n.id) > 1"
            ")", -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                q.ambiguous_call_refs = sqlite3_column_int64(stmt, 0);
            }
            sqlite3_finalize(stmt);
        }
    }

    // 8. Provenance
    {
        // Check if edges has the source column
        sqlite3_stmt* stmt = nullptr;
        bool has_source = false;
        if (sqlite3_prepare_v2(db, "PRAGMA table_info(edges)", -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* col = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                if (col && std::string(col) == "source") {
                    has_source = true;
                    break;
                }
            }
            sqlite3_finalize(stmt);
        }

        if (has_source) {
            sqlite3_stmt* p_stmt = nullptr;
            if (sqlite3_prepare_v2(db,
                "SELECT "
                "sum(CASE WHEN source = 'static' THEN 1 ELSE 0 END), "
                "sum(CASE WHEN source = 'runtime' THEN 1 ELSE 0 END), "
                "sum(CASE WHEN source = 'protocol' THEN 1 ELSE 0 END), "
                "sum(CASE WHEN source = 'semantic' THEN 1 ELSE 0 END), "
                "sum(CASE WHEN source = 'inferred' THEN 1 ELSE 0 END) "
                "FROM edges", -1, &p_stmt, nullptr) == SQLITE_OK) {
                if (sqlite3_step(p_stmt) == SQLITE_ROW) {
                    q.provenance.static_count = sqlite3_column_int64(p_stmt, 0);
                    q.provenance.runtime_count = sqlite3_column_int64(p_stmt, 1);
                    q.provenance.protocol_count = sqlite3_column_int64(p_stmt, 2);
                    q.provenance.semantic_count = sqlite3_column_int64(p_stmt, 3);
                    q.provenance.inferred_count = sqlite3_column_int64(p_stmt, 4);
                }
                sqlite3_finalize(p_stmt);
            }
        } else {
            // Default when source column is not yet present
            q.provenance.static_count = q.total_edges;
        }

        // Traces table check
        sqlite3_stmt* t_stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "SELECT count(*), coalesce(sum(call_count), 0) FROM traces",
            -1, &t_stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(t_stmt) == SQLITE_ROW) {
                q.provenance.total_traces = sqlite3_column_int64(t_stmt, 0);
                q.provenance.total_trace_calls = sqlite3_column_int64(t_stmt, 1);
            }
            sqlite3_finalize(t_stmt);
        }
    }

    return q;
}

inline std::string format_number(int64_t n) {
    if (n >= 1'000'000) {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1) << (static_cast<double>(n) / 1'000'000.0) << "M";
        return ss.str();
    }
    if (n >= 10'000) {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1) << (static_cast<double>(n) / 1'000.0) << "K";
        return ss.str();
    }
    return std::to_string(n);
}

inline std::string format_quality_table(const GraphQuality& q, bool color = true) {
    std::ostringstream ss;
    auto cyan = [color](const std::string& s) { return color ? ("\033[36m" + s + "\033[0m") : s; };
    auto green = [color](const std::string& s) { return color ? ("\033[32m" + s + "\033[0m") : s; };
    auto yellow = [color](const std::string& s) { return color ? ("\033[33m" + s + "\033[0m") : s; };
    auto bold = [color](const std::string& s) { return color ? ("\033[1m" + s + "\033[0m") : s; };

    ss << bold("================================================================================") << "\n";
    ss << bold("                       CodeTopo Graph Quality Report") << "\n";
    ss << bold("================================================================================") << "\n\n";

    ss << bold("Repository Overview:") << "\n";
    ss << "  Files:               " << cyan(std::to_string(q.total_files)) << " ("
       << q.files_ok << " ok, " << q.files_partial << " partial, " << q.files_failed << " failed)\n";
    ss << "  Symbols:             " << cyan(std::to_string(q.total_symbols)) << " (ranked with PageRank: "
       << q.ranked_symbols << ")\n";
    ss << "  Edges:               " << cyan(std::to_string(q.total_edges)) << " (exact: "
       << q.resolved_edges << ", approx: " << q.approx_edges << ")\n";
    std::ostringstream res_rate_ss;
    res_rate_ss << std::fixed << std::setprecision(1) << q.call_resolution_rate << "%";
    ss << "  Overall Resolution:  " << green(res_rate_ss.str()) << "\n\n";

    ss << bold("Call Resolution by Language:") << "\n";
    ss << "  " << std::left << std::setw(16) << "Language"
       << std::right << std::setw(10) << "Calls"
       << std::setw(18) << "Resolved"
       << std::setw(16) << "Approx"
       << std::setw(16) << "Unresolved" << "\n";
    ss << "  ----------------------------------------------------------------------------\n";
    for (const auto& lq : q.languages) {
        std::ostringstream res_ss, app_ss, unres_ss;
        res_ss << format_number(lq.resolved_calls) << " (" << std::fixed << std::setprecision(1) << lq.resolved_pct << "%)";
        app_ss << format_number(lq.approx_calls) << " (" << std::fixed << std::setprecision(1) << lq.approx_pct << "%)";
        unres_ss << format_number(lq.unresolved_calls) << " (" << std::fixed << std::setprecision(1) << lq.unresolved_pct << "%)";

        ss << "  " << std::left << std::setw(16) << lq.language
           << std::right << std::setw(10) << format_number(lq.total_calls)
           << std::setw(18) << res_ss.str()
           << std::setw(16) << app_ss.str()
           << std::setw(16) << unres_ss.str() << "\n";
    }
    ss << "\n";

    ss << bold("Edges by Kind:") << "\n";
    ss << "  " << std::left << std::setw(16) << "Kind"
       << std::right << std::setw(12) << "Count"
       << std::setw(14) << "Avg Conf"
       << std::setw(14) << "Exact"
       << std::setw(14) << "Approx" << "\n";
    ss << "  ----------------------------------------------------------------------\n";
    for (const auto& ek : q.edge_kinds) {
        ss << "  " << std::left << std::setw(16) << ek.kind
           << std::right << std::setw(12) << format_number(ek.count)
           << std::setw(14) << (std::to_string(std::round(ek.avg_confidence * 100.0) / 100.0).substr(0, 4))
           << std::setw(14) << format_number(ek.exact_count)
           << std::setw(14) << format_number(ek.approx_count) << "\n";
    }
    ss << "\n";

    ss << bold("Confidence Distribution:") << "\n";
    ss << "  " << std::left << std::setw(24) << "Bucket"
       << std::right << std::setw(12) << "Count"
       << std::setw(12) << "Pct" << "\n";
    ss << "  ------------------------------------------------\n";
    for (const auto& cb : q.confidence_distribution) {
        ss << "  " << std::left << std::setw(24) << cb.label
           << std::right << std::setw(12) << format_number(cb.count)
           << std::setw(11) << std::fixed << std::setprecision(1) << cb.pct << "%\n";
    }
    ss << "\n";

    ss << bold("Epistemic Provenance:") << "\n";
    ss << "  Static AST:          " << q.provenance.static_count << "\n";
    ss << "  Runtime Observed:    " << q.provenance.runtime_count << " (traces: " << q.provenance.total_traces
       << ", calls: " << q.provenance.total_trace_calls << ")\n";
    ss << "  Protocol (HTTP):     " << q.provenance.protocol_count << "\n";
    ss << "  Inferred (Heuristic):" << q.provenance.inferred_count << "\n\n";

    ss << bold("Reference Disambiguation:") << "\n";
    ss << "  Resolved Call Refs:      " << green(std::to_string(q.resolved_call_refs)) << "\n";
    ss << "  Ambiguous (>=2 matches): " << yellow(std::to_string(q.ambiguous_call_refs)) << "\n";
    ss << "  Dangling (external lib): " << std::to_string(q.dangling_call_refs) << "\n";
    ss << bold("================================================================================") << "\n";

    return ss.str();
}

inline std::string format_quality_json(const GraphQuality& q) {
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"total_files\": " << q.total_files << ",\n";
    ss << "  \"files_ok\": " << q.files_ok << ",\n";
    ss << "  \"files_partial\": " << q.files_partial << ",\n";
    ss << "  \"files_failed\": " << q.files_failed << ",\n";
    ss << "  \"total_symbols\": " << q.total_symbols << ",\n";
    ss << "  \"ranked_symbols\": " << q.ranked_symbols << ",\n";
    ss << "  \"total_edges\": " << q.total_edges << ",\n";
    ss << "  \"resolved_edges\": " << q.resolved_edges << ",\n";
    ss << "  \"approx_edges\": " << q.approx_edges << ",\n";
    ss << "  \"total_call_refs\": " << q.total_call_refs << ",\n";
    ss << "  \"resolved_call_refs\": " << q.resolved_call_refs << ",\n";
    ss << "  \"unresolved_call_refs\": " << q.unresolved_call_refs << ",\n";
    ss << "  \"ambiguous_call_refs\": " << q.ambiguous_call_refs << ",\n";
    ss << "  \"dangling_call_refs\": " << q.dangling_call_refs << ",\n";
    ss << "  \"call_resolution_rate\": " << std::fixed << std::setprecision(4) << q.call_resolution_rate << ",\n";
    
    // Languages array
    ss << "  \"languages\": [\n";
    for (size_t i = 0; i < q.languages.size(); ++i) {
        const auto& lq = q.languages[i];
        ss << "    {\n"
           << "      \"language\": \"" << lq.language << "\",\n"
           << "      \"total_calls\": " << lq.total_calls << ",\n"
           << "      \"resolved_calls\": " << lq.resolved_calls << ",\n"
           << "      \"approx_calls\": " << lq.approx_calls << ",\n"
           << "      \"unresolved_calls\": " << lq.unresolved_calls << ",\n"
           << "      \"resolved_pct\": " << std::fixed << std::setprecision(2) << lq.resolved_pct << ",\n"
           << "      \"approx_pct\": " << std::fixed << std::setprecision(2) << lq.approx_pct << ",\n"
           << "      \"unresolved_pct\": " << std::fixed << std::setprecision(2) << lq.unresolved_pct << "\n"
           << "    }" << (i + 1 < q.languages.size() ? "," : "") << "\n";
    }
    ss << "  ],\n";

    // Edge kinds array
    ss << "  \"edge_kinds\": [\n";
    for (size_t i = 0; i < q.edge_kinds.size(); ++i) {
        const auto& ek = q.edge_kinds[i];
        ss << "    {\n"
           << "      \"kind\": \"" << ek.kind << "\",\n"
           << "      \"count\": " << ek.count << ",\n"
           << "      \"avg_confidence\": " << std::fixed << std::setprecision(4) << ek.avg_confidence << ",\n"
           << "      \"exact_count\": " << ek.exact_count << ",\n"
           << "      \"approx_count\": " << ek.approx_count << "\n"
           << "    }" << (i + 1 < q.edge_kinds.size() ? "," : "") << "\n";
    }
    ss << "  ],\n";

    // Confidence distribution
    ss << "  \"confidence_distribution\": [\n";
    for (size_t i = 0; i < q.confidence_distribution.size(); ++i) {
        const auto& cb = q.confidence_distribution[i];
        ss << "    {\n"
           << "      \"label\": \"" << cb.label << "\",\n"
           << "      \"count\": " << cb.count << ",\n"
           << "      \"pct\": " << std::fixed << std::setprecision(2) << cb.pct << "\n"
           << "    }" << (i + 1 < q.confidence_distribution.size() ? "," : "") << "\n";
    }
    ss << "  ],\n";

    // Provenance object
    ss << "  \"provenance\": {\n";
    ss << "    \"static\": " << q.provenance.static_count << ",\n";
    ss << "    \"runtime\": " << q.provenance.runtime_count << ",\n";
    ss << "    \"protocol\": " << q.provenance.protocol_count << ",\n";
    ss << "    \"semantic\": " << q.provenance.semantic_count << ",\n";
    ss << "    \"inferred\": " << q.provenance.inferred_count << ",\n";
    ss << "    \"total_traces\": " << q.provenance.total_traces << ",\n";
    ss << "    \"total_trace_calls\": " << q.provenance.total_trace_calls << "\n";
    ss << "  }\n";
    ss << "}\n";

    return ss.str();
}

} // namespace codetopo
