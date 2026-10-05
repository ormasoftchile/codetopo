#pragma once

#include "db/connection.h"
#include "core/arena.h"
#include "index/parser.h"
#include "index/extractor.h"
#include "util/path.h"
#include "util/git.h"
#include "util/process.h"
#include "util/json.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cmath>

namespace codetopo {

struct SymbolDiffItem {
    std::string status; // "added", "removed", "modified", "moved"
    std::string kind;
    std::string name;
    std::string qualname;
    std::string file;
    int old_line = 0;
    int new_line = 0;
    std::string old_signature;
    std::string new_signature;
};

struct EdgeDiffItem {
    std::string status; // "added", "removed", "modified"
    std::string kind;   // "calls", "includes", etc.
    std::string src_name;
    std::string src_file;
    std::string dst_name;
    std::string dst_file;
    double old_confidence = 0.0;
    double new_confidence = 0.0;
};

struct TopologyDeltaItem {
    std::string symbol;
    std::string metric; // "fan_in", "fan_out"
    int64_t old_value = 0;
    int64_t new_value = 0;
};

struct GraphDiffReport {
    std::string base_ref;
    std::string target_ref;
    int files_changed = 0;
    std::vector<SymbolDiffItem> symbols;
    std::vector<EdgeDiffItem> edges;
    std::vector<TopologyDeltaItem> topology;
    int symbols_added = 0;
    int symbols_removed = 0;
    int symbols_modified = 0;
    int edges_added = 0;
    int edges_removed = 0;
    int edges_modified = 0;
};

namespace diff_internal {

inline bool glob_match(const std::string& str, const std::string& pattern) {
    if (pattern.empty() || pattern == "*") return true;
    size_t s_idx = 0, p_idx = 0;
    size_t s_star = std::string::npos, p_star = std::string::npos;
    while (s_idx < str.size()) {
        if (p_idx < pattern.size() && (pattern[p_idx] == '?' || pattern[p_idx] == str[s_idx])) {
            s_idx++;
            p_idx++;
        } else if (p_idx < pattern.size() && pattern[p_idx] == '*') {
            p_star = p_idx++;
            s_star = s_idx;
        } else if (p_star != std::string::npos) {
            p_idx = p_star + 1;
            s_idx = ++s_star;
        } else {
            return false;
        }
    }
    while (p_idx < pattern.size() && pattern[p_idx] == '*') p_idx++;
    return p_idx == pattern.size();
}

struct ParsedState {
    std::vector<ExtractedSymbol> symbols;
    std::vector<std::pair<std::string, std::string>> calls; // (caller, callee)
};

inline ParsedState parse_source_code(const std::string& content,
                                    const std::string& lang,
                                    const std::string& rel_path) {
    ParsedState state;
    if (content.empty() || lang.empty()) return state;

    Parser parser;
    if (!parser.set_language(lang)) return state;

    TreeGuard tree(parser.parse(content));
    if (!tree || !tree.tree) return state;

    Extractor extractor(50000, 200, 5);
    auto extraction = extractor.extract(tree.tree, content, lang, rel_path);

    state.symbols = std::move(extraction.symbols);

    // Build calls from refs
    for (const auto& ref : extraction.refs) {
        if (ref.kind == "call" && !ref.name.empty()) {
            std::string caller = "global";
            if (ref.containing_symbol_index >= 0 &&
                static_cast<size_t>(ref.containing_symbol_index) < state.symbols.size()) {
                const auto& sym = state.symbols[ref.containing_symbol_index];
                caller = sym.qualname.empty() ? sym.name : sym.qualname;
            }
            state.calls.push_back({caller, ref.name});
        }
    }

    return state;
}

inline ParsedState load_db_state(Connection& conn, const std::string& rel_path) {
    ParsedState state;
    sqlite3* db = conn.raw();

    int64_t file_id = -1;
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT id FROM files WHERE path = ?1 OR path LIKE ?2", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, rel_path.c_str(), -1, SQLITE_TRANSIENT);
            std::string like_pattern = "%/" + rel_path;
            sqlite3_bind_text(stmt, 2, like_pattern.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                file_id = sqlite3_column_int64(stmt, 0);
            }
            sqlite3_finalize(stmt);
        }
    }
    if (file_id < 0) return state;

    // Load symbols
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "SELECT kind, name, COALESCE(qualname, name), COALESCE(signature, ''), "
            "start_line, end_line, COALESCE(fingerprint, ''), stable_key "
            "FROM nodes WHERE file_id = ?1 AND node_type = 'symbol' ORDER BY start_line",
            -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, file_id);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                ExtractedSymbol sym;
                sym.kind = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                sym.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                sym.qualname = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                sym.signature = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
                sym.start_line = sqlite3_column_int(stmt, 4);
                sym.end_line = sqlite3_column_int(stmt, 5);
                sym.fingerprint = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
                sym.stable_key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
                state.symbols.push_back(std::move(sym));
            }
            sqlite3_finalize(stmt);
        }
    }

    // Load calls
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "SELECT COALESCE(sn.qualname, sn.name), COALESCE(dn.qualname, dn.name) "
            "FROM edges e "
            "JOIN nodes sn ON sn.id = e.src_id "
            "JOIN nodes dn ON dn.id = e.dst_id "
            "WHERE sn.file_id = ?1 AND e.kind = 'calls'",
            -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, file_id);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* src = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                const char* dst = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                if (src && dst) {
                    state.calls.push_back({src, dst});
                }
            }
            sqlite3_finalize(stmt);
        }
    }

    return state;
}

} // namespace diff_internal

inline GraphDiffReport compute_semantic_diff(Connection& conn,
                                             const std::string& repo_root,
                                             const std::string& base_ref = "HEAD",
                                             const std::string& target_ref = "working-tree",
                                             const std::string& file_pattern = "") {
    namespace fs = std::filesystem;
    GraphDiffReport report;
    report.base_ref = base_ref.empty() ? "HEAD" : base_ref;
    report.target_ref = target_ref.empty() ? "working-tree" : target_ref;

    // 1. Collect changed files
    std::unordered_map<std::string, char> changed_files; // rel_path -> status ('M', 'A', 'D')

    if (report.target_ref == "working-tree") {
        std::string git_diff_out = git_command(repo_root, "diff --name-status " + report.base_ref);
        std::istringstream ss(git_diff_out);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.size() >= 3) {
                char status = line[0];
                size_t tab = line.find('\t');
                if (tab != std::string::npos && tab + 1 < line.size()) {
                    std::string path = line.substr(tab + 1);
                    changed_files[path] = status;
                }
            }
        }
        // Also capture untracked files
        std::string git_status_out = git_command(repo_root, "status --porcelain -u");
        std::istringstream ssu(git_status_out);
        while (std::getline(ssu, line)) {
            if (line.size() >= 4 && line[0] == '?' && line[1] == '?') {
                std::string path = line.substr(3);
                changed_files[path] = 'A';
            }
        }
    } else {
        std::string git_diff_out = git_command(repo_root, "diff --name-status " + report.base_ref + ".." + report.target_ref);
        std::istringstream ss(git_diff_out);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.size() >= 3) {
                char status = line[0];
                size_t tab = line.find('\t');
                if (tab != std::string::npos && tab + 1 < line.size()) {
                    std::string path = line.substr(tab + 1);
                    changed_files[path] = status;
                }
            }
        }
    }

    // 2. Setup parser arena
    Arena* existing_arena = get_thread_arena();
    std::unique_ptr<Arena> temp_arena;
    if (!existing_arena) {
        register_arena_allocator();
        temp_arena = std::make_unique<Arena>(64 * 1024 * 1024);
        set_thread_arena(temp_arena.get());
    }

    std::unordered_map<std::string, int64_t> fan_in_deltas;

    // 3. Process each changed file
    for (const auto& [rel_path, status] : changed_files) {
        if (!file_pattern.empty() && !diff_internal::glob_match(rel_path, file_pattern)) {
            continue;
        }

        std::string lang = path_util::detect_language(fs::path(rel_path));
        if (lang.empty()) continue; // Not an indexed language

        report.files_changed++;

        // Load Old State
        diff_internal::ParsedState old_state;
        if (status != 'A') {
            if (report.base_ref == "HEAD" || report.base_ref == "index") {
                old_state = diff_internal::load_db_state(conn, rel_path);
            }
            if (old_state.symbols.empty() && report.base_ref != "index") {
                std::string old_content = git_command(repo_root, "show " + report.base_ref + ":" + rel_path);
                if (!old_content.empty()) {
                    old_state = diff_internal::parse_source_code(old_content, lang, rel_path);
                }
            }
        }

        // Load New State
        diff_internal::ParsedState new_state;
        if (status != 'D') {
            std::string new_content;
            if (report.target_ref == "working-tree") {
                fs::path full_p = fs::path(repo_root) / rel_path;
                if (fs::exists(full_p)) {
                    std::ifstream ifs(full_p, std::ios::binary);
                    if (ifs) {
                        std::ostringstream buf;
                        buf << ifs.rdbuf();
                        new_content = buf.str();
                    }
                }
            } else {
                new_content = git_command(repo_root, "show " + report.target_ref + ":" + rel_path);
            }

            if (!new_content.empty()) {
                new_state = diff_internal::parse_source_code(new_content, lang, rel_path);
            }
        }

        // Diff Symbols
        std::unordered_map<std::string, ExtractedSymbol> old_by_key;
        for (const auto& s : old_state.symbols) {
            old_by_key[s.stable_key] = s;
        }

        for (const auto& n : new_state.symbols) {
            auto it = old_by_key.find(n.stable_key);
            if (it != old_by_key.end()) {
                const auto& old_s = it->second;
                if (!old_s.signature.empty() && !n.signature.empty() && old_s.signature != n.signature) {
                    SymbolDiffItem item;
                    item.status = "modified";
                    item.kind = n.kind;
                    item.name = n.name;
                    item.qualname = n.qualname.empty() ? n.name : n.qualname;
                    item.file = rel_path;
                    item.old_line = old_s.start_line;
                    item.new_line = n.start_line;
                    item.old_signature = old_s.signature;
                    item.new_signature = n.signature;
                    report.symbols.push_back(item);
                    report.symbols_modified++;
                } else if (std::abs(old_s.start_line - n.start_line) >= 5) {
                    SymbolDiffItem item;
                    item.status = "moved";
                    item.kind = n.kind;
                    item.name = n.name;
                    item.qualname = n.qualname.empty() ? n.name : n.qualname;
                    item.file = rel_path;
                    item.old_line = old_s.start_line;
                    item.new_line = n.start_line;
                    report.symbols.push_back(item);
                }
                old_by_key.erase(it);
            } else {
                SymbolDiffItem item;
                item.status = "added";
                item.kind = n.kind;
                item.name = n.name;
                item.qualname = n.qualname.empty() ? n.name : n.qualname;
                item.file = rel_path;
                item.new_line = n.start_line;
                item.new_signature = n.signature;
                report.symbols.push_back(item);
                report.symbols_added++;
            }
        }

        for (const auto& [k, rem] : old_by_key) {
            SymbolDiffItem item;
            item.status = "removed";
            item.kind = rem.kind;
            item.name = rem.name;
            item.qualname = rem.qualname.empty() ? rem.name : rem.qualname;
            item.file = rel_path;
            item.old_line = rem.start_line;
            item.old_signature = rem.signature;
            report.symbols.push_back(item);
            report.symbols_removed++;
        }

        // Diff Calls
        std::unordered_set<std::string> old_calls_set;
        for (const auto& c : old_state.calls) {
            old_calls_set.insert(c.first + "->" + c.second);
        }

        std::unordered_set<std::string> new_calls_set;
        for (const auto& c : new_state.calls) {
            new_calls_set.insert(c.first + "->" + c.second);
        }

        for (const auto& c : new_state.calls) {
            std::string key = c.first + "->" + c.second;
            if (old_calls_set.find(key) == old_calls_set.end()) {
                EdgeDiffItem item;
                item.status = "added";
                item.kind = "calls";
                item.src_name = c.first;
                item.dst_name = c.second;
                item.src_file = rel_path;
                report.edges.push_back(item);
                report.edges_added++;
                fan_in_deltas[c.second]++;
            }
        }

        for (const auto& c : old_state.calls) {
            std::string key = c.first + "->" + c.second;
            if (new_calls_set.find(key) == new_calls_set.end()) {
                EdgeDiffItem item;
                item.status = "removed";
                item.kind = "calls";
                item.src_name = c.first;
                item.dst_name = c.second;
                item.src_file = rel_path;
                report.edges.push_back(item);
                report.edges_removed++;
                fan_in_deltas[c.second]--;
            }
        }
    }

    // 4. Compute Topology Deltas (fan-in changes)
    if (!fan_in_deltas.empty()) {
        sqlite3* db = conn.raw();
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "SELECT count(*) FROM edges e "
            "WHERE e.dst_id IN ("
            "  SELECT id FROM nodes WHERE (name = ?1 OR qualname = ?1) AND node_type = 'symbol'"
            ") AND e.kind = 'calls'", -1, &stmt, nullptr) == SQLITE_OK) {

            for (const auto& [callee, delta] : fan_in_deltas) {
                if (delta == 0) continue;
                sqlite3_reset(stmt);
                sqlite3_bind_text(stmt, 1, callee.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    int64_t current_fan_in = sqlite3_column_int64(stmt, 0);
                    int64_t old_val = current_fan_in;
                    int64_t new_val = current_fan_in + delta;
                    if (new_val < 0) new_val = 0;
                    if (old_val != new_val) {
                        TopologyDeltaItem t_item;
                        t_item.symbol = callee;
                        t_item.metric = "fan_in";
                        t_item.old_value = old_val;
                        t_item.new_value = new_val;
                        report.topology.push_back(t_item);
                    }
                }
            }
            sqlite3_finalize(stmt);
        }
    }

    if (temp_arena) {
        set_thread_arena(nullptr);
    }

    return report;
}

inline std::string format_diff_table(const GraphDiffReport& r, bool color = true) {
    std::ostringstream ss;
    auto green = [color](const std::string& s) { return color ? ("\033[32m" + s + "\033[0m") : s; };
    auto red = [color](const std::string& s) { return color ? ("\033[31m" + s + "\033[0m") : s; };
    auto yellow = [color](const std::string& s) { return color ? ("\033[33m" + s + "\033[0m") : s; };
    auto bold = [color](const std::string& s) { return color ? ("\033[1m" + s + "\033[0m") : s; };
    auto cyan = [color](const std::string& s) { return color ? ("\033[36m" + s + "\033[0m") : s; };

    ss << bold("================================================================================") << "\n";
    ss << bold("                       CodeTopo Semantic Graph Diff") << "\n";
    ss << bold("================================================================================") << "\n";
    ss << "Base:   " << cyan(r.base_ref) << "\n";
    ss << "Target: " << cyan(r.target_ref) << "\n";
    ss << "Files changed: " << r.files_changed << "\n\n";

    if (r.symbols.empty() && r.edges.empty() && r.topology.empty()) {
        ss << "No structural changes detected between " << r.base_ref << " and " << r.target_ref << ".\n";
        ss << bold("================================================================================") << "\n";
        return ss.str();
    }

    ss << bold("Summary:") << "\n";
    ss << "  Symbols: " << green("+" + std::to_string(r.symbols_added) + " added") << ", "
       << red("-" + std::to_string(r.symbols_removed) + " removed") << ", "
       << yellow("~" + std::to_string(r.symbols_modified) + " modified") << "\n";
    ss << "  Edges:   " << green("+" + std::to_string(r.edges_added) + " added") << ", "
       << red("-" + std::to_string(r.edges_removed) + " removed") << "\n\n";

    if (!r.symbols.empty()) {
        ss << bold("Symbol Changes:") << "\n";
        for (const auto& sym : r.symbols) {
            if (sym.status == "added") {
                ss << "  " << green("+ symbol " + sym.qualname) << " (" << sym.kind << ") [" << sym.file << ":" << sym.new_line << "]\n";
            } else if (sym.status == "removed") {
                ss << "  " << red("- symbol " + sym.qualname) << " (" << sym.kind << ") [" << sym.file << ":" << sym.old_line << "]\n";
            } else if (sym.status == "modified") {
                ss << "  " << yellow("~ symbol " + sym.qualname) << " (" << sym.kind << ") [" << sym.file << ":" << sym.new_line << "]\n";
                if (!sym.old_signature.empty() && !sym.new_signature.empty()) {
                    ss << "      signature: \"" << sym.old_signature << "\" -> \"" << sym.new_signature << "\"\n";
                }
            } else if (sym.status == "moved") {
                ss << "  " << cyan("~ symbol " + sym.qualname) << " moved: L" << sym.old_line << " -> L" << sym.new_line << "\n";
            }
        }
        ss << "\n";
    }

    if (!r.edges.empty()) {
        ss << bold("Structural Edge Changes:") << "\n";
        for (const auto& e : r.edges) {
            if (e.status == "added") {
                ss << "  " << green("+ " + e.kind + " " + e.src_name + " -> " + e.dst_name) << "\n";
            } else if (e.status == "removed") {
                ss << "  " << red("- " + e.kind + " " + e.src_name + " -> " + e.dst_name) << "\n";
            }
        }
        ss << "\n";
    }

    if (!r.topology.empty()) {
        ss << bold("Topology Deltas:") << "\n";
        for (const auto& t : r.topology) {
            ss << "  " << yellow("~ " + t.symbol + " " + t.metric + ": " + std::to_string(t.old_value) + " -> " + std::to_string(t.new_value)) << "\n";
        }
        ss << "\n";
    }

    ss << bold("================================================================================") << "\n";
    return ss.str();
}

inline std::string format_diff_json(const GraphDiffReport& r) {
    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);

    yyjson_mut_obj_add_strcpy(doc.doc, root, "base", r.base_ref.c_str());
    yyjson_mut_obj_add_strcpy(doc.doc, root, "target", r.target_ref.c_str());
    yyjson_mut_obj_add_int(doc.doc, root, "files_changed", r.files_changed);

    auto* summary = doc.new_obj();
    yyjson_mut_obj_add_int(doc.doc, summary, "symbols_added", r.symbols_added);
    yyjson_mut_obj_add_int(doc.doc, summary, "symbols_removed", r.symbols_removed);
    yyjson_mut_obj_add_int(doc.doc, summary, "symbols_modified", r.symbols_modified);
    yyjson_mut_obj_add_int(doc.doc, summary, "edges_added", r.edges_added);
    yyjson_mut_obj_add_int(doc.doc, summary, "edges_removed", r.edges_removed);
    yyjson_mut_obj_add_val(doc.doc, root, "summary", summary);

    auto* sym_arr = doc.new_arr();
    for (const auto& s : r.symbols) {
        auto* item = doc.new_obj();
        yyjson_mut_obj_add_strcpy(doc.doc, item, "status", s.status.c_str());
        yyjson_mut_obj_add_strcpy(doc.doc, item, "kind", s.kind.c_str());
        yyjson_mut_obj_add_strcpy(doc.doc, item, "name", s.name.c_str());
        yyjson_mut_obj_add_strcpy(doc.doc, item, "qualname", s.qualname.c_str());
        yyjson_mut_obj_add_strcpy(doc.doc, item, "file", s.file.c_str());
        if (s.old_line > 0) yyjson_mut_obj_add_int(doc.doc, item, "old_line", s.old_line);
        if (s.new_line > 0) yyjson_mut_obj_add_int(doc.doc, item, "new_line", s.new_line);
        if (!s.old_signature.empty()) yyjson_mut_obj_add_strcpy(doc.doc, item, "old_signature", s.old_signature.c_str());
        if (!s.new_signature.empty()) yyjson_mut_obj_add_strcpy(doc.doc, item, "new_signature", s.new_signature.c_str());
        yyjson_mut_arr_append(sym_arr, item);
    }
    yyjson_mut_obj_add_val(doc.doc, root, "symbols", sym_arr);

    auto* edge_arr = doc.new_arr();
    for (const auto& e : r.edges) {
        auto* item = doc.new_obj();
        yyjson_mut_obj_add_strcpy(doc.doc, item, "status", e.status.c_str());
        yyjson_mut_obj_add_strcpy(doc.doc, item, "kind", e.kind.c_str());
        yyjson_mut_obj_add_strcpy(doc.doc, item, "src", e.src_name.c_str());
        yyjson_mut_obj_add_strcpy(doc.doc, item, "dst", e.dst_name.c_str());
        yyjson_mut_obj_add_strcpy(doc.doc, item, "file", e.src_file.c_str());
        yyjson_mut_arr_append(edge_arr, item);
    }
    yyjson_mut_obj_add_val(doc.doc, root, "edges", edge_arr);

    auto* top_arr = doc.new_arr();
    for (const auto& t : r.topology) {
        auto* item = doc.new_obj();
        yyjson_mut_obj_add_strcpy(doc.doc, item, "symbol", t.symbol.c_str());
        yyjson_mut_obj_add_strcpy(doc.doc, item, "metric", t.metric.c_str());
        yyjson_mut_obj_add_int(doc.doc, item, "old_value", t.old_value);
        yyjson_mut_obj_add_int(doc.doc, item, "new_value", t.new_value);
        yyjson_mut_arr_append(top_arr, item);
    }
    yyjson_mut_obj_add_val(doc.doc, root, "topology", top_arr);

    return doc.to_string();
}

} // namespace codetopo
