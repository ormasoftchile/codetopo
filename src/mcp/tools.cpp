// T060-T069: MCP tool implementations
// Split from tools.h — definitions moved here to reduce compile times.

#include "mcp/tools.h"

#include "util/path.h"
#include "util/git.h"
#include "mcp/error.h"
#include "db/schema.h"
#include <sqlite3.h>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <cstdint>
#include <set>
#include <algorithm>
#include <unordered_set>
#include <cstring>
#include <ctime>
#include <unordered_set>
#include <unordered_map>
#include <tuple>
#include <vector>
#include <algorithm>
#include <map>
#include <memory>
#include <cctype>

namespace codetopo {
namespace tools {

std::string read_source_snippet(const std::string& repo_root,
                                const std::string& rel_path,
                                int start_line, int end_line);

static void add_pagination_fields(JsonMutDoc& doc, yyjson_mut_val* root,
                                  yyjson_mut_val* results, int64_t total,
                                  bool has_more, int64_t offset, int64_t limit) {
    yyjson_mut_obj_add_val(doc.doc, root, "results", results);
    yyjson_mut_obj_add_int(doc.doc, root, "total", total);
    yyjson_mut_obj_add_bool(doc.doc, root, "has_more", has_more);
    yyjson_mut_obj_add_int(doc.doc, root, "offset", offset);
    yyjson_mut_obj_add_int(doc.doc, root, "limit", limit);
}

static bool include_field(const std::unordered_set<std::string>& fields_set,
                          const std::string& field) {
    return fields_set.empty() || fields_set.count(field);
}

static size_t cstr_len(const char* s) {
    return s ? std::strlen(s) : 0;
}

static constexpr const char* kPublicSymbolSql =
    "(n.visibility = 'public' OR "
    "(n.visibility IS NULL AND (n.qualname IS NULL OR n.qualname = n.name) "
    "AND n.kind IN ('class','struct','interface','type_alias','type','enum','function')))";

static bool parse_symbol_fields(yyjson_val* params,
                                std::unordered_set<std::string>& fields_set,
                                bool& fields_provided,
                                std::string& error) {
    fields_provided = false;
    fields_set.clear();
    error.clear();
    if (!params) return true;

    auto* fields = yyjson_obj_get(params, "fields");
    if (!fields) return true;
    if (!yyjson_is_arr(fields)) {
        error = "'fields' must be an array of strings";
        return false;
    }

    static const std::unordered_set<std::string> valid_fields = {
        "node_id", "name", "qualname", "kind", "signature",
        "start_line", "end_line", "span", "file_path", "file"
    };

    fields_provided = true;
    yyjson_val* field_val;
    size_t idx, max;
    yyjson_arr_foreach(fields, idx, max, field_val) {
        if (!yyjson_is_str(field_val)) {
            error = "'fields' entries must be strings";
            return false;
        }
        std::string field = yyjson_get_str(field_val);
        if (!valid_fields.count(field)) {
            error = "Invalid field name: " + field;
            return false;
        }
        fields_set.insert(field);
    }
    return true;
}

static void add_symbol_span_object(JsonMutDoc& doc, yyjson_mut_val* item,
                                   int start_line, int end_line) {
    auto* span = doc.new_obj();
    yyjson_mut_obj_add_int(doc.doc, span, "start_line", start_line);
    yyjson_mut_obj_add_int(doc.doc, span, "end_line", end_line);
    yyjson_mut_obj_add_val(doc.doc, item, "span", span);
}

static void add_symbol_span_array(JsonMutDoc& doc, yyjson_mut_val* item,
                                  int start_line, int end_line) {
    auto* span = doc.new_arr();
    yyjson_mut_arr_add_int(doc.doc, span, start_line);
    yyjson_mut_arr_add_int(doc.doc, span, end_line);
    yyjson_mut_obj_add_val(doc.doc, item, "span", span);
}

static void add_symbol_result(JsonMutDoc& doc, yyjson_mut_val* item,
                              int64_t node_id, const char* kind,
                              const char* name, const char* qualname,
                              const char* file_path, int start_line,
                              int end_line, bool compact,
                              bool compact_include_file,
                              const char* compact_file_key,
                              const std::unordered_set<std::string>& fields_set,
                              bool fields_provided) {
    if (fields_provided) {
        if (fields_set.empty()) {
            yyjson_mut_obj_add_int(doc.doc, item, "node_id", node_id);
            yyjson_mut_obj_add_strcpy(doc.doc, item, "kind", kind);
            yyjson_mut_obj_add_strcpy(doc.doc, item, "name", name);
            if (qualname) yyjson_mut_obj_add_strcpy(doc.doc, item, "qualname", qualname);
            if (file_path) yyjson_mut_obj_add_strcpy(doc.doc, item, "file_path", file_path);
            add_symbol_span_object(doc, item, start_line, end_line);
            return;
        }

        if (include_field(fields_set, "node_id"))
            yyjson_mut_obj_add_int(doc.doc, item, "node_id", node_id);
        if (include_field(fields_set, "kind"))
            yyjson_mut_obj_add_strcpy(doc.doc, item, "kind", kind);
        if (include_field(fields_set, "name"))
            yyjson_mut_obj_add_strcpy(doc.doc, item, "name", name);
        if (qualname && include_field(fields_set, "qualname"))
            yyjson_mut_obj_add_strcpy(doc.doc, item, "qualname", qualname);
        if (file_path && include_field(fields_set, "file_path"))
            yyjson_mut_obj_add_strcpy(doc.doc, item, "file_path", file_path);
        if (file_path && include_field(fields_set, "file"))
            yyjson_mut_obj_add_strcpy(doc.doc, item, "file", file_path);
        if (include_field(fields_set, "start_line"))
            yyjson_mut_obj_add_int(doc.doc, item, "start_line", start_line);
        if (include_field(fields_set, "end_line"))
            yyjson_mut_obj_add_int(doc.doc, item, "end_line", end_line);
        if (include_field(fields_set, "span"))
            add_symbol_span_array(doc, item, start_line, end_line);
        return;
    }

    yyjson_mut_obj_add_int(doc.doc, item, "node_id", node_id);
    yyjson_mut_obj_add_strcpy(doc.doc, item, "kind", kind);
    yyjson_mut_obj_add_strcpy(doc.doc, item, "name", name);
    if (!compact) {
        if (qualname) yyjson_mut_obj_add_strcpy(doc.doc, item, "qualname", qualname);
        if (file_path) yyjson_mut_obj_add_strcpy(doc.doc, item, "file_path", file_path);
        add_symbol_span_object(doc, item, start_line, end_line);
        return;
    }

    if (qualname && strcmp(qualname, name) != 0)
        yyjson_mut_obj_add_strcpy(doc.doc, item, "qualname", qualname);
    if (compact_include_file && file_path)
        yyjson_mut_obj_add_strcpy(doc.doc, item, compact_file_key, file_path);
    add_symbol_span_array(doc, item, start_line, end_line);
}

static void add_symbol_listing_result(JsonMutDoc& doc, yyjson_mut_val* item,
                                      int64_t node_id, const char* kind,
                                      const char* name, const char* qualname,
                                      const char* file_path, int start_line,
                                      int end_line, const char* signature,
                                      bool include_handles, bool compact,
                                      bool compact_include_file,
                                      const char* compact_file_key,
                                      const std::unordered_set<std::string>& fields_set,
                                      bool fields_provided) {
    if (fields_provided) {
        if (fields_set.empty()) {
            yyjson_mut_obj_add_int(doc.doc, item, "node_id", node_id);
            yyjson_mut_obj_add_strcpy(doc.doc, item, "kind", kind);
            yyjson_mut_obj_add_strcpy(doc.doc, item, "name", name);
            if (qualname) yyjson_mut_obj_add_strcpy(doc.doc, item, "qualname", qualname);
            if (signature) yyjson_mut_obj_add_strcpy(doc.doc, item, "signature", signature);
            if (file_path) yyjson_mut_obj_add_strcpy(doc.doc, item, "file_path", file_path);
            add_symbol_span_object(doc, item, start_line, end_line);
            return;
        }

        if (include_field(fields_set, "node_id"))
            yyjson_mut_obj_add_int(doc.doc, item, "node_id", node_id);
        if (include_field(fields_set, "kind"))
            yyjson_mut_obj_add_strcpy(doc.doc, item, "kind", kind);
        if (include_field(fields_set, "name"))
            yyjson_mut_obj_add_strcpy(doc.doc, item, "name", name);
        if (qualname && include_field(fields_set, "qualname"))
            yyjson_mut_obj_add_strcpy(doc.doc, item, "qualname", qualname);
        if (signature && include_field(fields_set, "signature"))
            yyjson_mut_obj_add_strcpy(doc.doc, item, "signature", signature);
        if (file_path && include_field(fields_set, "file_path"))
            yyjson_mut_obj_add_strcpy(doc.doc, item, "file_path", file_path);
        if (file_path && include_field(fields_set, "file"))
            yyjson_mut_obj_add_strcpy(doc.doc, item, "file", file_path);
        if (include_field(fields_set, "start_line"))
            yyjson_mut_obj_add_int(doc.doc, item, "start_line", start_line);
        if (include_field(fields_set, "end_line"))
            yyjson_mut_obj_add_int(doc.doc, item, "end_line", end_line);
        if (include_field(fields_set, "span"))
            add_symbol_span_array(doc, item, start_line, end_line);
        return;
    }

    if (include_handles) {
        add_symbol_result(doc, item, node_id, kind, name, qualname, file_path,
                          start_line, end_line, compact, compact_include_file,
                          compact_file_key, fields_set, false);
        if (signature) yyjson_mut_obj_add_strcpy(doc.doc, item, "signature", signature);
        return;
    }

    yyjson_mut_obj_add_strcpy(doc.doc, item, "kind", kind);
    yyjson_mut_obj_add_strcpy(doc.doc, item, "name", name);
    if (signature) yyjson_mut_obj_add_strcpy(doc.doc, item, "signature", signature);
    if (file_path) yyjson_mut_obj_add_strcpy(doc.doc, item, "file", file_path);
    yyjson_mut_obj_add_int(doc.doc, item, "start_line", start_line);
}

static size_t estimate_symbol_listing_bytes(const char* kind, const char* name,
                                            const char* qualname,
                                            const char* file_path,
                                            const char* signature,
                                            bool include_handles,
                                            bool fields_provided,
                                            const std::unordered_set<std::string>& fields_set) {
    size_t bytes = 48 + cstr_len(kind) + cstr_len(name);
    bool include_file = !fields_provided || fields_set.empty()
        || fields_set.count("file") || fields_set.count("file_path");
    bool include_signature = signature && (!fields_provided || fields_set.empty()
        || fields_set.count("signature"));
    bool include_qualname = qualname && (include_handles || fields_set.count("qualname")
        || (fields_provided && fields_set.empty()));

    if (include_file) bytes += 16 + cstr_len(file_path);
    if (include_signature) bytes += 16 + cstr_len(signature);
    if (include_qualname) bytes += 16 + cstr_len(qualname);
    if (include_handles || fields_set.count("node_id")) bytes += 24;
    if (include_handles || fields_set.count("span")) bytes += 36;
    if (fields_set.count("start_line")) bytes += 18;
    if (fields_set.count("end_line")) bytes += 18;
    return bytes;
}

// Resolve a node by node_id, or by symbol+file name lookup.
// Allows tools to be called with stable identifiers instead of volatile IDs.
static int64_t resolve_node_id(yyjson_val* params, Connection& conn, QueryCache& cache,
                                const char* id_param = "node_id") {
    if (!params) return -1;
    int64_t id = json_get_int(params, id_param, -1);
    if (id >= 0) return id;

    auto* sym_val = yyjson_obj_get(params, "symbol");
    auto* file_val = yyjson_obj_get(params, "file");
    if (!sym_val || !file_val) return -1;

    auto* stmt = cache.get("resolve_node_by_name_file",
        "SELECT id FROM nodes WHERE name = ? "
        "AND file_id = (SELECT id FROM files WHERE path = ?) "
        "AND node_type = 'symbol' "
        "ORDER BY is_definition DESC, id ASC LIMIT 1");
    sqlite3_bind_text(stmt, 1, yyjson_get_str(sym_val), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, yyjson_get_str(file_val), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) return sqlite3_column_int64(stmt, 0);
    return -1;
}

enum class CandidateMode {
    ExactOnly,
    ExactThenCandidates,
    ExactPlusCandidates
};

struct CallsiteCandidate {
    int64_t ref_id = -1;
    int64_t file_id = -1;
    int64_t caller_node_id = -1;
    std::string caller_name;
    std::string caller_qualname;
    std::string file_path;
    std::string callee_text;
    std::string receiver_hint;
    std::string receiver_type_hint;
    std::string resolved_receiver_type;
    std::string receiver_type_resolution_source;
    std::string arg_pattern;
    std::string heuristic;
    std::string why;
    std::string evidence;
    int arg_count = -1;
    int start_line = 0;
    int start_col = 0;
    int end_line = 0;
    int end_col = 0;
    double confidence = 0.0;
    bool receiver_type_ambiguous = false;
};

struct CallsiteCandidateOptions {
    std::string receiver_filter;
    bool include_handles = false;
    int64_t max_bytes = 16000;
};

struct CallsiteCandidateSet {
    std::vector<CallsiteCandidate> rows;
    int64_t total = 0;
    int64_t eligible_total = 0;
    int64_t filtered_hidden = 0;
    int64_t arity_filtered = 0;
    bool budget_exceeded = false;
    bool has_more = false;
    int64_t max_bytes = 16000;
    struct ArityBucket {
        int64_t count = 0;
        std::map<std::string, int64_t> files;
    };
    std::map<int, ArityBucket> arity_buckets;
    std::map<std::string, int64_t> heuristic_buckets;
    std::map<std::string, int64_t> file_buckets;
};

struct TargetCallsiteInfo {
    std::string name;
    std::string qualname;
    std::string owner_name;
    std::string owner_qualname;
    std::string signature;
    int64_t file_id = -1;
    std::string file_path;
    int start_line = 0;
    int end_line = 0;
    int min_param_count = -1;
    int param_count = -1;
    std::string param_pattern;
};

static std::string lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

static bool iequals(const std::string& lhs, const std::string& rhs) {
    return lower_copy(lhs) == lower_copy(rhs);
}

static std::string trailing_identifier(const std::string& text);

static bool iends_with_separator_suffix(const std::string& text,
                                        const std::string& suffix) {
    if (text.size() <= suffix.size() || suffix.empty()) return false;
    std::string lhs = lower_copy(text);
    std::string rhs = lower_copy(suffix);
    if (lhs.compare(lhs.size() - rhs.size(), rhs.size(), rhs) != 0) return false;
    size_t sep_end = lhs.size() - rhs.size();
    if (sep_end == 0) return false;
    char prev = lhs[sep_end - 1];
    return prev == '.' || prev == ':' || prev == '#' || prev == '/' || prev == '>';
}

static bool receiver_text_matches_name(const std::string& receiver,
                                       const std::string& name) {
    if (receiver.empty() || name.empty()) return false;
    if (iequals(receiver, name)) return true;
    if (iends_with_separator_suffix(receiver, name) ||
        iends_with_separator_suffix(name, receiver)) return true;
    std::string receiver_leaf = trailing_identifier(receiver);
    std::string name_leaf = trailing_identifier(name);
    return !receiver_leaf.empty() && !name_leaf.empty() && iequals(receiver_leaf, name_leaf);
}

static std::string trim_receiver_hint(std::string s) {
    while (!s.empty() && (s.back() == '?' || std::isspace(static_cast<unsigned char>(s.back()))))
        s.pop_back();
    size_t first = 0;
    while (first < s.size() && std::isspace(static_cast<unsigned char>(s[first]))) ++first;
    if (first > 0) s.erase(0, first);
    return s;
}

static std::string trailing_identifier(const std::string& text) {
    if (text.empty()) return {};
    size_t end = text.size();
    while (end > 0) {
        unsigned char c = static_cast<unsigned char>(text[end - 1]);
        if (std::isalnum(c) || c == '_') break;
        --end;
    }
    size_t start = end;
    while (start > 0) {
        unsigned char c = static_cast<unsigned char>(text[start - 1]);
        if (!(std::isalnum(c) || c == '_')) break;
        --start;
    }
    return text.substr(start, end - start);
}

static std::string qualname_owner(const std::string& qualname) {
    if (qualname.empty()) return {};
    size_t pos = std::string::npos;
    for (const char* sep : {"::", ".", "#", "/"}) {
        size_t p = qualname.rfind(sep);
        if (p != std::string::npos && (pos == std::string::npos || p > pos)) pos = p;
    }

    if (pos == std::string::npos) return {};
    return qualname.substr(0, pos);
}


static std::string strip_generic_suffix(std::string s) {
    int depth = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '<') {
            if (depth == 0) return trim_receiver_hint(s.substr(0, i));
            ++depth;
        } else if (s[i] == '>' && depth > 0) {
            --depth;
        }
    }
    return s;
}

static std::string canonical_type_hint(std::string s) {
    s = trim_receiver_hint(std::move(s));
    if (s.empty()) return {};
    for (const char* prefix : {"readonly ", "typeof "}) {
        size_t len = std::strlen(prefix);
        if (s.size() > len && lower_copy(s.substr(0, len)) == prefix)
            s = trim_receiver_hint(s.substr(len));
    }
    size_t split = s.find_first_of("|&=");
    if (split != std::string::npos) s = trim_receiver_hint(s.substr(0, split));
    s = strip_generic_suffix(s);
    while (s.size() >= 2 && s.compare(s.size() - 2, 2, "[]") == 0)
        s = trim_receiver_hint(s.substr(0, s.size() - 2));
    while (!s.empty() && (s.back() == '*' || s.back() == '&' || s.back() == '?' ||
                          std::isspace(static_cast<unsigned char>(s.back()))))
        s.pop_back();
    return trim_receiver_hint(s);
}

static int64_t parse_max_bytes(yyjson_val* params, int64_t default_value = 16000) {
    int64_t max_bytes = params ? json_get_int(params, "max_bytes", default_value) : default_value;
    if (max_bytes < 0) max_bytes = 0;
    if (max_bytes > 100000) max_bytes = 100000;
    return max_bytes;
}

static CallsiteCandidateOptions parse_callsite_candidate_options(yyjson_val* params) {
    CallsiteCandidateOptions options;
    if (!params) return options;
    if (const char* receiver = json_get_str(params, "receiver"))
        options.receiver_filter = trim_receiver_hint(receiver);
    options.include_handles = json_get_bool(params, "include_handles", false);
    options.max_bytes = parse_max_bytes(params);
    return options;
}

static CandidateMode parse_candidate_mode(yyjson_val* params) {
    CandidateMode mode = CandidateMode::ExactThenCandidates;
    if (!params) return mode;

    if (auto* mode_val = yyjson_obj_get(params, "mode")) {
        if (yyjson_is_str(mode_val)) {
            std::string raw = yyjson_get_str(mode_val);
            if (raw == "exact") mode = CandidateMode::ExactOnly;
            else if (raw == "exact_plus_candidates") mode = CandidateMode::ExactPlusCandidates;
            else if (raw == "exact_then_candidates") mode = CandidateMode::ExactThenCandidates;
        }
    }

    if (auto* include_val = yyjson_obj_get(params, "include_candidates")) {
        if (yyjson_is_bool(include_val)) {
            mode = yyjson_get_bool(include_val)
                ? CandidateMode::ExactPlusCandidates
                : CandidateMode::ExactOnly;
        }
    }
    return mode;
}

static bool should_collect_candidates(CandidateMode mode, bool exact_empty) {
    return mode == CandidateMode::ExactPlusCandidates
        || (mode == CandidateMode::ExactThenCandidates && exact_empty);
}

static bool is_common_member_name(const std::string& name) {
    static const std::unordered_set<std::string> common = {
        "set", "get", "add", "remove", "delete", "dispose", "clear", "push",
        "pop", "map", "filter", "find", "open", "close", "read", "write"
    };
    return common.count(lower_copy(name)) > 0;
}

enum class ReceiverMatchStrength {
    None = 0,
    Bare = 1,
    Suffix = 2,
    Exact = 3
};

static ReceiverMatchStrength receiver_owner_match_strength(const std::string& receiver,
                                                           const TargetCallsiteInfo& target) {
    if (receiver.empty()) return ReceiverMatchStrength::None;
    if ((!target.owner_qualname.empty() && iequals(receiver, target.owner_qualname)) ||
        (!target.owner_name.empty() && iequals(receiver, target.owner_name))) {
        return ReceiverMatchStrength::Exact;
    }

    if ((!target.owner_qualname.empty() &&
         (iends_with_separator_suffix(receiver, target.owner_qualname) ||
          iends_with_separator_suffix(target.owner_qualname, receiver))) ||
        (!target.owner_name.empty() && iends_with_separator_suffix(receiver, target.owner_name))) {
        return ReceiverMatchStrength::Suffix;
    }

    std::string receiver_leaf = trailing_identifier(receiver);
    std::string owner_leaf = trailing_identifier(target.owner_name);
    std::string owner_qual_leaf = trailing_identifier(target.owner_qualname);
    if (!receiver_leaf.empty() &&
        ((!owner_leaf.empty() && iequals(receiver_leaf, owner_leaf)) ||
         (!owner_qual_leaf.empty() && iequals(receiver_leaf, owner_qual_leaf)))) {
        return ReceiverMatchStrength::Bare;
    }

    return ReceiverMatchStrength::None;
}

struct TypeCandidateNode {
    int64_t id = 0;
    std::string name;
    std::string qualname;
};

struct ReceiverTypeResolution {
    ReceiverMatchStrength strength = ReceiverMatchStrength::None;
    bool resolved = false;
    bool ambiguous = false;
    std::string resolved_type;
    std::string source;
};

static ReceiverMatchStrength type_candidate_owner_match(const TypeCandidateNode& candidate,
                                                       const TargetCallsiteInfo& target) {
    if (!candidate.qualname.empty() && !target.owner_qualname.empty() &&
        iequals(candidate.qualname, target.owner_qualname))
        return ReceiverMatchStrength::Exact;
    if (!candidate.qualname.empty() && !target.owner_qualname.empty() &&
        (iends_with_separator_suffix(candidate.qualname, target.owner_qualname) ||
         iends_with_separator_suffix(target.owner_qualname, candidate.qualname)))
        return ReceiverMatchStrength::Suffix;
    if (!candidate.name.empty() && !target.owner_name.empty() &&
        iequals(candidate.name, target.owner_name))
        return ReceiverMatchStrength::Bare;
    if (!candidate.qualname.empty() && !target.owner_name.empty() &&
        iends_with_separator_suffix(candidate.qualname, target.owner_name))
        return ReceiverMatchStrength::Suffix;
    return ReceiverMatchStrength::None;
}

static ReceiverTypeResolution choose_receiver_type_resolution(
        const std::vector<TypeCandidateNode>& candidates,
        const TargetCallsiteInfo& target,
        const std::string& source) {
    ReceiverTypeResolution result;
    if (candidates.empty()) return result;
    result.source = source;

    std::vector<std::pair<TypeCandidateNode, ReceiverMatchStrength>> matches;
    for (const auto& candidate : candidates) {
        auto strength = type_candidate_owner_match(candidate, target);
        if (strength != ReceiverMatchStrength::None)
            matches.push_back({candidate, strength});
    }
    if (matches.empty()) {
        result.ambiguous = candidates.size() > 1;
        return result;
    }

    auto best = matches.front();
    for (const auto& match : matches) {
        if (static_cast<int>(match.second) > static_cast<int>(best.second))
            best = match;
    }

    int equally_good = 0;
    for (const auto& match : matches) {
        if (match.second == best.second) ++equally_good;
    }
    if (equally_good > 1) {
        result.ambiguous = true;
        return result;
    }

    if (source == "global" && candidates.size() > 1) {
        result.ambiguous = true;
        return result;
    }

    result.resolved = true;
    result.strength = best.second;
    result.resolved_type = best.first.qualname.empty() ? best.first.name : best.first.qualname;
    return result;
}

static std::vector<TypeCandidateNode> fetch_type_candidates(sqlite3_stmt* stmt) {
    std::vector<TypeCandidateNode> candidates;
    std::unordered_set<int64_t> seen;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        TypeCandidateNode candidate;
        candidate.id = sqlite3_column_int64(stmt, 0);
        if (!seen.insert(candidate.id).second) continue;
        auto* name = sqlite3_column_text(stmt, 1);
        auto* qualname = sqlite3_column_text(stmt, 2);
        candidate.name = name ? reinterpret_cast<const char*>(name) : "";
        candidate.qualname = qualname ? reinterpret_cast<const char*>(qualname) : "";
        candidates.push_back(std::move(candidate));
    }
    return candidates;
}

static ReceiverTypeResolution resolve_receiver_type_hint(
        const CallsiteCandidate& row,
        const TargetCallsiteInfo& target,
        Connection& /*conn*/,
        QueryCache& cache) {
    std::string type_name = canonical_type_hint(row.receiver_type_hint);
    if (type_name.empty() || row.file_id <= 0) return {};
    std::string type_leaf = trailing_identifier(type_name);
    if (type_leaf.empty()) type_leaf = type_name;
    std::string like_qual = "%." + type_leaf;

    auto resolve_with = [&](const std::string& key, const std::string& sql,
                            const std::string& source) {
        auto* stmt = cache.get(key, sql);
        sqlite3_bind_int64(stmt, 1, row.file_id);
        sqlite3_bind_text(stmt, 2, type_leaf.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, type_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, like_qual.c_str(), -1, SQLITE_TRANSIENT);
        return choose_receiver_type_resolution(fetch_type_candidates(stmt), target, source);
    };

    auto same_file = resolve_with("approx_receiver_type_same_file",
        "SELECT n.id, n.name, n.qualname "
        "FROM nodes n "
        "WHERE n.file_id = ? AND n.node_type = 'symbol' "
        "AND n.kind IN ('class','struct','interface','type_alias','type','enum') "
        "AND (n.name = ? OR n.qualname = ? OR n.qualname LIKE ?) "
        "ORDER BY n.is_definition DESC, n.id ASC LIMIT 8",
        "same_file");
    if (same_file.resolved || same_file.ambiguous) return same_file;

    auto included = resolve_with("approx_receiver_type_included_file",
        "SELECT n.id, n.name, n.qualname "
        "FROM files cf "
        "JOIN nodes cfn ON cfn.node_type = 'file' AND cfn.name = cf.path "
        "JOIN edges e ON e.src_id = cfn.id AND e.kind = 'includes' "
        "JOIN nodes ifn ON ifn.id = e.dst_id AND ifn.node_type = 'file' "
        "JOIN files inf ON inf.path = ifn.name "
        "JOIN nodes n ON n.file_id = inf.id "
        "WHERE cf.id = ? AND n.node_type = 'symbol' "
        "AND n.kind IN ('class','struct','interface','type_alias','type','enum') "
        "AND (n.name = ? OR n.qualname = ? OR n.qualname LIKE ?) "
        "ORDER BY n.is_definition DESC, n.id ASC LIMIT 12",
        "include");
    if (included.resolved || included.ambiguous) return included;

    auto* global_stmt = cache.get("approx_receiver_type_global",
        "SELECT n.id, n.name, n.qualname "
        "FROM nodes n "
        "WHERE n.node_type = 'symbol' "
        "AND n.kind IN ('class','struct','interface','type_alias','type','enum') "
        "AND (n.name = ? OR n.qualname = ? OR n.qualname LIKE ?) "
        "ORDER BY n.is_definition DESC, n.id ASC LIMIT 16");
    sqlite3_bind_text(global_stmt, 1, type_leaf.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(global_stmt, 2, type_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(global_stmt, 3, like_qual.c_str(), -1, SQLITE_TRANSIENT);
    return choose_receiver_type_resolution(fetch_type_candidates(global_stmt), target, "global");
}

static bool containing_context_matches_owner(const std::string& containing_name,
                                             const std::string& containing_qualname,
                                             const TargetCallsiteInfo& target) {
    std::string containing_owner = qualname_owner(containing_qualname);
    return (!target.owner_name.empty() &&
            (iequals(containing_name, target.owner_name) ||
             iequals(containing_owner, target.owner_name))) ||
           (!target.owner_qualname.empty() &&
            (iequals(containing_qualname, target.owner_qualname) ||
             iequals(containing_owner, target.owner_qualname)));
}

static std::string extract_receiver_hint(const std::string& callee_text,
                                         const std::string& target_name) {
    if (target_name.empty() || callee_text.size() <= target_name.size()) return {};
    auto ends_with = [&](const std::string& suffix) {
        return callee_text.size() >= suffix.size() &&
               callee_text.compare(callee_text.size() - suffix.size(), suffix.size(), suffix) == 0;
    };

    std::string suffix = "." + target_name;
    if (ends_with(suffix))
        return trim_receiver_hint(callee_text.substr(0, callee_text.size() - suffix.size()));
    suffix = "::" + target_name;
    if (ends_with(suffix))
        return trim_receiver_hint(callee_text.substr(0, callee_text.size() - suffix.size()));
    suffix = "->" + target_name;
    if (ends_with(suffix))
        return trim_receiver_hint(callee_text.substr(0, callee_text.size() - suffix.size()));
    return {};
}

static std::string trim_ascii(std::string s) {
    size_t first = 0;
    while (first < s.size() && std::isspace(static_cast<unsigned char>(s[first]))) ++first;
    size_t last = s.size();
    while (last > first && std::isspace(static_cast<unsigned char>(s[last - 1]))) --last;
    return s.substr(first, last - first);
}

static std::vector<std::string> split_top_level_commas(const std::string& text) {
    std::vector<std::string> parts;
    std::string current;
    int paren = 0, angle = 0, brace = 0, bracket = 0;
    char quote = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (quote) {
            current += c;
            if (c == quote && (i == 0 || text[i - 1] != '\\')) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'' || c == '`') {
            quote = c;
            current += c;
            continue;
        }
        if (c == '(') ++paren;
        else if (c == ')' && paren > 0) --paren;
        else if (c == '<') ++angle;
        else if (c == '>' && angle > 0) --angle;
        else if (c == '{') ++brace;
        else if (c == '}' && brace > 0) --brace;
        else if (c == '[') ++bracket;
        else if (c == ']' && bracket > 0) --bracket;
        if (c == ',' && paren == 0 && angle == 0 && brace == 0 && bracket == 0) {
            parts.push_back(trim_ascii(current));
            current.clear();
        } else {
            current += c;
        }
    }
    auto tail = trim_ascii(current);
    if (!tail.empty() || !parts.empty()) parts.push_back(tail);
    if (parts.size() == 1) {
        auto only = lower_copy(trim_ascii(parts[0]));
        if (only.empty() || only == "void") parts.clear();
    }
    return parts;
}

static std::string classify_param_shape(std::string param) {
    param = lower_copy(param);
    if (param.find("=>") != std::string::npos || param.find("function") != std::string::npos ||
        param.find("callback") != std::string::npos || param.find("std::function") != std::string::npos)
        return "callback";
    if (param.find("string") != std::string::npos || param.find("char") != std::string::npos)
        return "string";
    if (param.find("number") != std::string::npos || param.find("int") != std::string::npos ||
        param.find("float") != std::string::npos || param.find("double") != std::string::npos ||
        param.find("long") != std::string::npos || param.find("size_t") != std::string::npos)
        return "number";
    if (param.find("bool") != std::string::npos)
        return "bool";
    if (param.find("[]") != std::string::npos || param.find("array") != std::string::npos ||
        param.find("vector") != std::string::npos || param.find("list") != std::string::npos)
        return "array";
    if (param.find("object") != std::string::npos || param.find("record") != std::string::npos ||
        param.find("{") != std::string::npos)
        return "object";
    if (param.find("any") != std::string::npos || param.find("unknown") != std::string::npos)
        return "unknown";
    return "unknown";
}

static bool has_top_level_char(const std::string& text, char needle) {
    int paren = 0, angle = 0, brace = 0, bracket = 0;
    char quote = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (quote) {
            if (c == quote && (i == 0 || text[i - 1] != '\\')) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'' || c == '`') {
            quote = c;
            continue;
        }
        if (c == '(') ++paren;
        else if (c == ')' && paren > 0) --paren;
        else if (c == '<') ++angle;
        else if (c == '>' && angle > 0) --angle;
        else if (c == '{') ++brace;
        else if (c == '}' && brace > 0) --brace;
        else if (c == '[') ++bracket;
        else if (c == ']' && bracket > 0) --bracket;
        else if (c == needle && paren == 0 && angle == 0 && brace == 0 && bracket == 0) {
            return true;
        }
    }
    return false;
}

static bool param_is_optional(const std::string& param) {
    if (has_top_level_char(param, '=')) return true;
    size_t colon = param.find(':');
    size_t limit = colon == std::string::npos ? param.size() : colon;
    for (size_t i = 0; i < limit; ++i) {
        if (param[i] == '?') return true;
    }
    return false;
}

static std::string join_pattern_parts(const std::vector<std::string>& parts) {
    std::string out;
    for (const auto& part : parts) {
        if (!out.empty()) out += ",";
        out += part;
    }
    return out;
}

static bool parse_param_shape_from_text(const std::string& text,
                                        const std::string& target_name,
                                        int& min_param_count,
                                        int& param_count,
                                        std::string& param_pattern) {
    if (text.empty()) return false;
    size_t search_from = 0;
    size_t open = std::string::npos;
    while (true) {
        size_t name_pos = target_name.empty() ? std::string::npos : text.find(target_name, search_from);
        if (name_pos == std::string::npos) {
            open = text.find('(', search_from);
        } else {
            open = text.find('(', name_pos + target_name.size());
        }
        if (open == std::string::npos) return false;
        break;
    }

    int depth = 0;
    size_t close = std::string::npos;
    for (size_t i = open; i < text.size(); ++i) {
        if (text[i] == '(') ++depth;
        else if (text[i] == ')' && --depth == 0) {
            close = i;
            break;
        }
    }
    if (close == std::string::npos || close <= open) return false;

    auto params = split_top_level_commas(text.substr(open + 1, close - open - 1));
    param_count = static_cast<int>(params.size());
    min_param_count = 0;
    std::vector<std::string> shapes;
    shapes.reserve(params.size());
    for (auto& p : params) {
        if (!param_is_optional(p)) ++min_param_count;
        shapes.push_back(classify_param_shape(p));
    }
    param_pattern = join_pattern_parts(shapes);
    return true;
}

static void populate_target_param_shape(TargetCallsiteInfo& target,
                                        const std::string& repo_root) {
    if (parse_param_shape_from_text(target.signature, target.name, target.min_param_count,
                                    target.param_count, target.param_pattern))
        return;
    if (target.file_path.empty() || target.start_line <= 0 || target.end_line <= 0) return;
    auto source = read_source_snippet(repo_root, target.file_path, target.start_line, target.end_line);
    parse_param_shape_from_text(source, target.name, target.min_param_count,
                                target.param_count, target.param_pattern);
}

static std::vector<std::string> split_pattern(const std::string& pattern) {
    std::vector<std::string> parts;
    std::string current;
    for (char c : pattern) {
        if (c == ',') {
            parts.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty() || !pattern.empty()) parts.push_back(current);
    return parts;
}

static std::string canonical_call_shape(std::string shape) {
    shape = lower_copy(shape);
    if (shape.rfind("new:", 0) == 0) return "object";
    if (shape == "string" || shape == "number" || shape == "bool" ||
        shape == "object" || shape == "array" || shape == "callback")
        return shape;
    return "unknown";
}

static double arg_pattern_bonus(const std::string& call_pattern,
                                const std::string& param_pattern) {
    auto calls = split_pattern(call_pattern);
    auto params = split_pattern(param_pattern);
    if (calls.empty() || params.empty() || calls.size() != params.size()) return 0.0;
    int comparable = 0;
    int matches = 0;
    for (size_t i = 0; i < calls.size(); ++i) {
        auto c = canonical_call_shape(calls[i]);
        auto p = params[i];
        if (c == "unknown" || p == "unknown") continue;
        ++comparable;
        if (c == p) ++matches;
    }
    if (comparable == 0 || matches == 0) return 0.0;
    return 0.04 * (static_cast<double>(matches) / static_cast<double>(comparable));
}

static const char* receiver_match_name(ReceiverMatchStrength strength) {
    switch (strength) {
        case ReceiverMatchStrength::Exact: return "receiver_exact_target_owner";
        case ReceiverMatchStrength::Suffix: return "receiver_suffix_target_owner";
        case ReceiverMatchStrength::Bare: return "receiver_bare_target_owner";
        case ReceiverMatchStrength::None: break;
    }
    return "receiver_unrelated";
}

static void score_candidate(CallsiteCandidate& row,
                            const TargetCallsiteInfo& target,
                            const std::string& containing_name,
                            const std::string& containing_qualname) {
    bool has_receiver = !row.receiver_hint.empty();
    ReceiverMatchStrength receiver_match = receiver_owner_match_strength(row.receiver_hint, target);
    const std::string& receiver_type_for_score =
        row.resolved_receiver_type.empty() ? row.receiver_type_hint : row.resolved_receiver_type;
    ReceiverMatchStrength receiver_type_match = row.receiver_type_ambiguous
        ? ReceiverMatchStrength::None
        : receiver_owner_match_strength(receiver_type_for_score, target);
    bool strong_owner = receiver_match != ReceiverMatchStrength::None;
    bool strong_receiver_type = receiver_type_match != ReceiverMatchStrength::None;
    bool this_in_owner = iequals(row.receiver_hint, "this") &&
                         containing_context_matches_owner(containing_name, containing_qualname, target);
    std::string owner_hint = !target.owner_qualname.empty() ? target.owner_qualname : target.owner_name;

    if (strong_owner) {
        if (receiver_match == ReceiverMatchStrength::Exact) {
            row.confidence = 0.90;
            row.why = "call receiver text exactly matches the target's containing type";
        } else if (receiver_match == ReceiverMatchStrength::Suffix) {
            row.confidence = 0.82;
            row.why = "call receiver text suffix matches the target's containing type";
        } else {
            row.confidence = 0.72;
            row.why = "call receiver bare name matches the target's containing type";
        }
        row.heuristic = receiver_match_name(receiver_match);
        row.receiver_type_hint = owner_hint;
    } else if (strong_receiver_type) {
        bool resolved_cross_file = !row.receiver_type_resolution_source.empty() &&
                                   row.receiver_type_resolution_source != "same_file";
        if (resolved_cross_file) {
            row.confidence = receiver_type_match == ReceiverMatchStrength::Exact ? 0.93 : 0.86;
            row.heuristic = receiver_type_match == ReceiverMatchStrength::Exact
                ? "cross_file_receiver_type_exact"
                : "cross_file_receiver_type_suffix";
            row.why = "resolved receiver declaration type matches the target's containing type";
        } else {
            row.confidence = receiver_type_match == ReceiverMatchStrength::Exact ? 0.89 : 0.81;
            row.heuristic = receiver_type_match == ReceiverMatchStrength::Exact
                ? "local_receiver_type_exact_target_owner"
                : "local_receiver_type_matches_target_owner";
            row.why = "local receiver declaration type matches the target's containing type";
        }
        if (!row.resolved_receiver_type.empty())
            row.receiver_type_hint = row.resolved_receiver_type;
    } else if (this_in_owner) {
        row.confidence = 0.68;
        row.heuristic = "this_receiver_in_target_owner";
        row.receiver_type_hint = owner_hint;
        row.why = "this receiver appears inside the target's containing type";
    } else if (has_receiver) {
        row.confidence = 0.40;
        row.heuristic = "unrelated_member_suffix_match";
        row.why = "call ref ends with the target member name but receiver text does not match the target type";
    } else {
        row.confidence = 0.35;
        row.heuristic = "bare_name_match";
        row.why = "bare call ref matches the target name";
    }

    if (is_common_member_name(target.name) && !strong_owner && !strong_receiver_type && !this_in_owner) {
        row.confidence = std::max(0.20, row.confidence - 0.10);
        row.why += "; confidence reduced for common member name";
    }
    if (row.receiver_type_ambiguous) {
        row.why += "; receiver type hint was ambiguous across visible types";
    }
}

static bool apply_arity_and_pattern_score(CallsiteCandidate& row,
                                          const TargetCallsiteInfo& target) {
    if (target.param_count < 0 || row.arg_count < 0) return true;
    int min_param_count = target.min_param_count >= 0 ? target.min_param_count : target.param_count;
    if (row.arg_count < min_param_count || row.arg_count > target.param_count) {
        row.heuristic = "arity_mismatch_filtered";
        row.why = "call arity does not match the target overload parameter count";
        return false;
    }

    row.confidence = std::min(0.99, row.confidence + 0.06);
    row.heuristic += "+arity_match";
    row.why += "; call arity matches target overload";
    double bonus = arg_pattern_bonus(row.arg_pattern, target.param_pattern);
    if (bonus > 0.0) {
        row.confidence = std::min(0.99, row.confidence + bonus);
        row.heuristic += "+arg_pattern";
        row.why += "; argument pattern is compatible with target parameters";
    }
    return true;
}

static bool candidate_matches_receiver_filter(const CallsiteCandidate& row,
                                              const std::string& receiver_filter) {
    if (receiver_filter.empty()) return true;
    return receiver_text_matches_name(row.receiver_hint, receiver_filter) ||
           receiver_text_matches_name(row.receiver_type_hint, receiver_filter);
}

static size_t estimate_callsite_candidate_bytes(const CallsiteCandidate& row,
                                                bool include_handles) {
    size_t bytes = 96 + cstr_len(row.caller_name.c_str()) + cstr_len(row.file_path.c_str()) +
                   cstr_len(row.callee_text.c_str()) + cstr_len(row.receiver_hint.c_str()) +
                   cstr_len(row.receiver_type_hint.c_str()) + cstr_len(row.arg_pattern.c_str()) +
                   cstr_len(row.heuristic.c_str());
    if (include_handles) {
        bytes += 120 + cstr_len(row.caller_qualname.c_str()) + cstr_len(row.why.c_str()) +
                 cstr_len(row.evidence.c_str());
    }
    return bytes;
}

static CallsiteCandidateSet collect_callsite_candidates(
        int64_t target_node_id, int64_t limit, Connection& conn, QueryCache& cache,
        const CallsiteCandidateOptions& options, const std::string& repo_root) {
    CallsiteCandidateSet result;
    result.max_bytes = options.max_bytes;
    if (limit <= 0) return result;
    if (limit > 500) limit = 500;

    TargetCallsiteInfo target;
    auto* target_stmt = cache.get("approx_callsite_target",
        "SELECT n.name, n.qualname, cn.name, cn.qualname, n.signature, n.file_id, f.path, n.start_line, n.end_line "
        "FROM nodes n "
        "LEFT JOIN edges ce ON ce.dst_id = n.id AND ce.kind = 'contains' "
        "LEFT JOIN nodes cn ON cn.id = ce.src_id AND cn.node_type = 'symbol' "
        "LEFT JOIN files f ON f.id = n.file_id "
        "WHERE n.id = ? AND n.node_type = 'symbol' LIMIT 1");
    sqlite3_bind_int64(target_stmt, 1, target_node_id);
    if (sqlite3_step(target_stmt) != SQLITE_ROW) return result;

    auto* name_txt = sqlite3_column_text(target_stmt, 0);
    if (!name_txt) return result;
    target.name = reinterpret_cast<const char*>(name_txt);
    auto* qn_txt = sqlite3_column_text(target_stmt, 1);
    target.qualname = qn_txt ? reinterpret_cast<const char*>(qn_txt) : "";
    auto* owner_txt = sqlite3_column_text(target_stmt, 2);
    target.owner_name = owner_txt ? reinterpret_cast<const char*>(owner_txt) : "";
    auto* owner_qn_txt = sqlite3_column_text(target_stmt, 3);
    target.owner_qualname = owner_qn_txt ? reinterpret_cast<const char*>(owner_qn_txt) : "";
    auto* sig_txt = sqlite3_column_text(target_stmt, 4);
    target.signature = sig_txt ? reinterpret_cast<const char*>(sig_txt) : "";
    target.file_id = sqlite3_column_int64(target_stmt, 5);
    auto* file_txt = sqlite3_column_text(target_stmt, 6);
    target.file_path = file_txt ? reinterpret_cast<const char*>(file_txt) : "";
    target.start_line = sqlite3_column_int(target_stmt, 7);
    target.end_line = sqlite3_column_int(target_stmt, 8);
    if (target.owner_name.empty()) target.owner_name = trailing_identifier(qualname_owner(target.qualname));
    if (target.owner_qualname.empty()) target.owner_qualname = qualname_owner(target.qualname);
    if (target.owner_name.empty() && target.file_id > 0 && target.start_line > 0) {
        auto* owner_stmt = cache.get("approx_callsite_lexical_owner",
            "SELECT name, qualname FROM nodes "
            "WHERE file_id = ? AND node_type = 'symbol' "
            "AND kind IN ('class','struct','interface','type_alias','type','enum') "
            "AND start_line <= ? AND end_line >= ? "
            "ORDER BY (end_line - start_line) ASC, start_line DESC LIMIT 1");
        sqlite3_bind_int64(owner_stmt, 1, target.file_id);
        sqlite3_bind_int(owner_stmt, 2, target.start_line);
        sqlite3_bind_int(owner_stmt, 3, target.end_line > 0 ? target.end_line : target.start_line);
        if (sqlite3_step(owner_stmt) == SQLITE_ROW) {
            auto* lexical_owner = sqlite3_column_text(owner_stmt, 0);
            auto* lexical_owner_qn = sqlite3_column_text(owner_stmt, 1);
            target.owner_name = lexical_owner ? reinterpret_cast<const char*>(lexical_owner) : "";
            target.owner_qualname = lexical_owner_qn ? reinterpret_cast<const char*>(lexical_owner_qn) : "";
        }
    }
    populate_target_param_shape(target, repo_root);

    auto like_dot = "%." + target.name;
    auto like_colon = "%::" + target.name;
    auto like_arrow = "%->" + target.name;
    int64_t query_limit = options.receiver_filter.empty()
        ? std::min<int64_t>(5000, std::max<int64_t>({limit * 4, limit, 200}))
        : 5000;

    auto* count_stmt = cache.get("approx_callsite_candidates_count",
        "SELECT COUNT(*) "
        "FROM refs r "
        "WHERE r.kind = 'call' AND (r.name = ? OR r.name LIKE ? OR r.name LIKE ? OR r.name LIKE ?)");
    sqlite3_bind_text(count_stmt, 1, target.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(count_stmt, 2, like_dot.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(count_stmt, 3, like_colon.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(count_stmt, 4, like_arrow.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(count_stmt) == SQLITE_ROW) result.total = sqlite3_column_int64(count_stmt, 0);

    auto* stmt = cache.get("approx_callsite_candidates",
        "SELECT r.id, r.file_id, r.name, f.path, r.start_line, r.start_col, r.end_line, r.end_col, "
        "r.evidence, r.containing_node_id, cn.name, cn.qualname, "
        "r.arg_count, r.arg_pattern, r.receiver_type_hint "
        "FROM refs r "
        "LEFT JOIN files f ON f.id = r.file_id "
        "LEFT JOIN nodes cn ON cn.id = r.containing_node_id "
        "WHERE r.kind = 'call' AND (r.name = ? OR r.name LIKE ? OR r.name LIKE ? OR r.name LIKE ?) "
        "ORDER BY f.path, r.start_line, r.start_col LIMIT ?");
    sqlite3_bind_text(stmt, 1, target.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, like_dot.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, like_colon.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, like_arrow.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, query_limit);

    std::vector<CallsiteCandidate> rows;
    std::unordered_set<std::string> seen;
    std::unordered_map<std::string, ReceiverTypeResolution> receiver_resolution_cache;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        CallsiteCandidate row;
        row.ref_id = sqlite3_column_int64(stmt, 0);
        row.file_id = sqlite3_column_int64(stmt, 1);
        auto* callee = sqlite3_column_text(stmt, 2);
        row.callee_text = callee ? reinterpret_cast<const char*>(callee) : "";
        auto* fp = sqlite3_column_text(stmt, 3);
        row.file_path = fp ? reinterpret_cast<const char*>(fp) : "";
        row.start_line = sqlite3_column_int(stmt, 4);
        row.start_col = sqlite3_column_int(stmt, 5);
        row.end_line = sqlite3_column_int(stmt, 6);
        row.end_col = sqlite3_column_int(stmt, 7);
        auto* evidence = sqlite3_column_text(stmt, 8);
        row.evidence = evidence ? reinterpret_cast<const char*>(evidence) : "";
        row.caller_node_id = sqlite3_column_int64(stmt, 9);
        auto* caller = sqlite3_column_text(stmt, 10);
        row.caller_name = caller ? reinterpret_cast<const char*>(caller) : "";
        auto* caller_qn = sqlite3_column_text(stmt, 11);
        row.caller_qualname = caller_qn ? reinterpret_cast<const char*>(caller_qn) : "";
        if (sqlite3_column_type(stmt, 12) != SQLITE_NULL) row.arg_count = sqlite3_column_int(stmt, 12);
        auto* arg_pattern = sqlite3_column_text(stmt, 13);
        row.arg_pattern = arg_pattern ? reinterpret_cast<const char*>(arg_pattern) : "";
        auto* receiver_type = sqlite3_column_text(stmt, 14);
        row.receiver_type_hint = receiver_type ? reinterpret_cast<const char*>(receiver_type) : "";
        row.receiver_hint = extract_receiver_hint(row.callee_text, target.name);

        std::string key = row.file_path + ":" + std::to_string(row.start_line) + ":" +
                          std::to_string(row.start_col) + ":" + row.callee_text;
        if (!seen.insert(key).second) continue;

        bool raw_receiver_type_matches_target =
            receiver_owner_match_strength(canonical_type_hint(row.receiver_type_hint), target) !=
                ReceiverMatchStrength::None;
        if (!row.receiver_type_hint.empty() &&
            !(raw_receiver_type_matches_target && !options.receiver_filter.empty())) {
            bool receiver_filter_can_match =
                options.receiver_filter.empty() ||
                receiver_text_matches_name(row.receiver_hint, options.receiver_filter) ||
                receiver_text_matches_name(row.receiver_type_hint, options.receiver_filter);
            if (receiver_filter_can_match) {
                std::string resolution_key = std::to_string(row.file_id) + "\x1f" +
                                             canonical_type_hint(row.receiver_type_hint);
                auto cached = receiver_resolution_cache.find(resolution_key);
                ReceiverTypeResolution resolved;
                if (cached != receiver_resolution_cache.end()) {
                    resolved = cached->second;
                } else {
                    resolved = resolve_receiver_type_hint(row, target, conn, cache);
                    receiver_resolution_cache.emplace(std::move(resolution_key), resolved);
                }
                if (resolved.resolved) {
                    row.resolved_receiver_type = resolved.resolved_type;
                    row.receiver_type_resolution_source = resolved.source;
                } else if (resolved.ambiguous) {
                    row.receiver_type_ambiguous = true;
                }
            }
        }
        score_candidate(row, target, row.caller_name, row.caller_qualname);
        if (!apply_arity_and_pattern_score(row, target)) {
            ++result.arity_filtered;
            continue;
        }
        rows.push_back(std::move(row));
    }

    std::sort(rows.begin(), rows.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.confidence != rhs.confidence) return lhs.confidence > rhs.confidence;
        if (lhs.file_path != rhs.file_path) return lhs.file_path < rhs.file_path;
        if (lhs.start_line != rhs.start_line) return lhs.start_line < rhs.start_line;
        return lhs.start_col < rhs.start_col;
    });

    std::vector<const CallsiteCandidate*> visible_rows;
    visible_rows.reserve(rows.size());
    for (const auto& row : rows) {
        if (!candidate_matches_receiver_filter(row, options.receiver_filter)) continue;
        visible_rows.push_back(&row);
        ++result.eligible_total;
        auto& arity_bucket = result.arity_buckets[row.arg_count];
        ++arity_bucket.count;
        arity_bucket.files[row.file_path.empty() ? "(unknown)" : row.file_path]++;
        result.heuristic_buckets[row.heuristic.empty() ? "(unknown)" : row.heuristic]++;
        result.file_buckets[row.file_path.empty() ? "(unknown)" : row.file_path]++;
    }

    size_t approx_bytes = 256;
    for (const auto* row_ptr : visible_rows) {
        const auto& row = *row_ptr;
        size_t item_bytes = estimate_callsite_candidate_bytes(row, options.include_handles);
        if (result.max_bytes > 0 && !result.rows.empty() &&
            approx_bytes + item_bytes > static_cast<size_t>(result.max_bytes)) {
            result.budget_exceeded = true;
            result.has_more = true;
            break;
        }
        if (static_cast<int64_t>(result.rows.size()) >= limit) {
            result.has_more = true;
            break;
        }
        result.rows.push_back(row);
        approx_bytes += item_bytes + 1;
    }
    if (result.total > static_cast<int64_t>(result.rows.size()))
        result.filtered_hidden = result.total - static_cast<int64_t>(result.rows.size());
    return result;
}

static yyjson_mut_val* emit_callsite_candidate(JsonMutDoc& doc, const CallsiteCandidate& row,
                                               bool include_handles) {
    auto* item = doc.new_obj();
    if (!row.caller_name.empty()) yyjson_mut_obj_add_strcpy(doc.doc, item, "caller_name", row.caller_name.c_str());
    if (!row.file_path.empty()) {
        yyjson_mut_obj_add_strcpy(doc.doc, item, include_handles ? "file_path" : "file", row.file_path.c_str());
    }
    yyjson_mut_obj_add_int(doc.doc, item, "start_line", row.start_line);
    if (row.start_col > 0) yyjson_mut_obj_add_int(doc.doc, item, "start_col", row.start_col);
    if (include_handles) {
        yyjson_mut_obj_add_int(doc.doc, item, "ref_id", row.ref_id);
        if (row.caller_node_id > 0) yyjson_mut_obj_add_int(doc.doc, item, "caller_node_id", row.caller_node_id);
        if (!row.caller_qualname.empty()) yyjson_mut_obj_add_strcpy(doc.doc, item, "caller_qualname", row.caller_qualname.c_str());
        auto* span = doc.new_obj();
        yyjson_mut_obj_add_int(doc.doc, span, "start_line", row.start_line);
        yyjson_mut_obj_add_int(doc.doc, span, "start_col", row.start_col);
        yyjson_mut_obj_add_int(doc.doc, span, "end_line", row.end_line);
        yyjson_mut_obj_add_int(doc.doc, span, "end_col", row.end_col);
        yyjson_mut_obj_add_val(doc.doc, item, "span", span);
    }
    yyjson_mut_obj_add_strcpy(doc.doc, item, "callee_text", row.callee_text.c_str());
    if (!row.receiver_hint.empty()) yyjson_mut_obj_add_strcpy(doc.doc, item, "receiver_hint", row.receiver_hint.c_str());
    if (!row.receiver_type_hint.empty()) yyjson_mut_obj_add_strcpy(doc.doc, item, "receiver_type_hint", row.receiver_type_hint.c_str());
    if (!row.resolved_receiver_type.empty()) {
        yyjson_mut_obj_add_strcpy(doc.doc, item, "resolved_receiver_type", row.resolved_receiver_type.c_str());
        yyjson_mut_obj_add_bool(doc.doc, item, "receiver_type_resolved", true);
        if (!row.receiver_type_resolution_source.empty())
            yyjson_mut_obj_add_strcpy(doc.doc, item, "receiver_type_resolution_source",
                                      row.receiver_type_resolution_source.c_str());
    }
    if (row.receiver_type_ambiguous)
        yyjson_mut_obj_add_bool(doc.doc, item, "receiver_type_ambiguous", true);
    yyjson_mut_obj_add_real(doc.doc, item, "confidence", row.confidence);
    yyjson_mut_obj_add_strcpy(doc.doc, item, "heuristic", row.heuristic.c_str());
    if (include_handles) {
        if (row.arg_count >= 0) yyjson_mut_obj_add_int(doc.doc, item, "arg_count", row.arg_count);
        if (!row.arg_pattern.empty()) yyjson_mut_obj_add_strcpy(doc.doc, item, "arg_pattern", row.arg_pattern.c_str());
        yyjson_mut_obj_add_strcpy(doc.doc, item, "why", row.why.c_str());
        if (!row.evidence.empty()) yyjson_mut_obj_add_strcpy(doc.doc, item, "evidence", row.evidence.c_str());
    }
    return item;
}

static yyjson_mut_val* emit_callsite_candidate_lean(JsonMutDoc& doc, const CallsiteCandidate& row) {
    auto* item = doc.new_obj();
    if (!row.file_path.empty()) yyjson_mut_obj_add_strcpy(doc.doc, item, "file", row.file_path.c_str());
    yyjson_mut_obj_add_int(doc.doc, item, "line", row.start_line);
    if (!row.caller_name.empty()) yyjson_mut_obj_add_strcpy(doc.doc, item, "caller", row.caller_name.c_str());
    yyjson_mut_obj_add_real(doc.doc, item, "confidence", row.confidence);
    if (row.arg_count >= 0) yyjson_mut_obj_add_int(doc.doc, item, "arg_count", row.arg_count);
    bool receiver_explains =
        row.heuristic.find("receiver") != std::string::npos ||
        row.heuristic.find("target_owner") != std::string::npos;
    if (receiver_explains && !row.receiver_hint.empty())
        yyjson_mut_obj_add_strcpy(doc.doc, item, "receiver", row.receiver_hint.c_str());
    if (receiver_explains && !row.receiver_type_hint.empty())
        yyjson_mut_obj_add_strcpy(doc.doc, item, "receiver_type", row.receiver_type_hint.c_str());
    if (!row.resolved_receiver_type.empty()) {
        yyjson_mut_obj_add_strcpy(doc.doc, item, "resolved_receiver_type", row.resolved_receiver_type.c_str());
        yyjson_mut_obj_add_bool(doc.doc, item, "receiver_type_resolved", true);
    }
    yyjson_mut_obj_add_strcpy(doc.doc, item, "heuristic", row.heuristic.c_str());
    return item;
}

static std::vector<std::pair<std::string, int64_t>> top_counts(
        const std::map<std::string, int64_t>& counts,
        size_t limit) {
    std::vector<std::pair<std::string, int64_t>> items(counts.begin(), counts.end());
    std::sort(items.begin(), items.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.second != rhs.second) return lhs.second > rhs.second;
        return lhs.first < rhs.first;
    });
    if (items.size() > limit) items.resize(limit);
    return items;
}

static yyjson_mut_val* emit_count_array(JsonMutDoc& doc,
                                        const std::map<std::string, int64_t>& counts,
                                        size_t limit) {
    auto* arr = doc.new_arr();
    for (const auto& [name, count] : top_counts(counts, limit)) {
        auto* item = doc.new_obj();
        yyjson_mut_obj_add_strcpy(doc.doc, item, "name", name.c_str());
        yyjson_mut_obj_add_int(doc.doc, item, "count", count);
        yyjson_mut_arr_append(arr, item);
    }
    return arr;
}

static void add_callsite_candidate_buckets(JsonMutDoc& doc, yyjson_mut_val* root,
                                           const CallsiteCandidateSet& candidates) {
    auto* buckets = doc.new_obj();
    auto* by_arg_count = doc.new_obj();
    for (const auto& [arity, bucket] : candidates.arity_buckets) {
        auto* item = doc.new_obj();
        yyjson_mut_obj_add_int(doc.doc, item, "count", bucket.count);
        yyjson_mut_obj_add_val(doc.doc, item, "top_files",
            emit_count_array(doc, bucket.files, 3));
        std::string key = arity < 0 ? "unknown" : std::to_string(arity);
        yyjson_mut_obj_add_val(doc.doc, by_arg_count, key.c_str(), item);
    }
    yyjson_mut_obj_add_val(doc.doc, buckets, "arg_count", by_arg_count);
    yyjson_mut_obj_add_val(doc.doc, buckets, "heuristic",
        emit_count_array(doc, candidates.heuristic_buckets, 8));
    yyjson_mut_obj_add_val(doc.doc, buckets, "file",
        emit_count_array(doc, candidates.file_buckets, 8));
    yyjson_mut_obj_add_val(doc.doc, root, "candidate_buckets", buckets);
}

static void add_callsite_candidate_metadata(JsonMutDoc& doc, yyjson_mut_val* root,
                                            const char* prefix,
                                            const CallsiteCandidateSet& candidates,
                                            const CallsiteCandidateOptions& options) {
    std::string p(prefix ? prefix : "candidate");
    if (p == "candidate") {
        yyjson_mut_obj_add_int(doc.doc, root, "candidate_total", candidates.total);
        yyjson_mut_obj_add_int(doc.doc, root, "candidate_eligible", candidates.eligible_total);
        yyjson_mut_obj_add_int(doc.doc, root, "candidate_filtered_hidden", candidates.filtered_hidden);
        yyjson_mut_obj_add_int(doc.doc, root, "candidate_arity_filtered", candidates.arity_filtered);
        yyjson_mut_obj_add_int(doc.doc, root, "candidate_max_bytes", candidates.max_bytes);
        if (candidates.has_more) yyjson_mut_obj_add_bool(doc.doc, root, "candidate_has_more", true);
        if (candidates.budget_exceeded) yyjson_mut_obj_add_bool(doc.doc, root, "candidate_budget_exceeded", true);
        if (!options.receiver_filter.empty())
            yyjson_mut_obj_add_strcpy(doc.doc, root, "candidate_receiver", options.receiver_filter.c_str());
        yyjson_mut_obj_add_int(doc.doc, root, "total", candidates.total);
        yyjson_mut_obj_add_int(doc.doc, root, "total_candidates", candidates.total);
        yyjson_mut_obj_add_int(doc.doc, root, "eligible_candidates", candidates.eligible_total);
        yyjson_mut_obj_add_int(doc.doc, root, "filtered_hidden", candidates.filtered_hidden);
        yyjson_mut_obj_add_int(doc.doc, root, "arity_filtered", candidates.arity_filtered);
        yyjson_mut_obj_add_int(doc.doc, root, "max_bytes", candidates.max_bytes);
        if (candidates.budget_exceeded) yyjson_mut_obj_add_bool(doc.doc, root, "budget_exceeded", true);
    } else if (p == "candidate_callers") {
        yyjson_mut_obj_add_int(doc.doc, root, "candidate_callers_total", candidates.total);
        yyjson_mut_obj_add_int(doc.doc, root, "candidate_callers_eligible", candidates.eligible_total);
        yyjson_mut_obj_add_int(doc.doc, root, "candidate_callers_filtered_hidden", candidates.filtered_hidden);
        yyjson_mut_obj_add_int(doc.doc, root, "candidate_callers_arity_filtered", candidates.arity_filtered);
        yyjson_mut_obj_add_int(doc.doc, root, "candidate_callers_max_bytes", candidates.max_bytes);
        if (candidates.has_more) yyjson_mut_obj_add_bool(doc.doc, root, "candidate_callers_has_more", true);
        if (candidates.budget_exceeded) yyjson_mut_obj_add_bool(doc.doc, root, "candidate_callers_budget_exceeded", true);
        if (!options.receiver_filter.empty())
            yyjson_mut_obj_add_strcpy(doc.doc, root, "candidate_callers_receiver", options.receiver_filter.c_str());
    } else if (p == "candidate_impacted") {
        yyjson_mut_obj_add_int(doc.doc, root, "candidate_impacted_total", candidates.total);
        yyjson_mut_obj_add_int(doc.doc, root, "candidate_impacted_eligible", candidates.eligible_total);
        yyjson_mut_obj_add_int(doc.doc, root, "candidate_impacted_filtered_hidden", candidates.filtered_hidden);
        yyjson_mut_obj_add_int(doc.doc, root, "candidate_impacted_arity_filtered", candidates.arity_filtered);
        yyjson_mut_obj_add_int(doc.doc, root, "candidate_impacted_max_bytes", candidates.max_bytes);
        if (candidates.has_more) yyjson_mut_obj_add_bool(doc.doc, root, "candidate_impacted_has_more", true);
        if (candidates.budget_exceeded) yyjson_mut_obj_add_bool(doc.doc, root, "candidate_impacted_budget_exceeded", true);
        if (!options.receiver_filter.empty())
            yyjson_mut_obj_add_strcpy(doc.doc, root, "candidate_impacted_receiver", options.receiver_filter.c_str());
    }
}

// Resolves a user-supplied path to the canonical absolute path as stored in the DB.
// - If already absolute: verifies it exists in files, returns it as-is.
// - If relative: tries exact match of (repo_root + "/" + path), then suffix LIKE '%/' || path.
// Returns empty string if not found.
static std::string resolve_db_path(sqlite3* db, const std::string& path, const std::string& repo_root) {
    if (std::filesystem::path(path).is_absolute()) {
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db, "SELECT 1 FROM files WHERE path = ? LIMIT 1", -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
        bool found = (sqlite3_step(stmt) == SQLITE_ROW);
        sqlite3_finalize(stmt);
        return found ? path : std::string();
    }

    // Try exact match as provided (single-root indexes store repo-relative paths)
    {
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db, "SELECT 1 FROM files WHERE path = ? LIMIT 1", -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
        bool found = (sqlite3_step(stmt) == SQLITE_ROW);
        sqlite3_finalize(stmt);
        if (found) return path;
    }

    // Try exact match: repo_root/path
    std::string abs = repo_root;
    if (!abs.empty() && abs.back() != '/') abs += '/';
    abs += path;
    {
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db, "SELECT 1 FROM files WHERE path = ? LIMIT 1", -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, abs.c_str(), -1, SQLITE_TRANSIENT);
        bool found = (sqlite3_step(stmt) == SQLITE_ROW);
        sqlite3_finalize(stmt);
        if (found) return abs;
    }

    // Suffix match: any stored path ending with '/' + path
    std::string like_pat = "%/" + path;
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "SELECT path FROM files WHERE path LIKE ? LIMIT 1", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, like_pat.c_str(), -1, SQLITE_TRANSIENT);
    std::string result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* txt = sqlite3_column_text(stmt, 0);
        if (txt) result = reinterpret_cast<const char*>(txt);
    }
    sqlite3_finalize(stmt);
    return result;
}

// Resolve a directory-or-file path to the canonical absolute path prefix as stored in the DB.
// - If already absolute: accepts an exact file path, a directory prefix with indexed files,
//   or a registered workspace root.
// - If relative: resolves against repo_root.
// Returns empty string if not found.
static std::string resolve_db_dir_path(sqlite3* db, const std::string& path, const std::string& repo_root) {
    std::string input = path;
    if (input.empty() || input == "." || input == "./") return std::string();

    auto exists_as_prefix = [&](const std::string& candidate) -> bool {
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db,
            "SELECT 1 FROM files WHERE path = ? OR path GLOB ? LIMIT 1",
            -1, &stmt, nullptr);
        std::string glob = candidate;
        if (!glob.empty() && glob.back() != '/') glob += '/';
        glob += '*';
        sqlite3_bind_text(stmt, 1, candidate.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, glob.c_str(), -1, SQLITE_TRANSIENT);
        bool found = (sqlite3_step(stmt) == SQLITE_ROW);
        sqlite3_finalize(stmt);
        if (found) return true;

        if (sqlite3_prepare_v2(db, "SELECT 1 FROM roots WHERE path = ? LIMIT 1", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, candidate.c_str(), -1, SQLITE_TRANSIENT);
            found = (sqlite3_step(stmt) == SQLITE_ROW);
            sqlite3_finalize(stmt);
        }
        return found;
    };

    if (std::filesystem::path(input).is_absolute()) {
        if (exists_as_prefix(input)) return input;
        std::error_code ec;
        auto rel = std::filesystem::relative(std::filesystem::path(input), std::filesystem::path(repo_root), ec);
        if (!ec) {
            std::string rel_str = rel.generic_string();
            if (exists_as_prefix(rel_str)) return rel_str;
        }
        return std::string();
    }

    if (input.find("..") != std::string::npos) return std::string();

    std::error_code ec;
    auto canonical = std::filesystem::canonical(std::filesystem::path(repo_root) / input, ec);
    if (ec) return std::string();
    auto canonical_root = std::filesystem::canonical(std::filesystem::path(repo_root), ec);
    if (ec) return std::string();
    auto rel = std::filesystem::relative(canonical, canonical_root, ec);
    if (ec) return std::string();
    std::string validated = rel.generic_string();
    if (validated.starts_with("..")) return std::string();

    if (exists_as_prefix(validated)) return validated;

    std::string abs = repo_root;
    if (!abs.empty() && abs.back() != '/') abs += '/';
    abs += validated;
    return exists_as_prefix(abs) ? abs : std::string();
}

static bool canonical_paths_equal(const std::string& lhs, const std::string& rhs) {
    std::error_code ec;
    auto lhs_canon = std::filesystem::weakly_canonical(std::filesystem::path(lhs), ec);
    if (ec) return false;
    auto rhs_canon = std::filesystem::weakly_canonical(std::filesystem::path(rhs), ec);
    if (ec) return false;
    return lhs_canon == rhs_canon;
}

static bool path_has_db_children(sqlite3* db, const std::string& path) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM files WHERE path GLOB ? LIMIT 1", -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    std::string glob = path;
    if (!glob.empty() && glob.back() != '/') glob += '/';
    glob += '*';
    sqlite3_bind_text(stmt, 1, glob.c_str(), -1, SQLITE_TRANSIENT);
    bool found = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return found;
}

static std::vector<std::string> split_path_components(const std::string& path) {
    std::vector<std::string> parts;
    std::stringstream ss(path);
    std::string part;
    while (std::getline(ss, part, '/')) {
        if (!part.empty() && part != ".") parts.push_back(part);
    }
    return parts;
}

static std::string path_basename_or_default(const std::string& path, const std::string& fallback) {
    auto filename = std::filesystem::path(path).filename().string();
    return filename.empty() ? fallback : filename;
}

struct DirTreeNode {
    std::string name;
    bool is_file = false;
    std::string language;
    int64_t size_bytes = 0;
    bool truncated = false;
    int64_t children_count = 0;
    int64_t descendant_file_count = 0;
    std::map<std::string, std::unique_ptr<DirTreeNode>> children;
};

static DirTreeNode* ensure_dir_child(DirTreeNode& parent, const std::string& name) {
    auto it = parent.children.find(name);
    if (it == parent.children.end()) {
        auto child = std::make_unique<DirTreeNode>();
        child->name = name;
        auto [inserted, _] = parent.children.emplace(name, std::move(child));
        it = inserted;
    }
    return it->second.get();
}

static yyjson_mut_val* serialize_dir_tree_node(JsonMutDoc& doc, const DirTreeNode& node) {
    auto* json_node = doc.new_obj();
    yyjson_mut_obj_add_strcpy(doc.doc, json_node, "name", node.name.c_str());
    yyjson_mut_obj_add_str(doc.doc, json_node, "type", node.is_file ? "file" : "dir");
    if (node.is_file) {
        yyjson_mut_obj_add_strcpy(doc.doc, json_node, "language", node.language.c_str());
        yyjson_mut_obj_add_int(doc.doc, json_node, "size_bytes", node.size_bytes);
        return json_node;
    }

    if (node.truncated) {
        yyjson_mut_obj_add_bool(doc.doc, json_node, "truncated", true);
        yyjson_mut_obj_add_int(doc.doc, json_node, "children_count", node.children_count);
        return json_node;
    }

    auto* children = doc.new_arr();
    for (const auto& [_, child] : node.children) {
        yyjson_mut_arr_append(children, serialize_dir_tree_node(doc, *child));
    }
    yyjson_mut_obj_add_val(doc.doc, json_node, "children", children);
    return json_node;
}

static int64_t count_visible_dir_tree_files(const DirTreeNode& node) {
    if (node.is_file) return 1;
    if (node.truncated) return 0;

    int64_t visible = 0;
    for (const auto& [_, child] : node.children) {
        visible += count_visible_dir_tree_files(*child);
    }
    return visible;
}

static void collect_truncatable_dir_nodes(DirTreeNode& node, int depth,
                                          std::vector<std::pair<int, DirTreeNode*>>& out) {
    if (node.is_file || node.truncated) return;
    if (depth > 0) out.emplace_back(depth, &node);
    for (auto& [_, child] : node.children) {
        collect_truncatable_dir_nodes(*child, depth + 1, out);
    }
}

static void apply_dir_tree_file_cap(DirTreeNode& root, int64_t max_files) {
    if (max_files < 0) return;

    int64_t visible_files = count_visible_dir_tree_files(root);
    if (visible_files <= max_files) return;

    std::vector<std::pair<int, DirTreeNode*>> candidates;
    collect_truncatable_dir_nodes(root, 0, candidates);
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& lhs, const auto& rhs) {
                  return lhs.first > rhs.first;
              });

    for (const auto& [_, node] : candidates) {
        if (visible_files <= max_files) break;
        if (!node || node->truncated) continue;

        int64_t subtree_visible = count_visible_dir_tree_files(*node);
        if (subtree_visible <= 0) continue;

        node->truncated = true;
        node->children_count = node->descendant_file_count;
        node->children.clear();
        visible_files -= subtree_visible;
    }
}

// T060: server_info
std::string server_info(yyjson_val* /*params*/, Connection& conn,
                                QueryCache& /*cache*/, const std::string& repo_root) {
    auto version = schema::get_kv(conn, "schema_version", "0");
    auto idx_version = schema::get_kv(conn, "indexer_version", "unknown");
    auto last_index = schema::get_kv(conn, "last_index_time", "");

    // Skip integrity/quick_check in MCP hot path — too slow on large DBs (5GB+).
    // A basic connectivity test via the get_kv calls above is sufficient.
    std::string db_status = version.empty() ? "error" : "ok";

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);
    yyjson_mut_obj_add_str(doc.doc, root, "protocol_version", "codetopo-mcp/1.0");
    yyjson_mut_obj_add_int(doc.doc, root, "schema_version", std::atoi(version.c_str()));
    yyjson_mut_obj_add_strcpy(doc.doc, root, "indexer_version", idx_version.c_str());

    auto* caps = doc.new_arr();
    yyjson_mut_arr_add_str(doc.doc, caps, "fts5");
    yyjson_mut_arr_add_str(doc.doc, caps, "source_snippets");
    yyjson_mut_arr_add_str(doc.doc, caps, "context_for");
    yyjson_mut_arr_add_str(doc.doc, caps, "impact_of");
    yyjson_mut_arr_add_str(doc.doc, caps, "approx_callgraph_candidates");
    yyjson_mut_arr_add_str(doc.doc, caps, "code_search");
    yyjson_mut_obj_add_val(doc.doc, root, "capabilities", caps);

    yyjson_mut_obj_add_strcpy(doc.doc, root, "repo_root", repo_root.c_str());
    yyjson_mut_obj_add_strcpy(doc.doc, root, "db_status", db_status.c_str());

    // R5: Freshness metadata — indexed vs current git state
    auto indexed_branch = schema::get_kv(conn, "git_branch", "");
    auto indexed_commit = schema::get_kv(conn, "git_head", "");
    auto current_branch = get_git_branch(repo_root);
    auto current_commit = get_git_head(repo_root);
    bool stale = !indexed_commit.empty() && indexed_commit != current_commit;

    yyjson_mut_obj_add_strcpy(doc.doc, root, "indexed_branch", indexed_branch.c_str());
    yyjson_mut_obj_add_strcpy(doc.doc, root, "indexed_commit", indexed_commit.c_str());
    yyjson_mut_obj_add_strcpy(doc.doc, root, "current_branch", current_branch.c_str());
    yyjson_mut_obj_add_strcpy(doc.doc, root, "current_commit", current_commit.c_str());
    yyjson_mut_obj_add_bool(doc.doc, root, "stale", stale);

    // Compute index_age_seconds from last_index_time
    int64_t index_age_seconds = -1;
    if (!last_index.empty()) {
        struct tm tm_parsed = {};
        // Cross-platform ISO 8601 parse (strptime unavailable on MSVC)
        if (sscanf(last_index.c_str(), "%d-%d-%dT%d:%d:%d",
                   &tm_parsed.tm_year, &tm_parsed.tm_mon, &tm_parsed.tm_mday,
                   &tm_parsed.tm_hour, &tm_parsed.tm_min, &tm_parsed.tm_sec) == 6) {
            tm_parsed.tm_year -= 1900;
            tm_parsed.tm_mon -= 1;
            tm_parsed.tm_isdst = -1;
            time_t indexed_time = mktime(&tm_parsed);
            if (indexed_time != -1) {
                time_t now = time(nullptr);
                index_age_seconds = static_cast<int64_t>(difftime(now, indexed_time));
            }
        }
    }
    yyjson_mut_obj_add_int(doc.doc, root, "index_age_seconds", index_age_seconds);

    return doc.to_string();
}

// T061: repo_stats
std::string repo_stats(yyjson_val* /*params*/, Connection& conn,
                               QueryCache& cache, const std::string& /*repo_root*/) {
    auto count_query = [&](const char* table) -> int64_t {
        std::string sql = std::string("SELECT COUNT(*) FROM ") + table;
        auto* stmt = cache.get(std::string("count_") + table, sql);
        int64_t n = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int64(stmt, 0);
        return n;
    };

    auto last_index = schema::get_kv(conn, "last_index_time", "");
    auto idx_version = schema::get_kv(conn, "indexer_version", "");

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);
    yyjson_mut_obj_add_int(doc.doc, root, "file_count", count_query("files"));
    yyjson_mut_obj_add_int(doc.doc, root, "symbol_count", count_query("nodes"));
    yyjson_mut_obj_add_int(doc.doc, root, "edge_count", count_query("edges"));
    yyjson_mut_obj_add_strcpy(doc.doc, root, "last_index_time", last_index.c_str());
    yyjson_mut_obj_add_strcpy(doc.doc, root, "indexer_version", idx_version.c_str());

    return doc.to_string();
}

// T080: file_search — search file paths by GLOB pattern
std::string file_search(yyjson_val* params, Connection& conn,
                                QueryCache& cache, const std::string& /*repo_root*/) {
    const char* pattern = params ? json_get_str(params, "pattern") : nullptr;
    if (!pattern) return McpError::invalid_input("Missing 'pattern' parameter").to_json_rpc(0);

    const char* language = params ? json_get_str(params, "language") : nullptr;
    int64_t limit = params ? json_get_int(params, "limit", 50) : 50;
    int64_t offset = params ? json_get_int(params, "offset", 0) : 0;
    if (limit > 500) limit = 500;

    // Pattern analysis: absolute paths use as-is; patterns with no '/' also match basename
    bool is_absolute_pat = pattern[0] == '/';
    bool has_slash = strchr(pattern, '/') != nullptr;
    bool add_basename_or = !has_slash && !is_absolute_pat;

    std::string where_sql;
    if (add_basename_or) {
        where_sql = " WHERE (path GLOB ? OR path GLOB '*/' || ?)";
    } else {
        where_sql = " WHERE path GLOB ?";
    }
    std::string cache_key = "file_search";
    if (add_basename_or) cache_key += "_bn";
    if (language && strlen(language) > 0) {
        where_sql += " AND language = ?";
        cache_key += "_lang";
    }

    std::string sql =
        "SELECT id, path, language, size_bytes FROM files" + where_sql + " ORDER BY path LIMIT ? OFFSET ?";
    std::string count_sql = "SELECT COUNT(*) FROM files" + where_sql;

    auto* stmt = cache.get(cache_key, sql);
    auto* count_stmt = cache.get(cache_key + "_count", count_sql);
    int bind_idx = 1;
    int count_bind_idx = 1;
    sqlite3_bind_text(stmt, bind_idx++, pattern, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(count_stmt, count_bind_idx++, pattern, -1, SQLITE_TRANSIENT);
    if (add_basename_or) {
        sqlite3_bind_text(stmt, bind_idx++, pattern, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(count_stmt, count_bind_idx++, pattern, -1, SQLITE_TRANSIENT);
    }
    if (language && strlen(language) > 0) {
        sqlite3_bind_text(stmt, bind_idx++, language, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(count_stmt, count_bind_idx++, language, -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int64(stmt, bind_idx++, limit + 1);
    sqlite3_bind_int64(stmt, bind_idx++, offset);

    int64_t total = 0;
    if (sqlite3_step(count_stmt) == SQLITE_ROW) total = sqlite3_column_int64(count_stmt, 0);

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);
    auto* results = doc.new_arr();

    int count = 0;
    bool has_more = false;
    while (sqlite3_step(stmt) == SQLITE_ROW && count <= limit) {
        if (count == limit) { has_more = true; break; }
        count++;

        auto* item = doc.new_obj();
        yyjson_mut_obj_add_int(doc.doc, item, "file_id", sqlite3_column_int64(stmt, 0));
        yyjson_mut_obj_add_strcpy(doc.doc, item, "path",
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
        yyjson_mut_obj_add_strcpy(doc.doc, item, "language",
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
        yyjson_mut_obj_add_int(doc.doc, item, "size_bytes", sqlite3_column_int64(stmt, 3));
        yyjson_mut_arr_append(results, item);
    }

    add_pagination_fields(doc, root, results, total, has_more, offset, limit);
    return doc.to_string();
}

// T081: dir_list — list files in a directory (one level)
std::string dir_list(yyjson_val* params, Connection& conn,
                             QueryCache& cache, const std::string& /*repo_root*/) {
    const char* dir_path = params ? json_get_str(params, "path") : nullptr;
    if (!dir_path) return McpError::invalid_input("Missing 'path' parameter").to_json_rpc(0);

    int64_t limit = params ? json_get_int(params, "limit", 200) : 200;
    int64_t offset = params ? json_get_int(params, "offset", 0) : 0;
    if (limit > 2000) limit = 2000;

    // Normalize: ensure trailing /
    std::string dir = dir_path;
    if (!dir.empty() && dir.back() != '/') dir += '/';

    // Match files directly under this directory (not in subdirectories)
    // GLOB pattern: dir/* matches one level; exclude dir/*/* to avoid deeper
    std::string glob_one = dir + "*";
    std::string glob_deep = dir + "*/*";

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);
    yyjson_mut_obj_add_strcpy(doc.doc, root, "directory", dir_path);
    auto* files_arr = doc.new_arr();
    auto* dirs_arr = doc.new_arr();
    auto* results = doc.new_arr();

    int64_t total = 0;
    bool has_more = false;
    std::string entry_cte =
        "WITH entries AS ("
        "SELECT 'directory' AS entry_type, "
        "substr(path, 1, instr(substr(path, length(?) + 1), '/') + length(?)) AS entry_path, "
        "'' AS language, 0 AS size_bytes "
        "FROM files WHERE path GLOB ? AND path GLOB ? "
        "GROUP BY entry_path "
        "UNION ALL "
        "SELECT 'file' AS entry_type, path AS entry_path, language, size_bytes "
        "FROM files WHERE path GLOB ? AND path NOT GLOB ?"
        ")";

    auto* count_stmt = cache.get("dir_list_count",
        entry_cte + " SELECT COUNT(*) FROM entries");
    sqlite3_bind_text(count_stmt, 1, dir.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(count_stmt, 2, dir.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(count_stmt, 3, glob_one.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(count_stmt, 4, glob_deep.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(count_stmt, 5, glob_one.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(count_stmt, 6, glob_deep.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(count_stmt) == SQLITE_ROW) total = sqlite3_column_int64(count_stmt, 0);

    auto* stmt = cache.get("dir_list",
        entry_cte + " SELECT entry_type, entry_path, language, size_bytes "
        "FROM entries ORDER BY entry_path LIMIT ? OFFSET ?");
    sqlite3_bind_text(stmt, 1, dir.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, dir.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, glob_one.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, glob_deep.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, glob_one.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, glob_deep.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 7, limit + 1);
    sqlite3_bind_int64(stmt, 8, offset);

    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && count <= limit) {
        if (count == limit) { has_more = true; break; }
        count++;

        const char* entry_type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* entry_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (!entry_type || !entry_path) continue;

        auto* item = doc.new_obj();
        yyjson_mut_obj_add_strcpy(doc.doc, item, "type", entry_type);
        yyjson_mut_obj_add_strcpy(doc.doc, item, "path", entry_path);

        if (strcmp(entry_type, "file") == 0) {
            auto* file_item = doc.new_obj();
            yyjson_mut_obj_add_strcpy(doc.doc, file_item, "path", entry_path);
            yyjson_mut_obj_add_strcpy(doc.doc, file_item, "language",
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
            yyjson_mut_obj_add_int(doc.doc, file_item, "size_bytes", sqlite3_column_int64(stmt, 3));
            yyjson_mut_obj_add_strcpy(doc.doc, item, "language",
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
            yyjson_mut_obj_add_int(doc.doc, item, "size_bytes", sqlite3_column_int64(stmt, 3));
            yyjson_mut_arr_append(files_arr, file_item);
        } else {
            yyjson_mut_arr_add_strcpy(doc.doc, dirs_arr, entry_path);
        }

        yyjson_mut_arr_append(results, item);
    }

    // Workspace mode fallback: if "." returned no results, list roots table as subdirectories.
    // In workspace mode, file paths are absolute so "." GLOB patterns won't match anything.
    if (total == 0 && (strcmp(dir_path, ".") == 0 || strcmp(dir_path, "./") == 0)) {
        sqlite3_stmt* roots_count_stmt = nullptr;
        if (sqlite3_prepare_v2(conn.raw(), "SELECT COUNT(*) FROM roots", -1, &roots_count_stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(roots_count_stmt) == SQLITE_ROW) total = sqlite3_column_int64(roots_count_stmt, 0);
            sqlite3_finalize(roots_count_stmt);
        }

        sqlite3_stmt* roots_stmt = nullptr;
        if (sqlite3_prepare_v2(conn.raw(),
                "SELECT path FROM roots ORDER BY path LIMIT ? OFFSET ?", -1, &roots_stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(roots_stmt, 1, limit + 1);
            sqlite3_bind_int64(roots_stmt, 2, offset);

            int root_count = 0;
            while (sqlite3_step(roots_stmt) == SQLITE_ROW && root_count <= limit) {
                if (root_count == limit) { has_more = true; break; }
                root_count++;

                const char* root_path = reinterpret_cast<const char*>(sqlite3_column_text(roots_stmt, 0));
                if (!root_path) continue;
                yyjson_mut_arr_add_strcpy(doc.doc, dirs_arr, root_path);

                auto* item = doc.new_obj();
                yyjson_mut_obj_add_str(doc.doc, item, "type", "directory");
                yyjson_mut_obj_add_strcpy(doc.doc, item, "path", root_path);
                yyjson_mut_arr_append(results, item);
            }
            sqlite3_finalize(roots_stmt);
        }
    }

    yyjson_mut_obj_add_val(doc.doc, root, "files", files_arr);
    yyjson_mut_obj_add_val(doc.doc, root, "subdirectories", dirs_arr);
    add_pagination_fields(doc, root, results, total, has_more, offset, limit);
    return doc.to_string();
}

// T081b: dir_tree — return a directory subtree up to a given depth
std::string dir_tree(yyjson_val* params, Connection& conn,
                     QueryCache& cache, const std::string& repo_root) {
    const char* requested_path = params ? json_get_str(params, "path") : nullptr;
    if (!requested_path) requested_path = ".";

    int64_t depth = params ? json_get_int(params, "depth", 1) : 1;
    int64_t max_files = params ? json_get_int(params, "max_files", 500) : 500;
    if (depth < 0) {
        return McpError::invalid_input("Invalid 'depth': must be >= 0").to_json_rpc(0);
    }
    if (max_files <= 0) {
        return McpError::invalid_input("Invalid 'max_files': must be > 0").to_json_rpc(0);
    }
    int64_t max_depth = (depth == 0) ? 20 : std::min<int64_t>(depth, 20);

    bool all_paths = (strcmp(requested_path, ".") == 0 || strcmp(requested_path, "./") == 0);
    if (!all_paths && std::filesystem::path(requested_path).is_absolute() &&
        canonical_paths_equal(requested_path, repo_root)) {
        all_paths = true;
    }

    std::string resolved_dir;
    if (!all_paths) {
        resolved_dir = resolve_db_dir_path(conn.raw(), requested_path, repo_root);
        if (resolved_dir.empty()) {
            if (!std::filesystem::path(requested_path).is_absolute()) {
                auto validated = path_util::validate_mcp_path(requested_path, repo_root);
                if (validated.empty()) {
                    return McpError::invalid_input("Invalid path: rejected by traversal guard").to_json_rpc(0);
                }
            }
            return McpError::not_found(std::string("Path not found: ") + requested_path).to_json_rpc(0);
        }
        if (!path_has_db_children(conn.raw(), resolved_dir)) {
            return McpError::invalid_input(std::string("Path is not a directory: ") + requested_path).to_json_rpc(0);
        }
    }

    std::string count_sql = "SELECT COUNT(*) FROM files";
    std::string select_sql = "SELECT path, language, size_bytes FROM files";
    std::string path_glob;
    if (!all_paths) {
        std::string prefix = resolved_dir;
        if (!prefix.empty() && prefix.back() != '/') prefix += '/';
        path_glob = prefix + "*";
        count_sql += " WHERE path GLOB ?";
        select_sql += " WHERE path GLOB ?";
    }
    select_sql += " ORDER BY path";

    std::string cache_key = all_paths ? "dir_tree_all" : "dir_tree_dir";
    auto* count_stmt = cache.get(cache_key + "_count", count_sql);
    auto* stmt = cache.get(cache_key, select_sql);
    if (!all_paths) {
        sqlite3_bind_text(count_stmt, 1, path_glob.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 1, path_glob.c_str(), -1, SQLITE_TRANSIENT);
    }

    int64_t total = 0;
    if (sqlite3_step(count_stmt) == SQLITE_ROW) total = sqlite3_column_int64(count_stmt, 0);

    std::string root_name;
    if (all_paths) {
        root_name = (std::filesystem::path(requested_path).is_absolute() &&
                     canonical_paths_equal(requested_path, repo_root))
            ? path_basename_or_default(repo_root, ".")
            : ".";
    } else {
        root_name = path_basename_or_default(resolved_dir, resolved_dir);
    }

    DirTreeNode tree_root;
    tree_root.name = root_name;

    std::string relative_prefix;
    if (!all_paths) {
        relative_prefix = resolved_dir;
        if (!relative_prefix.empty() && relative_prefix.back() != '/') relative_prefix += '/';
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* file_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* language = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (!file_path || !language) continue;

        std::string relative_path = all_paths ? std::string(file_path) : std::string(file_path).substr(relative_prefix.size());
        auto parts = split_path_components(relative_path);
        if (parts.empty()) continue;

        DirTreeNode* current = &tree_root;
        current->descendant_file_count++;
        bool inserted = false;
        for (size_t i = 0; i + 1 < parts.size(); ++i) {
            int64_t dir_depth = static_cast<int64_t>(i) + 1;
            auto* child = ensure_dir_child(*current, parts[i]);
            child->descendant_file_count++;
            if (dir_depth == max_depth) {
                child->truncated = true;
                child->children_count = child->descendant_file_count;
                inserted = true;
                break;
            }
            current = child;
        }
        if (inserted) continue;

        int64_t file_depth = static_cast<int64_t>(parts.size());
        if (file_depth > max_depth) continue;

        auto file_node = std::make_unique<DirTreeNode>();
        file_node->name = parts.back();
        file_node->is_file = true;
        file_node->language = language;
        file_node->size_bytes = sqlite3_column_int64(stmt, 2);
        current->children[file_node->name] = std::move(file_node);
    }

    apply_dir_tree_file_cap(tree_root, max_files);
    int64_t visible_file_count = count_visible_dir_tree_files(tree_root);
    bool has_more = total > visible_file_count;

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);
    yyjson_mut_obj_add_strcpy(doc.doc, root, "path", requested_path);
    yyjson_mut_obj_add_int(doc.doc, root, "depth", depth == 0 ? 0 : max_depth);
    yyjson_mut_obj_add_val(doc.doc, root, "tree", serialize_dir_tree_node(doc, tree_root));
    yyjson_mut_obj_add_int(doc.doc, root, "file_count", total);
    yyjson_mut_obj_add_bool(doc.doc, root, "has_more", has_more);
    return doc.to_string();
}

// T062: symbol_search
std::string symbol_search(yyjson_val* params, Connection& conn,
                                   QueryCache& cache, const std::string& /*repo_root*/) {
    const char* query = params ? json_get_str(params, "query") : nullptr;
    if (!query) return McpError::invalid_input("Missing 'query' parameter").to_json_rpc(0);

    const char* kind = params ? json_get_str(params, "kind") : nullptr;
    int64_t limit = params ? json_get_int(params, "limit", 50) : 50;
    int64_t offset = params ? json_get_int(params, "offset", 0) : 0;
    bool include_source = params ? json_get_bool(params, "include_source", false) : false;
    (void)include_source;

    if (limit > 500) limit = 500;

    // When query is "*" or empty, skip FTS and query nodes table directly
    bool wildcard = (strcmp(query, "*") == 0 || strlen(query) == 0);

    sqlite3_stmt* stmt = nullptr;
    sqlite3_stmt* count_stmt = nullptr;
    int bind_idx = 1;
    int count_bind_idx = 1;

    if (wildcard) {
        if (kind && strlen(kind) > 0) {
            bool fn_kind = (std::string(kind) == "function");
            stmt = fn_kind
                ? cache.get("symbol_search_wildcard_fn",
                    "SELECT n.id, n.kind, n.name, n.qualname, f.path, n.start_line, n.end_line "
                    "FROM nodes n LEFT JOIN files f ON n.file_id = f.id "
                    "WHERE n.kind IN ('function', 'method') "
                    "ORDER BY f.path, n.start_line, n.id "
                    "LIMIT ? OFFSET ?")
                : cache.get("symbol_search_wildcard_kind",
                    "SELECT n.id, n.kind, n.name, n.qualname, f.path, n.start_line, n.end_line "
                    "FROM nodes n LEFT JOIN files f ON n.file_id = f.id "
                    "WHERE n.kind = ? "
                    "ORDER BY f.path, n.start_line, n.id "
                    "LIMIT ? OFFSET ?");
            count_stmt = fn_kind
                ? cache.get("symbol_search_wildcard_fn_count",
                    "SELECT COUNT(*) FROM nodes n WHERE n.kind IN ('function', 'method')")
                : cache.get("symbol_search_wildcard_kind_count",
                    "SELECT COUNT(*) FROM nodes n WHERE n.kind = ?");
            if (!fn_kind) {
                sqlite3_bind_text(stmt, bind_idx++, kind, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(count_stmt, count_bind_idx++, kind, -1, SQLITE_TRANSIENT);
            }
        } else {
            stmt = cache.get("symbol_search_wildcard",
                "SELECT n.id, n.kind, n.name, n.qualname, f.path, n.start_line, n.end_line "
                "FROM nodes n LEFT JOIN files f ON n.file_id = f.id "
                "ORDER BY f.path, n.start_line, n.id "
                "LIMIT ? OFFSET ?");
            count_stmt = cache.get("symbol_search_wildcard_count",
                "SELECT COUNT(*) FROM nodes n");
        }
    } else {
        std::string fts_query = "name: \"" + std::string(query) + "\"*";
        std::string like_pattern = std::string("%") + query + "%";

        if (kind && strlen(kind) > 0) {
            bool fn_kind = (std::string(kind) == "function");
            stmt = fn_kind
                ? cache.get("symbol_search_fn",
                    "WITH primary_matches AS ("
                    "SELECT n.id, n.kind, n.name, n.qualname, f.path, n.start_line, n.end_line "
                    "FROM nodes_fts fts JOIN nodes n ON fts.rowid = n.id "
                    "LEFT JOIN files f ON n.file_id = f.id "
                    "WHERE nodes_fts MATCH ? AND n.kind IN ('function', 'method')"
                    "), primary_count AS (SELECT COUNT(*) AS cnt FROM primary_matches), "
                    "fallback_matches AS ("
                    "SELECT n.id, n.kind, n.name, n.qualname, f.path, n.start_line, n.end_line "
                    "FROM nodes n LEFT JOIN files f ON n.file_id = f.id "
                    "WHERE (n.name LIKE ? OR n.qualname LIKE ?) "
                    "AND n.kind IN ('function', 'method') "
                    "AND NOT EXISTS (SELECT 1 FROM primary_matches p WHERE p.id = n.id)"
                    "), combined AS ("
                    "SELECT 0 AS match_bucket, id, kind, name, qualname, path, start_line, end_line "
                    "FROM primary_matches "
                    "UNION ALL "
                    "SELECT 1 AS match_bucket, id, kind, name, qualname, path, start_line, end_line "
                    "FROM fallback_matches WHERE (SELECT cnt FROM primary_count) < 5"
                    ") "
                    "SELECT id, kind, name, qualname, path, start_line, end_line "
                    "FROM combined "
                    "ORDER BY match_bucket, length(name), name, path, start_line, id "
                    "LIMIT ? OFFSET ?")
                : cache.get("symbol_search_kind",
                    "WITH primary_matches AS ("
                    "SELECT n.id, n.kind, n.name, n.qualname, f.path, n.start_line, n.end_line "
                    "FROM nodes_fts fts JOIN nodes n ON fts.rowid = n.id "
                    "LEFT JOIN files f ON n.file_id = f.id "
                    "WHERE nodes_fts MATCH ? AND n.kind = ?"
                    "), primary_count AS (SELECT COUNT(*) AS cnt FROM primary_matches), "
                    "fallback_matches AS ("
                    "SELECT n.id, n.kind, n.name, n.qualname, f.path, n.start_line, n.end_line "
                    "FROM nodes n LEFT JOIN files f ON n.file_id = f.id "
                    "WHERE (n.name LIKE ? OR n.qualname LIKE ?) AND n.kind = ? "
                    "AND NOT EXISTS (SELECT 1 FROM primary_matches p WHERE p.id = n.id)"
                    "), combined AS ("
                    "SELECT 0 AS match_bucket, id, kind, name, qualname, path, start_line, end_line "
                    "FROM primary_matches "
                    "UNION ALL "
                    "SELECT 1 AS match_bucket, id, kind, name, qualname, path, start_line, end_line "
                    "FROM fallback_matches WHERE (SELECT cnt FROM primary_count) < 5"
                    ") "
                    "SELECT id, kind, name, qualname, path, start_line, end_line "
                    "FROM combined "
                    "ORDER BY match_bucket, length(name), name, path, start_line, id "
                    "LIMIT ? OFFSET ?");
            count_stmt = fn_kind
                ? cache.get("symbol_search_fn_count",
                    "WITH primary_matches AS ("
                    "SELECT n.id "
                    "FROM nodes_fts fts JOIN nodes n ON fts.rowid = n.id "
                    "WHERE nodes_fts MATCH ? AND n.kind IN ('function', 'method')"
                    "), primary_count AS (SELECT COUNT(*) AS cnt FROM primary_matches), "
                    "fallback_matches AS ("
                    "SELECT n.id "
                    "FROM nodes n "
                    "WHERE (n.name LIKE ? OR n.qualname LIKE ?) "
                    "AND n.kind IN ('function', 'method') "
                    "AND NOT EXISTS (SELECT 1 FROM primary_matches p WHERE p.id = n.id)"
                    "), combined AS ("
                    "SELECT id FROM primary_matches "
                    "UNION ALL "
                    "SELECT id FROM fallback_matches WHERE (SELECT cnt FROM primary_count) < 5"
                    ") "
                    "SELECT COUNT(*) FROM combined")
                : cache.get("symbol_search_kind_count",
                    "WITH primary_matches AS ("
                    "SELECT n.id "
                    "FROM nodes_fts fts JOIN nodes n ON fts.rowid = n.id "
                    "WHERE nodes_fts MATCH ? AND n.kind = ?"
                    "), primary_count AS (SELECT COUNT(*) AS cnt FROM primary_matches), "
                    "fallback_matches AS ("
                    "SELECT n.id "
                    "FROM nodes n "
                    "WHERE (n.name LIKE ? OR n.qualname LIKE ?) AND n.kind = ? "
                    "AND NOT EXISTS (SELECT 1 FROM primary_matches p WHERE p.id = n.id)"
                    "), combined AS ("
                    "SELECT id FROM primary_matches "
                    "UNION ALL "
                    "SELECT id FROM fallback_matches WHERE (SELECT cnt FROM primary_count) < 5"
                    ") "
                    "SELECT COUNT(*) FROM combined");
        } else {
            stmt = cache.get("symbol_search",
                "WITH primary_matches AS ("
                "SELECT n.id, n.kind, n.name, n.qualname, f.path, n.start_line, n.end_line "
                "FROM nodes_fts fts JOIN nodes n ON fts.rowid = n.id "
                "LEFT JOIN files f ON n.file_id = f.id "
                "WHERE nodes_fts MATCH ?"
                "), primary_count AS (SELECT COUNT(*) AS cnt FROM primary_matches), "
                "fallback_matches AS ("
                "SELECT n.id, n.kind, n.name, n.qualname, f.path, n.start_line, n.end_line "
                "FROM nodes n LEFT JOIN files f ON n.file_id = f.id "
                "WHERE (n.name LIKE ? OR n.qualname LIKE ?) "
                "AND NOT EXISTS (SELECT 1 FROM primary_matches p WHERE p.id = n.id)"
                "), combined AS ("
                "SELECT 0 AS match_bucket, id, kind, name, qualname, path, start_line, end_line "
                "FROM primary_matches "
                "UNION ALL "
                "SELECT 1 AS match_bucket, id, kind, name, qualname, path, start_line, end_line "
                "FROM fallback_matches WHERE (SELECT cnt FROM primary_count) < 5"
                ") "
                "SELECT id, kind, name, qualname, path, start_line, end_line "
                "FROM combined "
                "ORDER BY match_bucket, length(name), name, path, start_line, id "
                "LIMIT ? OFFSET ?");
            count_stmt = cache.get("symbol_search_count",
                "WITH primary_matches AS ("
                "SELECT n.id "
                "FROM nodes_fts fts JOIN nodes n ON fts.rowid = n.id "
                "WHERE nodes_fts MATCH ?"
                "), primary_count AS (SELECT COUNT(*) AS cnt FROM primary_matches), "
                "fallback_matches AS ("
                "SELECT n.id "
                "FROM nodes n "
                "WHERE (n.name LIKE ? OR n.qualname LIKE ?) "
                "AND NOT EXISTS (SELECT 1 FROM primary_matches p WHERE p.id = n.id)"
                "), combined AS ("
                "SELECT id FROM primary_matches "
                "UNION ALL "
                "SELECT id FROM fallback_matches WHERE (SELECT cnt FROM primary_count) < 5"
                ") "
                "SELECT COUNT(*) FROM combined");
        }

        sqlite3_bind_text(stmt, bind_idx++, fts_query.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(count_stmt, count_bind_idx++, fts_query.c_str(), -1, SQLITE_TRANSIENT);
        if (kind && strlen(kind) > 0 && std::string(kind) != "function") {
            sqlite3_bind_text(stmt, bind_idx++, kind, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(count_stmt, count_bind_idx++, kind, -1, SQLITE_TRANSIENT);
        }
        sqlite3_bind_text(stmt, bind_idx++, like_pattern.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, bind_idx++, like_pattern.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(count_stmt, count_bind_idx++, like_pattern.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(count_stmt, count_bind_idx++, like_pattern.c_str(), -1, SQLITE_TRANSIENT);
        if (kind && strlen(kind) > 0 && std::string(kind) != "function") {
            sqlite3_bind_text(stmt, bind_idx++, kind, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(count_stmt, count_bind_idx++, kind, -1, SQLITE_TRANSIENT);
        }
    }
    sqlite3_bind_int64(stmt, bind_idx++, limit + 1);
    sqlite3_bind_int64(stmt, bind_idx++, offset);

    int64_t total = 0;
    if (sqlite3_step(count_stmt) == SQLITE_ROW) total = sqlite3_column_int64(count_stmt, 0);

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);
    auto* results = doc.new_arr();

    int count = 0;
    bool has_more = false;
    while (sqlite3_step(stmt) == SQLITE_ROW && count <= limit) {
        if (count == limit) { has_more = true; break; }
        count++;

        auto* item = doc.new_obj();
        yyjson_mut_obj_add_int(doc.doc, item, "node_id", sqlite3_column_int64(stmt, 0));
        yyjson_mut_obj_add_strcpy(doc.doc, item, "kind",
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
        yyjson_mut_obj_add_strcpy(doc.doc, item, "name",
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
        auto* qn = sqlite3_column_text(stmt, 3);
        if (qn) yyjson_mut_obj_add_strcpy(doc.doc, item, "qualname", reinterpret_cast<const char*>(qn));
        auto* fp = sqlite3_column_text(stmt, 4);
        if (fp) yyjson_mut_obj_add_strcpy(doc.doc, item, "file_path", reinterpret_cast<const char*>(fp));

        auto* span = doc.new_obj();
        yyjson_mut_obj_add_int(doc.doc, span, "start_line", sqlite3_column_int(stmt, 5));
        yyjson_mut_obj_add_int(doc.doc, span, "end_line", sqlite3_column_int(stmt, 6));
        yyjson_mut_obj_add_val(doc.doc, item, "span", span);

        yyjson_mut_arr_append(results, item);
    }

    add_pagination_fields(doc, root, results, total, has_more, offset, limit);
    return doc.to_string();
}

// T062b: symbol_list — list/filter symbols without FTS, supports kind, file, and name-glob filters
std::string symbol_list(yyjson_val* params, Connection& conn,
                                QueryCache& cache, const std::string& /*repo_root*/) {
    const char* kind = params ? json_get_str(params, "kind") : nullptr;
    const char* file_path = params ? json_get_str(params, "file_path") : nullptr;
    const char* name_glob = params ? json_get_str(params, "name_glob") : nullptr;
    bool compact = params ? json_get_bool(params, "compact", false) : false;
    bool include_handles = params ? json_get_bool(params, "include_handles", false) : false;
    int64_t min_span_lines = params ? json_get_int(params, "min_span_lines", 0) : 0;
    int64_t limit = params ? json_get_int(params, "limit", 200) : 200;
    int64_t offset = params ? json_get_int(params, "offset", 0) : 0;
    int64_t max_bytes = params ? json_get_int(params, "max_bytes", 16000) : 16000;

    std::unordered_set<std::string> fields_set;
    bool fields_provided = false;
    std::string fields_error;
    if (!parse_symbol_fields(params, fields_set, fields_provided, fields_error)) {
        return McpError::invalid_input(fields_error).to_json_rpc(0);
    }

    if (limit > 2000) limit = 2000;
    if (limit < 1) limit = 1;
    if (offset < 0) offset = 0;
    if (max_bytes < 0) max_bytes = 0;
    if (max_bytes > 100000) max_bytes = 100000;

    std::string base_where_sql = " WHERE 1=1";
    if (file_path && strlen(file_path) > 0) base_where_sql += " AND f.path = ?";
    if (name_glob && strlen(name_glob) > 0) base_where_sql += " AND n.name GLOB ?";

    std::string semantic_sql = "1=1";
    bool semantic_filters = false;
    if (kind && strlen(kind) > 0) {
        semantic_sql += " AND n.kind = ?";
        semantic_filters = true;
    }
    if (min_span_lines > 0) {
        semantic_sql += " AND ((n.end_line - n.start_line + 1) >= ? OR ";
        semantic_sql += kPublicSymbolSql;
        semantic_sql += ")";
        semantic_filters = true;
    }

    std::string where_sql = base_where_sql;
    if (semantic_filters) where_sql += " AND " + semantic_sql;

    std::string sql =
        "SELECT n.id, n.kind, n.name, n.qualname, f.path, n.start_line, n.end_line, n.signature "
        "FROM nodes n LEFT JOIN files f ON n.file_id = f.id" + where_sql
        + " ORDER BY f.path, n.start_line, n.id LIMIT ? OFFSET ?";
    std::string count_sql =
        "SELECT COUNT(*) FROM nodes n LEFT JOIN files f ON n.file_id = f.id" + where_sql;
    std::string candidate_count_sql =
        "SELECT COUNT(*) FROM nodes n LEFT JOIN files f ON n.file_id = f.id" + base_where_sql;
    std::string hidden_public_sql =
        "SELECT COUNT(*) FROM nodes n LEFT JOIN files f ON n.file_id = f.id" + base_where_sql
        + " AND " + kPublicSymbolSql + " AND NOT (" + semantic_sql + ")";

    std::string cache_key = "symbol_list";
    if (kind && strlen(kind) > 0) cache_key += "_k";
    if (file_path && strlen(file_path) > 0) cache_key += "_f";
    if (name_glob && strlen(name_glob) > 0) cache_key += "_g";
    if (min_span_lines > 0) cache_key += "_s";

    auto bind_base = [&](sqlite3_stmt* s, int idx) {
        if (file_path && strlen(file_path) > 0)
            sqlite3_bind_text(s, idx++, file_path, -1, SQLITE_TRANSIENT);
        if (name_glob && strlen(name_glob) > 0)
            sqlite3_bind_text(s, idx++, name_glob, -1, SQLITE_TRANSIENT);
        return idx;
    };
    auto bind_semantic = [&](sqlite3_stmt* s, int idx) {
        if (kind && strlen(kind) > 0)
            sqlite3_bind_text(s, idx++, kind, -1, SQLITE_TRANSIENT);
        if (min_span_lines > 0)
            sqlite3_bind_int64(s, idx++, min_span_lines);
        return idx;
    };

    auto* stmt = cache.get(cache_key, sql);
    auto* count_stmt = cache.get(cache_key + "_count", count_sql);
    auto* candidate_count_stmt = cache.get(cache_key + "_candidate_count", candidate_count_sql);
    int bind_idx = bind_semantic(stmt, bind_base(stmt, 1));
    bind_semantic(count_stmt, bind_base(count_stmt, 1));
    bind_base(candidate_count_stmt, 1);
    sqlite3_bind_int64(stmt, bind_idx++, limit + 1);
    sqlite3_bind_int64(stmt, bind_idx++, offset);

    int64_t total = 0;
    if (sqlite3_step(count_stmt) == SQLITE_ROW) total = sqlite3_column_int64(count_stmt, 0);
    int64_t total_candidates = total;
    if (sqlite3_step(candidate_count_stmt) == SQLITE_ROW)
        total_candidates = sqlite3_column_int64(candidate_count_stmt, 0);
    int64_t hidden_public_count = 0;
    if (semantic_filters) {
        auto* hidden_public_stmt = cache.get(cache_key + "_hidden_public_count", hidden_public_sql);
        bind_semantic(hidden_public_stmt, bind_base(hidden_public_stmt, 1));
        if (sqlite3_step(hidden_public_stmt) == SQLITE_ROW)
            hidden_public_count = sqlite3_column_int64(hidden_public_stmt, 0);
    }

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);
    auto* results = doc.new_arr();

    int count = 0;
    bool has_more = false;
    bool budget_exceeded = false;
    size_t approx_bytes = 384;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= limit) { has_more = true; break; }

        auto* qn_txt = sqlite3_column_text(stmt, 3);
        auto* fp_txt = sqlite3_column_text(stmt, 4);
        auto* sig_txt = sqlite3_column_text(stmt, 7);
        const char* row_kind = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* row_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        const char* row_qn = qn_txt ? reinterpret_cast<const char*>(qn_txt) : nullptr;
        const char* row_file = fp_txt ? reinterpret_cast<const char*>(fp_txt) : nullptr;
        const char* row_sig = sig_txt ? reinterpret_cast<const char*>(sig_txt) : nullptr;
        size_t item_bytes = estimate_symbol_listing_bytes(row_kind, row_name, row_qn, row_file,
            row_sig, include_handles, fields_provided, fields_set);
        if (max_bytes > 0 && count > 0 && approx_bytes + item_bytes > static_cast<size_t>(max_bytes)) {
            has_more = true;
            budget_exceeded = true;
            break;
        }

        auto* item = doc.new_obj();
        add_symbol_listing_result(doc, item,
            sqlite3_column_int64(stmt, 0), row_kind, row_name, row_qn, row_file,
            sqlite3_column_int(stmt, 5), sqlite3_column_int(stmt, 6), row_sig,
            include_handles, compact, true, "file", fields_set, fields_provided);

        yyjson_mut_arr_append(results, item);
        count++;
        approx_bytes += item_bytes + 1;
    }

    add_pagination_fields(doc, root, results, total, has_more, offset, limit);
    yyjson_mut_obj_add_int(doc.doc, root, "total_candidates", total_candidates);
    yyjson_mut_obj_add_int(doc.doc, root, "filtered_hidden", total_candidates > total ? total_candidates - total : 0);
    yyjson_mut_obj_add_int(doc.doc, root, "hidden_public_count", hidden_public_count);
    yyjson_mut_obj_add_bool(doc.doc, root, "min_span_lines_lossy", min_span_lines > 0);
    yyjson_mut_obj_add_int(doc.doc, root, "max_bytes", max_bytes);
    if (has_more) yyjson_mut_obj_add_int(doc.doc, root, "next_offset", offset + count);
    if (budget_exceeded) yyjson_mut_obj_add_bool(doc.doc, root, "budget_exceeded", true);
    if (min_span_lines > 0 || hidden_public_count > 0) {
        auto* warnings = doc.new_arr();
        if (min_span_lines > 0)
            yyjson_mut_arr_add_str(doc.doc, warnings,
                "min_span_lines is lossy; public/API-like symbols bypass span pruning");
        if (hidden_public_count > 0)
            yyjson_mut_arr_add_str(doc.doc, warnings,
                "kind filters hide public/API-like symbols; inspect total_candidates/hidden_public_count");
        yyjson_mut_obj_add_val(doc.doc, root, "warnings", warnings);
    }
    return doc.to_string();
}

// T062c: symbols_in_path — list symbols under a directory subtree
std::string symbols_in_path(yyjson_val* params, Connection& conn,
                            QueryCache& cache, const std::string& repo_root) {
    const char* path = params ? json_get_str(params, "path") : nullptr;
    if (!path) return McpError::invalid_input("Missing 'path' parameter").to_json_rpc(0);

    bool recursive = params ? json_get_bool(params, "recursive", true) : true;
    bool compact = params ? json_get_bool(params, "compact", true) : true;
    bool include_handles = params ? json_get_bool(params, "include_handles", false) : false;
    int64_t min_span_lines = params ? json_get_int(params, "min_span_lines", 0) : 0;
    int64_t limit = params ? json_get_int(params, "limit", 200) : 200;
    int64_t offset = params ? json_get_int(params, "offset", 0) : 0;
    int64_t max_bytes = params ? json_get_int(params, "max_bytes", 16000) : 16000;
    if (limit > 2000) limit = 2000;
    if (limit < 1) limit = 1;
    if (offset < 0) offset = 0;
    if (max_bytes < 0) max_bytes = 0;
    if (max_bytes > 100000) max_bytes = 100000;

    std::unordered_set<std::string> fields_set;
    bool fields_provided = false;
    std::string fields_error;
    if (!parse_symbol_fields(params, fields_set, fields_provided, fields_error)) {
        return McpError::invalid_input(fields_error).to_json_rpc(0);
    }

    std::vector<std::string> kinds;
    if (params) {
        if (auto* kind_arr = yyjson_obj_get(params, "kind"); kind_arr && yyjson_is_arr(kind_arr)) {
            yyjson_val* kind_val;
            size_t idx, max;
            yyjson_arr_foreach(kind_arr, idx, max, kind_val) {
                if (yyjson_is_str(kind_val)) {
                    kinds.emplace_back(yyjson_get_str(kind_val));
                }
            }
        } else if (auto* kind_str = yyjson_obj_get(params, "kind"); kind_str && yyjson_is_str(kind_str)) {
            kinds.emplace_back(yyjson_get_str(kind_str));
        }
    }

    bool all_paths = (strcmp(path, ".") == 0 || strcmp(path, "./") == 0);
    std::string resolved_file = all_paths ? std::string() : resolve_db_path(conn.raw(), path, repo_root);
    std::string resolved_dir = all_paths ? std::string() : resolve_db_dir_path(conn.raw(), path, repo_root);
    if (!all_paths && resolved_file.empty() && resolved_dir.empty()) {
        if (!std::filesystem::path(path).is_absolute()) {
            auto validated = path_util::validate_mcp_path(path, repo_root);
            if (validated.empty())
                return McpError::invalid_input("Invalid path: rejected by traversal guard").to_json_rpc(0);
        }
        return McpError::not_found(std::string("Path not found: ") + path).to_json_rpc(0);
    }

    std::string base_where_sql = " WHERE 1=1";
    std::vector<std::string> path_binds;
    if (!resolved_file.empty()) {
        base_where_sql += " AND f.path = ?";
        path_binds.push_back(resolved_file);
    } else if (!all_paths) {
        std::string dir = resolved_dir;
        if (!dir.empty() && dir.back() != '/') dir += '/';
        if (recursive) {
            base_where_sql += " AND f.path GLOB ?";
            path_binds.push_back(dir + "*");
        } else {
            base_where_sql += " AND f.path GLOB ? AND f.path NOT GLOB ?";
            path_binds.push_back(dir + "*");
            path_binds.push_back(dir + "*/*");
        }
    }

    std::string semantic_sql = "1=1";
    bool semantic_filters = false;
    if (!kinds.empty()) {
        semantic_sql += " AND n.kind IN (";
        for (size_t i = 0; i < kinds.size(); ++i) {
            if (i) semantic_sql += ", ";
            semantic_sql += "?";
        }
        semantic_sql += ")";
        semantic_filters = true;
    }
    if (min_span_lines > 0) {
        semantic_sql += " AND ((n.end_line - n.start_line + 1) >= ? OR ";
        semantic_sql += kPublicSymbolSql;
        semantic_sql += ")";
        semantic_filters = true;
    }

    std::string where_sql = base_where_sql;
    if (semantic_filters) where_sql += " AND " + semantic_sql;

    std::string sql =
        "SELECT n.id, n.kind, n.name, n.qualname, f.path, n.start_line, n.end_line, n.signature "
        "FROM nodes n LEFT JOIN files f ON n.file_id = f.id" + where_sql
        + " ORDER BY f.path, n.start_line, n.id LIMIT ? OFFSET ?";
    std::string count_sql =
        "SELECT COUNT(*) FROM nodes n LEFT JOIN files f ON n.file_id = f.id" + where_sql;
    std::string candidate_count_sql =
        "SELECT COUNT(*) FROM nodes n LEFT JOIN files f ON n.file_id = f.id" + base_where_sql;
    std::string hidden_public_sql =
        "SELECT COUNT(*) FROM nodes n LEFT JOIN files f ON n.file_id = f.id" + base_where_sql
        + " AND " + kPublicSymbolSql + " AND NOT (" + semantic_sql + ")";

    std::string cache_key = "symbols_in_path";
    if (all_paths) cache_key += "_all";
    else if (!resolved_file.empty()) cache_key += "_file";
    else cache_key += recursive ? "_dir_r" : "_dir_n";
    if (!kinds.empty()) cache_key += "_k" + std::to_string(kinds.size());
    if (min_span_lines > 0) cache_key += "_s";

    auto bind_base = [&](sqlite3_stmt* s, int idx) {
        for (const auto& bind : path_binds)
            sqlite3_bind_text(s, idx++, bind.c_str(), -1, SQLITE_TRANSIENT);
        return idx;
    };
    auto bind_semantic = [&](sqlite3_stmt* s, int idx) {
        for (const auto& k : kinds)
            sqlite3_bind_text(s, idx++, k.c_str(), -1, SQLITE_TRANSIENT);
        if (min_span_lines > 0)
            sqlite3_bind_int64(s, idx++, min_span_lines);
        return idx;
    };

    auto* stmt = cache.get(cache_key, sql);
    auto* count_stmt = cache.get(cache_key + "_count", count_sql);
    auto* candidate_count_stmt = cache.get(cache_key + "_candidate_count", candidate_count_sql);
    int bind_idx = bind_semantic(stmt, bind_base(stmt, 1));
    bind_semantic(count_stmt, bind_base(count_stmt, 1));
    bind_base(candidate_count_stmt, 1);
    sqlite3_bind_int64(stmt, bind_idx++, limit + 1);
    sqlite3_bind_int64(stmt, bind_idx++, offset);

    int64_t total = 0;
    if (sqlite3_step(count_stmt) == SQLITE_ROW) total = sqlite3_column_int64(count_stmt, 0);
    int64_t total_candidates = total;
    if (sqlite3_step(candidate_count_stmt) == SQLITE_ROW)
        total_candidates = sqlite3_column_int64(candidate_count_stmt, 0);
    int64_t hidden_public_count = 0;
    if (semantic_filters) {
        auto* hidden_public_stmt = cache.get(cache_key + "_hidden_public_count", hidden_public_sql);
        bind_semantic(hidden_public_stmt, bind_base(hidden_public_stmt, 1));
        if (sqlite3_step(hidden_public_stmt) == SQLITE_ROW)
            hidden_public_count = sqlite3_column_int64(hidden_public_stmt, 0);
    }

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);
    yyjson_mut_obj_add_strcpy(doc.doc, root, "path", path);
    yyjson_mut_obj_add_bool(doc.doc, root, "recursive", recursive);
    auto* results = doc.new_arr();

    int count = 0;
    bool has_more = false;
    bool budget_exceeded = false;
    size_t approx_bytes = 384 + std::strlen(path);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= limit) { has_more = true; break; }

        auto* qn_txt = sqlite3_column_text(stmt, 3);
        auto* fp_txt = sqlite3_column_text(stmt, 4);
        auto* sig_txt = sqlite3_column_text(stmt, 7);
        const char* row_kind = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* row_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        const char* row_qn = qn_txt ? reinterpret_cast<const char*>(qn_txt) : nullptr;
        const char* row_file = fp_txt ? reinterpret_cast<const char*>(fp_txt) : nullptr;
        const char* row_sig = sig_txt ? reinterpret_cast<const char*>(sig_txt) : nullptr;
        size_t item_bytes = estimate_symbol_listing_bytes(row_kind, row_name, row_qn, row_file,
            row_sig, include_handles, fields_provided, fields_set);
        if (max_bytes > 0 && count > 0 && approx_bytes + item_bytes > static_cast<size_t>(max_bytes)) {
            has_more = true;
            budget_exceeded = true;
            break;
        }

        auto* item = doc.new_obj();
        add_symbol_listing_result(doc, item,
            sqlite3_column_int64(stmt, 0), row_kind, row_name, row_qn, row_file,
            sqlite3_column_int(stmt, 5), sqlite3_column_int(stmt, 6), row_sig,
            include_handles, compact, false, "file_path", fields_set, fields_provided);
        yyjson_mut_arr_append(results, item);
        count++;
        approx_bytes += item_bytes + 1;
    }

    add_pagination_fields(doc, root, results, total, has_more, offset, limit);
    yyjson_mut_obj_add_int(doc.doc, root, "total_candidates", total_candidates);
    yyjson_mut_obj_add_int(doc.doc, root, "filtered_hidden", total_candidates > total ? total_candidates - total : 0);
    yyjson_mut_obj_add_int(doc.doc, root, "hidden_public_count", hidden_public_count);
    yyjson_mut_obj_add_bool(doc.doc, root, "min_span_lines_lossy", min_span_lines > 0);
    yyjson_mut_obj_add_int(doc.doc, root, "max_bytes", max_bytes);
    if (has_more) yyjson_mut_obj_add_int(doc.doc, root, "next_offset", offset + count);
    if (budget_exceeded) yyjson_mut_obj_add_bool(doc.doc, root, "budget_exceeded", true);
    if (min_span_lines > 0 || hidden_public_count > 0) {
        auto* warnings = doc.new_arr();
        if (min_span_lines > 0)
            yyjson_mut_arr_add_str(doc.doc, warnings,
                "min_span_lines is lossy; public/API-like symbols bypass span pruning");
        if (hidden_public_count > 0)
            yyjson_mut_arr_add_str(doc.doc, warnings,
                "kind filters hide public/API-like symbols; inspect total_candidates/hidden_public_count");
        yyjson_mut_obj_add_val(doc.doc, root, "warnings", warnings);
    }
    return doc.to_string();
}

// Helper: read source snippet from disk
std::string read_source_snippet(const std::string& repo_root,
                                        const std::string& rel_path,
                                        int start_line, int end_line) {
    std::filesystem::path full_path;
    if (std::filesystem::path(rel_path).is_absolute()) {
        full_path = rel_path;
    } else {
        auto validated = path_util::validate_mcp_path(rel_path, repo_root);
        if (validated.empty()) return "";
        full_path = std::filesystem::path(repo_root) / rel_path;
    }
    std::ifstream f(full_path);
    if (!f) return "";

    std::string result;
    std::string line;
    int ln = 0;
    while (std::getline(f, line)) {
        ln++;
        if (ln >= start_line && ln <= end_line) {
            result += line + "\n";
        }
        if (ln > end_line) break;
    }
    return result;
}

static std::string truncate_source_lines(const std::string& source, int64_t max_source_lines) {
    if (max_source_lines <= 0 || source.empty()) return source;

    std::istringstream in(source);
    std::string result;
    std::string line;
    int64_t line_count = 0;
    while (std::getline(in, line)) {
        if (line_count == max_source_lines) {
            result += "...(truncated)";
            return result;
        }
        result += line + "\n";
        line_count++;
    }
    return source;
}

// T063: symbol_get
std::string symbol_get(yyjson_val* params, Connection& conn,
                               QueryCache& cache, const std::string& repo_root) {
    int64_t node_id = resolve_node_id(params, conn, cache);
    if (node_id < 0) return McpError::invalid_input("Missing 'node_id'").to_json_rpc(0);

    bool include_source = params ? json_get_bool(params, "include_source", true) : true;

    auto* stmt = cache.get("symbol_get",
        "SELECT n.id, n.kind, n.name, n.qualname, n.signature, f.path, "
        "n.start_line, n.start_col, n.end_line, n.end_col, n.doc, f.mtime_ns "
        "FROM nodes n LEFT JOIN files f ON n.file_id = f.id "
        "WHERE n.id = ? AND n.node_type = 'symbol'");

    sqlite3_bind_int64(stmt, 1, node_id);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        return McpError::not_found("Symbol not found: " + std::to_string(node_id)).to_json_rpc(0);
    }

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);

    yyjson_mut_obj_add_int(doc.doc, root, "node_id", sqlite3_column_int64(stmt, 0));
    yyjson_mut_obj_add_strcpy(doc.doc, root, "kind",
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
    yyjson_mut_obj_add_strcpy(doc.doc, root, "name",
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));

    auto* qn = sqlite3_column_text(stmt, 3);
    if (qn) yyjson_mut_obj_add_strcpy(doc.doc, root, "qualname", reinterpret_cast<const char*>(qn));
    auto* sig = sqlite3_column_text(stmt, 4);
    if (sig) yyjson_mut_obj_add_strcpy(doc.doc, root, "signature", reinterpret_cast<const char*>(sig));

    auto* fp = sqlite3_column_text(stmt, 5);
    std::string file_path = fp ? reinterpret_cast<const char*>(fp) : "";
    if (!file_path.empty()) yyjson_mut_obj_add_strcpy(doc.doc, root, "file_path", file_path.c_str());

    int start_line = sqlite3_column_int(stmt, 6);
    int end_line = sqlite3_column_int(stmt, 8);

    auto* span = doc.new_obj();
    yyjson_mut_obj_add_int(doc.doc, span, "start_line", start_line);
    yyjson_mut_obj_add_int(doc.doc, span, "end_line", end_line);
    yyjson_mut_obj_add_val(doc.doc, root, "span", span);

    auto* doc_text = sqlite3_column_text(stmt, 10);
    if (doc_text) yyjson_mut_obj_add_strcpy(doc.doc, root, "doc", reinterpret_cast<const char*>(doc_text));

    // Source snippet + staleness detection (T071)
    if (include_source && !file_path.empty()) {
        auto source = read_source_snippet(repo_root, file_path, start_line, end_line);
        if (!source.empty()) {
            yyjson_mut_obj_add_strcpy(doc.doc, root, "source", source.c_str());
        }

        // Staleness check
        int64_t indexed_mtime = sqlite3_column_int64(stmt, 11);
        auto full = std::filesystem::path(repo_root) / file_path;
        std::error_code ec;
        auto current_mtime = std::filesystem::last_write_time(full, ec);
        if (!ec) {
            auto current_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                current_mtime.time_since_epoch()).count();
            if (current_ns != indexed_mtime) {
                yyjson_mut_obj_add_bool(doc.doc, root, "stale", true);
                yyjson_mut_obj_add_str(doc.doc, root, "stale_reason", "file modified since last index");
            }
        }
    }

    return doc.to_string();
}

// T064: symbol_get_batch — multi-ID lookup
std::string symbol_get_batch(yyjson_val* params, Connection& conn,
                                     QueryCache& cache, const std::string& repo_root) {
    if (!params) return McpError::invalid_input("Missing parameters").to_json_rpc(0);

    auto* ids_val = yyjson_obj_get(params, "node_ids");
    if (!ids_val || !yyjson_is_arr(ids_val))
        return McpError::invalid_input("Missing 'node_ids' array").to_json_rpc(0);

    size_t count = yyjson_arr_size(ids_val);
    if (count > 50) count = 50;

    bool include_source = json_get_bool(params, "include_source", true);

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);
    auto* results = doc.new_arr();

    yyjson_val* id_item;
    size_t idx, max;
    yyjson_arr_foreach(ids_val, idx, max, id_item) {
        if (idx >= 50) break;

        int64_t nid = yyjson_get_sint(id_item);

        auto* stmt = cache.get("symbol_get",
            "SELECT n.id, n.kind, n.name, n.qualname, n.signature, f.path, "
            "n.start_line, n.start_col, n.end_line, n.end_col, n.doc, f.mtime_ns "
            "FROM nodes n LEFT JOIN files f ON n.file_id = f.id "
            "WHERE n.id = ? AND n.node_type = 'symbol'");
        sqlite3_bind_int64(stmt, 1, nid);

        if (sqlite3_step(stmt) != SQLITE_ROW) continue;  // Silently omit missing

        auto* item = doc.new_obj();
        yyjson_mut_obj_add_int(doc.doc, item, "node_id", sqlite3_column_int64(stmt, 0));
        yyjson_mut_obj_add_strcpy(doc.doc, item, "kind",
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
        yyjson_mut_obj_add_strcpy(doc.doc, item, "name",
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
        auto* qn = sqlite3_column_text(stmt, 3);
        if (qn) yyjson_mut_obj_add_strcpy(doc.doc, item, "qualname", reinterpret_cast<const char*>(qn));
        auto* sig = sqlite3_column_text(stmt, 4);
        if (sig) yyjson_mut_obj_add_strcpy(doc.doc, item, "signature", reinterpret_cast<const char*>(sig));
        auto* fp = sqlite3_column_text(stmt, 5);
        std::string file_path = fp ? reinterpret_cast<const char*>(fp) : "";
        if (!file_path.empty()) yyjson_mut_obj_add_strcpy(doc.doc, item, "file_path", file_path.c_str());

        int start_line = sqlite3_column_int(stmt, 6);
        int end_line = sqlite3_column_int(stmt, 8);
        auto* span = doc.new_obj();
        yyjson_mut_obj_add_int(doc.doc, span, "start_line", start_line);
        yyjson_mut_obj_add_int(doc.doc, span, "end_line", end_line);
        yyjson_mut_obj_add_val(doc.doc, item, "span", span);

        if (include_source && !file_path.empty()) {
            auto source = read_source_snippet(repo_root, file_path, start_line, end_line);
            if (!source.empty()) yyjson_mut_obj_add_strcpy(doc.doc, item, "source", source.c_str());
        }

        yyjson_mut_arr_append(results, item);
    }

    yyjson_mut_obj_add_val(doc.doc, root, "results", results);
    return doc.to_string();
}

// T066: callers_approx
std::string callers_approx(yyjson_val* params, Connection& conn,
                                    QueryCache& cache, const std::string& repo_root) {
    int64_t node_id = resolve_node_id(params, conn, cache);
    if (node_id < 0) return McpError::invalid_input("Missing 'node_id'").to_json_rpc(0);

    int64_t limit = params ? json_get_int(params, "limit", 50) : 50;
    if (limit > 500) limit = 500;

    const char* group_by = params ? json_get_str(params, "group_by") : nullptr;
    bool compact = params ? json_get_bool(params, "compact", false) : false;
    CandidateMode candidate_mode = parse_candidate_mode(params);
    CallsiteCandidateOptions candidate_options = parse_callsite_candidate_options(params);
    const char* response_mode = params ? json_get_str(params, "response_mode") : nullptr;
    bool lean_response = (response_mode && std::strcmp(response_mode, "lean") == 0) ||
                         (params && json_get_bool(params, "lean", false));
    bool include_buckets = !params || json_get_bool(params, "buckets", true);
    int64_t top_n = params ? json_get_int(params, "top_n", 10) : 10;
    if (top_n <= 0) top_n = 10;
    if (top_n > 100) top_n = 100;
    if (lean_response && params && !yyjson_obj_get(params, "max_bytes"))
        candidate_options.max_bytes = 4096;
    int64_t exact_limit = lean_response ? std::min<int64_t>(limit, top_n) : limit;

    int64_t exact_total = 0;
    {
        auto* count_stmt = cache.get("callers_approx_exact_count",
            "SELECT COUNT(*) FROM edges WHERE dst_id = ? AND kind = 'calls'");
        sqlite3_bind_int64(count_stmt, 1, node_id);
        if (sqlite3_step(count_stmt) == SQLITE_ROW)
            exact_total = sqlite3_column_int64(count_stmt, 0);
    }

    auto* stmt = cache.get("callers_approx",
        "SELECT e.src_id, n.kind, n.name, n.qualname, f.path, n.start_line, n.end_line, e.confidence "
        "FROM edges e "
        "JOIN nodes n ON e.src_id = n.id "
        "LEFT JOIN files f ON n.file_id = f.id "
        "WHERE e.dst_id = ? AND e.kind = 'calls' "
        "ORDER BY e.confidence DESC LIMIT ?");

    sqlite3_bind_int64(stmt, 1, node_id);
    sqlite3_bind_int64(stmt, 2, exact_limit);

    // Collect raw results
    struct CallerRow {
        int64_t id;
        std::string kind;
        std::string name;
        std::string qualname;
        std::string file_path;
        int start_line = 0;
        int end_line = 0;
        double confidence;
    };
    std::vector<CallerRow> rows;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        CallerRow r;
        r.id = sqlite3_column_int64(stmt, 0);
        r.kind = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        r.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        auto* qn = sqlite3_column_text(stmt, 3);
        r.qualname = qn ? reinterpret_cast<const char*>(qn) : "";
        auto* fp = sqlite3_column_text(stmt, 4);
        r.file_path = fp ? reinterpret_cast<const char*>(fp) : "";
        r.start_line = sqlite3_column_int(stmt, 5);
        r.end_line = sqlite3_column_int(stmt, 6);
        r.confidence = sqlite3_column_double(stmt, 7);
        rows.push_back(std::move(r));
    }
    CallsiteCandidateSet candidates;
    if (should_collect_candidates(candidate_mode, rows.empty()))
        candidates = collect_callsite_candidates(
            node_id, lean_response ? top_n : limit, conn, cache, candidate_options, repo_root);

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);

    auto emit_caller_item = [&](const CallerRow& r) -> yyjson_mut_val* {
        auto* item = doc.new_obj();
        if (!compact) {
            yyjson_mut_obj_add_int(doc.doc, item, "caller_node_id", r.id);
            yyjson_mut_obj_add_strcpy(doc.doc, item, "caller_name", r.name.c_str());
            if (!r.file_path.empty())
                yyjson_mut_obj_add_strcpy(doc.doc, item, "file_path", r.file_path.c_str());
        } else {
            add_symbol_result(doc, item, r.id, r.kind.c_str(), r.name.c_str(),
                r.qualname.empty() ? nullptr : r.qualname.c_str(),
                r.file_path.empty() ? nullptr : r.file_path.c_str(),
                r.start_line, r.end_line, true, true, "file", {}, false);
        }
        yyjson_mut_obj_add_real(doc.doc, item, "confidence", r.confidence);
        return item;
    };

    auto emit_caller_item_lean = [&](const CallerRow& r) -> yyjson_mut_val* {
        auto* item = doc.new_obj();
        if (!r.file_path.empty())
            yyjson_mut_obj_add_strcpy(doc.doc, item, "file", r.file_path.c_str());
        yyjson_mut_obj_add_int(doc.doc, item, "line", r.start_line);
        yyjson_mut_obj_add_strcpy(doc.doc, item, "caller", r.name.c_str());
        yyjson_mut_obj_add_real(doc.doc, item, "confidence", r.confidence);
        return item;
    };

    if (lean_response) {
        yyjson_mut_obj_add_str(doc.doc, root, "response_mode", "lean");
        yyjson_mut_obj_add_int(doc.doc, root, "exact_total", exact_total);
        yyjson_mut_obj_add_int(doc.doc, root, "top_n", top_n);
        yyjson_mut_obj_add_int(doc.doc, root, "max_bytes", candidate_options.max_bytes);

        auto* exact_results = doc.new_arr();
        for (const auto& r : rows)
            yyjson_mut_arr_append(exact_results, emit_caller_item_lean(r));
        yyjson_mut_obj_add_val(doc.doc, root, "results", exact_results);
        yyjson_mut_obj_add_bool(doc.doc, root, "has_more", exact_total > static_cast<int64_t>(rows.size()));

        if (!candidates.rows.empty() || should_collect_candidates(candidate_mode, rows.empty())) {
            auto* candidate_results = doc.new_arr();
            for (const auto& candidate : candidates.rows)
                yyjson_mut_arr_append(candidate_results, emit_callsite_candidate_lean(doc, candidate));
            yyjson_mut_obj_add_val(doc.doc, root, "candidate_results", candidate_results);
            yyjson_mut_obj_add_str(doc.doc, root, "candidate_source", "refs");
            add_callsite_candidate_metadata(doc, root, "candidate", candidates, candidate_options);
            if (include_buckets) add_callsite_candidate_buckets(doc, root, candidates);
        }
        return doc.to_string();
    }

    if (group_by && (strcmp(group_by, "file") == 0 || strcmp(group_by, "module") == 0)) {
        // Group by file path (module uses file's directory as key)
        std::unordered_map<std::string, std::vector<size_t>> groups;
        for (size_t i = 0; i < rows.size(); ++i) {
            std::string key = rows[i].file_path;
            if (strcmp(group_by, "module") == 0 && !key.empty()) {
                auto pos = key.rfind('/');
                key = (pos != std::string::npos) ? key.substr(0, pos) : "";
            }
            groups[key].push_back(i);
        }
        auto* grouped = doc.new_obj();
        for (auto& [gkey, indices] : groups) {
            auto* arr = doc.new_arr();
            for (size_t idx : indices)
                yyjson_mut_arr_append(arr, emit_caller_item(rows[idx]));
            yyjson_mut_obj_add_val(doc.doc, grouped, gkey.empty() ? "(unknown)" : gkey.c_str(), arr);
        }
        yyjson_mut_obj_add_val(doc.doc, root, "groups", grouped);
    } else if (group_by && strcmp(group_by, "symbol") == 0) {
        // Group by caller name
        std::unordered_map<std::string, std::vector<size_t>> groups;
        for (size_t i = 0; i < rows.size(); ++i) groups[rows[i].name].push_back(i);
        auto* grouped = doc.new_obj();
        for (auto& [gkey, indices] : groups) {
            auto* arr = doc.new_arr();
            for (size_t idx : indices)
                yyjson_mut_arr_append(arr, emit_caller_item(rows[idx]));
            yyjson_mut_obj_add_val(doc.doc, grouped, gkey.c_str(), arr);
        }
        yyjson_mut_obj_add_val(doc.doc, root, "groups", grouped);
    } else {
        // Flat list (default / backward-compatible)
        auto* results = doc.new_arr();
        for (auto& r : rows)
            yyjson_mut_arr_append(results, emit_caller_item(r));
        yyjson_mut_obj_add_val(doc.doc, root, "results", results);
        yyjson_mut_obj_add_bool(doc.doc, root, "has_more", false);
    }

    if (!candidates.rows.empty() || should_collect_candidates(candidate_mode, rows.empty())) {
        auto* candidate_results = doc.new_arr();
        for (const auto& candidate : candidates.rows)
            yyjson_mut_arr_append(candidate_results,
                emit_callsite_candidate(doc, candidate, candidate_options.include_handles));
        yyjson_mut_obj_add_val(doc.doc, root, "candidate_results", candidate_results);
        yyjson_mut_obj_add_str(doc.doc, root, "candidate_source", "refs");
        add_callsite_candidate_metadata(doc, root, "candidate", candidates, candidate_options);
    }

    return doc.to_string();
}

// T067: callees_approx
std::string callees_approx(yyjson_val* params, Connection& conn,
                                    QueryCache& cache, const std::string& /*repo_root*/) {
    int64_t node_id = resolve_node_id(params, conn, cache);
    if (node_id < 0) return McpError::invalid_input("Missing 'node_id'").to_json_rpc(0);

    int64_t limit = params ? json_get_int(params, "limit", 50) : 50;
    if (limit > 500) limit = 500;

    const char* group_by = params ? json_get_str(params, "group_by") : nullptr;
    bool compact = params ? json_get_bool(params, "compact", false) : false;

    auto* stmt = cache.get("callees_approx",
        "SELECT e.dst_id, n.kind, n.name, n.qualname, n.signature, e.confidence, f.path, n.start_line, n.end_line "
        "FROM edges e "
        "JOIN nodes n ON e.dst_id = n.id "
        "LEFT JOIN files f ON n.file_id = f.id "
        "WHERE e.src_id = ? AND e.kind = 'calls' "
        "ORDER BY e.confidence DESC LIMIT ?");

    sqlite3_bind_int64(stmt, 1, node_id);
    sqlite3_bind_int64(stmt, 2, limit);

    // Collect raw results
    struct CalleeRow {
        int64_t id;
        std::string kind;
        std::string name;
        std::string qualname;
        std::string signature;
        std::string file_path;
        int start_line = 0;
        int end_line = 0;
        double confidence;
    };
    std::vector<CalleeRow> rows;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        CalleeRow r;
        r.id = sqlite3_column_int64(stmt, 0);
        r.kind = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        r.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        auto* qn = sqlite3_column_text(stmt, 3);
        r.qualname = qn ? reinterpret_cast<const char*>(qn) : "";
        auto* sig = sqlite3_column_text(stmt, 4);
        r.signature = sig ? reinterpret_cast<const char*>(sig) : "";
        r.confidence = sqlite3_column_double(stmt, 5);
        auto* fp = sqlite3_column_text(stmt, 6);
        r.file_path = fp ? reinterpret_cast<const char*>(fp) : "";
        r.start_line = sqlite3_column_int(stmt, 7);
        r.end_line = sqlite3_column_int(stmt, 8);
        rows.push_back(std::move(r));
    }

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);

    auto emit_callee_item = [&](const CalleeRow& r) -> yyjson_mut_val* {
        auto* item = doc.new_obj();
        if (!compact) {
            yyjson_mut_obj_add_int(doc.doc, item, "callee_node_id", r.id);
            yyjson_mut_obj_add_strcpy(doc.doc, item, "callee_name", r.name.c_str());
            if (!r.qualname.empty())
                yyjson_mut_obj_add_strcpy(doc.doc, item, "qualname", r.qualname.c_str());
            if (!r.signature.empty())
                yyjson_mut_obj_add_strcpy(doc.doc, item, "signature", r.signature.c_str());
            if (!r.file_path.empty())
                yyjson_mut_obj_add_strcpy(doc.doc, item, "file_path", r.file_path.c_str());
        } else {
            add_symbol_result(doc, item, r.id, r.kind.c_str(), r.name.c_str(),
                r.qualname.empty() ? nullptr : r.qualname.c_str(),
                r.file_path.empty() ? nullptr : r.file_path.c_str(),
                r.start_line, r.end_line, true, true, "file", {}, false);
        }
        yyjson_mut_obj_add_real(doc.doc, item, "confidence", r.confidence);
        return item;
    };

    if (group_by && (strcmp(group_by, "file") == 0 || strcmp(group_by, "module") == 0)) {
        std::unordered_map<std::string, std::vector<size_t>> groups;
        for (size_t i = 0; i < rows.size(); ++i) {
            std::string key = rows[i].file_path;
            if (strcmp(group_by, "module") == 0 && !key.empty()) {
                auto pos = key.rfind('/');
                key = (pos != std::string::npos) ? key.substr(0, pos) : "";
            }
            groups[key].push_back(i);
        }
        auto* grouped = doc.new_obj();
        for (auto& [gkey, indices] : groups) {
            auto* arr = doc.new_arr();
            for (size_t idx : indices)
                yyjson_mut_arr_append(arr, emit_callee_item(rows[idx]));
            yyjson_mut_obj_add_val(doc.doc, grouped, gkey.empty() ? "(unknown)" : gkey.c_str(), arr);
        }
        yyjson_mut_obj_add_val(doc.doc, root, "groups", grouped);
    } else if (group_by && strcmp(group_by, "symbol") == 0) {
        std::unordered_map<std::string, std::vector<size_t>> groups;
        for (size_t i = 0; i < rows.size(); ++i) groups[rows[i].name].push_back(i);
        auto* grouped = doc.new_obj();
        for (auto& [gkey, indices] : groups) {
            auto* arr = doc.new_arr();
            for (size_t idx : indices)
                yyjson_mut_arr_append(arr, emit_callee_item(rows[idx]));
            yyjson_mut_obj_add_val(doc.doc, grouped, gkey.c_str(), arr);
        }
        yyjson_mut_obj_add_val(doc.doc, root, "groups", grouped);
    } else {
        // Flat list (default)
        auto* results = doc.new_arr();
        for (auto& r : rows)
            yyjson_mut_arr_append(results, emit_callee_item(r));
        yyjson_mut_obj_add_val(doc.doc, root, "results", results);
    }

    return doc.to_string();
}

// T065: references
std::string references(yyjson_val* params, Connection& conn,
                               QueryCache& cache, const std::string& /*repo_root*/) {
    int64_t node_id = resolve_node_id(params, conn, cache);
    if (node_id < 0) return McpError::invalid_input("Missing 'node_id'").to_json_rpc(0);

    int64_t limit = params ? json_get_int(params, "limit", 50) : 50;
    if (limit > 500) limit = 500;

    auto* stmt = cache.get("references",
        "SELECT r.id, r.kind, r.name, f.path, r.start_line, r.start_col, r.end_line, r.end_col, r.evidence "
        "FROM refs r LEFT JOIN files f ON r.file_id = f.id "
        "WHERE r.resolved_node_id = ? LIMIT ?");

    sqlite3_bind_int64(stmt, 1, node_id);
    sqlite3_bind_int64(stmt, 2, limit);

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);
    auto* results = doc.new_arr();

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        auto* item = doc.new_obj();
        yyjson_mut_obj_add_int(doc.doc, item, "ref_id", sqlite3_column_int64(stmt, 0));
        yyjson_mut_obj_add_strcpy(doc.doc, item, "kind",
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
        yyjson_mut_obj_add_strcpy(doc.doc, item, "name",
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
        auto* fp = sqlite3_column_text(stmt, 3);
        if (fp) yyjson_mut_obj_add_strcpy(doc.doc, item, "file_path", reinterpret_cast<const char*>(fp));

        auto* span = doc.new_obj();
        yyjson_mut_obj_add_int(doc.doc, span, "start_line", sqlite3_column_int(stmt, 4));
        yyjson_mut_obj_add_int(doc.doc, span, "start_col", sqlite3_column_int(stmt, 5));
        yyjson_mut_obj_add_int(doc.doc, span, "end_line", sqlite3_column_int(stmt, 6));
        yyjson_mut_obj_add_int(doc.doc, span, "end_col", sqlite3_column_int(stmt, 7));
        yyjson_mut_obj_add_val(doc.doc, item, "span", span);

        yyjson_mut_arr_append(results, item);
    }

    yyjson_mut_obj_add_val(doc.doc, root, "results", results);
    return doc.to_string();
}

// T069: file_summary
std::string file_summary(yyjson_val* params, Connection& conn,
                                 QueryCache& cache, const std::string& repo_root) {
    const char* path = params ? json_get_str(params, "path") : nullptr;
    if (!path && params) path = json_get_str(params, "file_path");
    if (!path) return McpError::invalid_input("Missing 'path' parameter").to_json_rpc(0);

    int64_t limit = params ? json_get_int(params, "limit", 200) : 200;
    int64_t offset = params ? json_get_int(params, "offset", 0) : 0;
    if (limit > 2000) limit = 2000;

    // Resolve the path: handles relative input in workspace mode (absolute paths in DB)
    std::string resolved = resolve_db_path(conn.raw(), path, repo_root);
    if (resolved.empty()) {
        if (!std::filesystem::path(path).is_absolute()) {
            auto validated = path_util::validate_mcp_path(path, repo_root);
            if (validated.empty())
                return McpError::invalid_input("Invalid path: rejected by traversal guard").to_json_rpc(0);
        }
        return McpError::not_found(std::string("File not found: ") + path).to_json_rpc(0);
    }

    auto* stmt = cache.get("file_by_path",
        "SELECT f.id, f.language FROM files f WHERE f.path = ?");
    sqlite3_bind_text(stmt, 1, resolved.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        return McpError::not_found(std::string("File not found: ") + path).to_json_rpc(0);
    }

    int64_t file_id = sqlite3_column_int64(stmt, 0);
    std::string language = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));

    // Use resolved path directly (absolute in workspace mode, repo-rooted otherwise)
    auto full = std::filesystem::path(resolved);
    int lines = 0;
    {
        std::ifstream f(full);
        std::string line;
        while (std::getline(f, line)) lines++;
    }

    auto* total_stmt = cache.get("file_symbols_count",
        "SELECT COUNT(*) FROM nodes WHERE file_id = ? AND node_type = 'symbol'");
    sqlite3_bind_int64(total_stmt, 1, file_id);
    int64_t total = 0;
    if (sqlite3_step(total_stmt) == SQLITE_ROW) total = sqlite3_column_int64(total_stmt, 0);

    auto* sym_stmt = cache.get("file_symbols",
        "SELECT id, kind, name, qualname, visibility, signature, start_line, end_line "
        "FROM nodes WHERE file_id = ? AND node_type = 'symbol' ORDER BY start_line, id LIMIT ? OFFSET ?");
    sqlite3_bind_int64(sym_stmt, 1, file_id);
    sqlite3_bind_int64(sym_stmt, 2, limit + 1);
    sqlite3_bind_int64(sym_stmt, 3, offset);

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);
    yyjson_mut_obj_add_strcpy(doc.doc, root, "path", resolved.c_str());
    yyjson_mut_obj_add_int(doc.doc, root, "lines", lines);
    yyjson_mut_obj_add_strcpy(doc.doc, root, "language", language.c_str());

    auto* results = doc.new_arr();
    auto* symbols = doc.new_arr();
    auto append_symbol = [&](yyjson_mut_val* arr) {
        auto* sym = doc.new_obj();
        yyjson_mut_obj_add_int(doc.doc, sym, "node_id", sqlite3_column_int64(sym_stmt, 0));
        yyjson_mut_obj_add_strcpy(doc.doc, sym, "kind",
            reinterpret_cast<const char*>(sqlite3_column_text(sym_stmt, 1)));
        yyjson_mut_obj_add_strcpy(doc.doc, sym, "name",
            reinterpret_cast<const char*>(sqlite3_column_text(sym_stmt, 2)));
        auto* qn = sqlite3_column_text(sym_stmt, 3);
        if (qn) yyjson_mut_obj_add_strcpy(doc.doc, sym, "qualname", reinterpret_cast<const char*>(qn));
        auto* vis = sqlite3_column_text(sym_stmt, 4);
        if (vis) yyjson_mut_obj_add_strcpy(doc.doc, sym, "visibility", reinterpret_cast<const char*>(vis));
        auto* sig = sqlite3_column_text(sym_stmt, 5);
        if (sig) yyjson_mut_obj_add_strcpy(doc.doc, sym, "signature", reinterpret_cast<const char*>(sig));

        auto* span = doc.new_obj();
        yyjson_mut_obj_add_int(doc.doc, span, "start_line", sqlite3_column_int(sym_stmt, 6));
        yyjson_mut_obj_add_int(doc.doc, span, "end_line", sqlite3_column_int(sym_stmt, 7));
        yyjson_mut_obj_add_val(doc.doc, sym, "span", span);

        yyjson_mut_arr_append(arr, sym);
    };

    int count = 0;
    bool has_more = false;
    while (sqlite3_step(sym_stmt) == SQLITE_ROW && count <= limit) {
        if (count == limit) { has_more = true; break; }
        count++;
        append_symbol(results);
        append_symbol(symbols);
    }

    add_pagination_fields(doc, root, results, total, has_more, offset, limit);
    yyjson_mut_obj_add_val(doc.doc, root, "symbols", symbols);
    return doc.to_string();
}

// T069b: file_overview
std::string file_overview(yyjson_val* params, Connection& conn,
                          QueryCache& cache, const std::string& repo_root) {
    const char* path = params ? json_get_str(params, "path") : nullptr;
    if (!path) return McpError::invalid_input("Missing 'path' parameter").to_json_rpc(0);

    std::string resolved = resolve_db_path(conn.raw(), path, repo_root);
    if (resolved.empty()) {
        if (!std::filesystem::path(path).is_absolute()) {
            auto validated = path_util::validate_mcp_path(path, repo_root);
            if (validated.empty())
                return McpError::invalid_input("Invalid path: rejected by traversal guard").to_json_rpc(0);
        }
        return McpError::not_found(std::string("File not found: ") + path).to_json_rpc(0);
    }

    auto* file_stmt = cache.get("file_overview_by_path",
        "SELECT f.id FROM files f WHERE f.path = ?");
    sqlite3_bind_text(file_stmt, 1, resolved.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(file_stmt) != SQLITE_ROW) {
        return McpError::not_found(std::string("File not found: ") + path).to_json_rpc(0);
    }

    int64_t file_id = sqlite3_column_int64(file_stmt, 0);

    auto* sym_stmt = cache.get("file_overview_symbols",
        "SELECT name, qualname, kind, start_line, end_line, signature, visibility, doc "
        "FROM nodes "
        "WHERE file_id = ? AND node_type = 'symbol' "
        "AND kind IN ('function', 'class', 'struct', 'interface', 'enum', 'type', 'macro', 'method', 'typedef', 'type_alias', 'constructor_fn') "
        "ORDER BY start_line, id");
    sqlite3_bind_int64(sym_stmt, 1, file_id);

    auto first_doc_line = [](const unsigned char* raw) -> std::string {
        if (!raw) return {};
        std::string doc = reinterpret_cast<const char*>(raw);
        auto newline = doc.find('\n');
        if (newline != std::string::npos) doc.resize(newline);
        auto first = doc.find_first_not_of(" \t\r");
        if (first == std::string::npos) return {};
        auto last = doc.find_last_not_of(" \t\r");
        return doc.substr(first, last - first + 1);
    };

    struct OverviewSymbol {
        std::string name;
        std::string qualname;
        std::string kind;
        int start_line = 0;
        int end_line = 0;
        std::string signature;
        std::string visibility;
        std::string doc;
    };

    std::vector<OverviewSymbol> raw_symbols;
    while (sqlite3_step(sym_stmt) == SQLITE_ROW) {
        OverviewSymbol sym;
        sym.name = reinterpret_cast<const char*>(sqlite3_column_text(sym_stmt, 0));
        if (const auto* qn = sqlite3_column_text(sym_stmt, 1)) {
            sym.qualname = reinterpret_cast<const char*>(qn);
        }
        sym.kind = reinterpret_cast<const char*>(sqlite3_column_text(sym_stmt, 2));
        if (sym.kind == "typedef" || sym.kind == "type_alias") sym.kind = "type";
        sym.start_line = sqlite3_column_int(sym_stmt, 3);
        sym.end_line = sqlite3_column_int(sym_stmt, 4);
        if (const auto* sig = sqlite3_column_text(sym_stmt, 5); sig && sig[0] != '\0') {
            sym.signature = reinterpret_cast<const char*>(sig);
        }
        if (const auto* vis = sqlite3_column_text(sym_stmt, 6); vis && vis[0] != '\0') {
            sym.visibility = reinterpret_cast<const char*>(vis);
        }
        sym.doc = first_doc_line(sqlite3_column_text(sym_stmt, 7));
        raw_symbols.push_back(std::move(sym));
    }

    std::vector<OverviewSymbol> symbols_filtered;
    for (const auto& sym : raw_symbols) {
        bool merged = false;
        for (auto& kept : symbols_filtered) {
            if (kept.name == sym.name && kept.qualname == sym.qualname && kept.kind == sym.kind
                && kept.start_line == sym.start_line) {
                if (sym.end_line > kept.end_line) {
                    kept.end_line = sym.end_line;
                    if (kept.signature.empty()) kept.signature = sym.signature;
                    if (kept.visibility.empty()) kept.visibility = sym.visibility;
                    if (kept.doc.empty()) kept.doc = sym.doc;
                }
                merged = true;
                break;
            }
        }
        if (!merged) symbols_filtered.push_back(sym);
    }

    std::vector<OverviewSymbol> final_symbols;
    for (const auto& sym : symbols_filtered) {
        bool nested_in_callable = false;
        for (const auto& kept : final_symbols) {
            if ((kept.kind == "function" || kept.kind == "method")
                && kept.start_line <= sym.start_line && kept.end_line >= sym.end_line
                && !(kept.name == sym.name && kept.qualname == sym.qualname
                     && kept.kind == sym.kind && kept.start_line == sym.start_line)) {
                nested_in_callable = true;
                break;
            }
        }
        if (!nested_in_callable) final_symbols.push_back(sym);
    }

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    auto* symbols = doc.new_arr();
    doc.set_root(root);

    yyjson_mut_obj_add_strcpy(doc.doc, root, "file", resolved.c_str());

    int64_t symbol_count = 0;
    bool any_signature = false;
    bool any_visibility = false;

    for (const auto& sym_row : final_symbols) {
        auto* sym = doc.new_obj();
        yyjson_mut_obj_add_strcpy(doc.doc, sym, "name", sym_row.name.c_str());
        if (!sym_row.qualname.empty()) yyjson_mut_obj_add_strcpy(doc.doc, sym, "qualname", sym_row.qualname.c_str());
        yyjson_mut_obj_add_strcpy(doc.doc, sym, "kind", sym_row.kind.c_str());

        int start_line = sym_row.start_line;
        int end_line = sym_row.end_line;
        yyjson_mut_obj_add_int(doc.doc, sym, "start_line", start_line);
        yyjson_mut_obj_add_int(doc.doc, sym, "end_line", end_line);
        yyjson_mut_obj_add_int(doc.doc, sym, "span_lines",
            (start_line > 0 && end_line >= start_line) ? (end_line - start_line + 1) : 0);

        if (!sym_row.signature.empty()) {
            any_signature = true;
            yyjson_mut_obj_add_strcpy(doc.doc, sym, "signature", sym_row.signature.c_str());
        }

        if (!sym_row.visibility.empty()) {
            any_visibility = true;
            yyjson_mut_obj_add_strcpy(doc.doc, sym, "visibility", sym_row.visibility.c_str());
        }

        if (!sym_row.doc.empty()) {
            yyjson_mut_obj_add_strcpy(doc.doc, sym, "doc", sym_row.doc.c_str());
        }

        yyjson_mut_arr_append(symbols, sym);
        symbol_count++;
    }

    yyjson_mut_obj_add_int(doc.doc, root, "symbol_count", symbol_count);
    yyjson_mut_obj_add_val(doc.doc, root, "symbols", symbols);
    if (symbol_count > 0 && !any_signature && !any_visibility) {
        yyjson_mut_obj_add_str(doc.doc, root, "_note",
            "signature/visibility fields are not yet populated by the extractor");
    }
    return doc.to_string();
}

// T068: context_for (one-shot symbol understanding)
std::string context_for(yyjson_val* params, Connection& conn,
                                QueryCache& cache, const std::string& repo_root) {
    int64_t node_id = resolve_node_id(params, conn, cache);
    if (node_id < 0) return McpError::invalid_input("Missing 'node_id'").to_json_rpc(0);

    bool include_source = params ? json_get_bool(params, "include_source", true) : true;
    int64_t max_source_lines = params ? json_get_int(params, "max_source_lines", 0) : 0;
    auto* max_callers_val = params ? yyjson_obj_get(params, "max_callers") : nullptr;
    auto* max_callees_val = params ? yyjson_obj_get(params, "max_callees") : nullptr;
    int64_t max_callers = max_callers_val ? yyjson_get_sint(max_callers_val) : 10;
    int64_t max_callees = max_callees_val ? yyjson_get_sint(max_callees_val) : 10;
    CandidateMode candidate_mode = parse_candidate_mode(params);
    CallsiteCandidateOptions candidate_options = parse_callsite_candidate_options(params);
    if (max_source_lines < 0)
        return McpError::invalid_input("Invalid 'max_source_lines': must be >= 0").to_json_rpc(0);
    if (max_callers < 0)
        return McpError::invalid_input("Invalid 'max_callers': must be >= 0").to_json_rpc(0);
    if (max_callees < 0)
        return McpError::invalid_input("Invalid 'max_callees': must be >= 0").to_json_rpc(0);

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);

    // Symbol info (inline from symbol_get query)
    auto* sym_stmt = cache.get("context_symbol",
        "SELECT n.id, n.kind, n.name, n.qualname, n.signature, f.path, "
        "n.start_line, n.end_line, n.doc "
        "FROM nodes n LEFT JOIN files f ON n.file_id = f.id "
        "WHERE n.id = ? AND n.node_type = 'symbol'");
    sqlite3_bind_int64(sym_stmt, 1, node_id);

    if (sqlite3_step(sym_stmt) != SQLITE_ROW) {
        return McpError::not_found("Symbol not found").to_json_rpc(0);
    }

    auto* symbol = doc.new_obj();
    yyjson_mut_obj_add_int(doc.doc, symbol, "node_id", sqlite3_column_int64(sym_stmt, 0));
    yyjson_mut_obj_add_strcpy(doc.doc, symbol, "kind",
        reinterpret_cast<const char*>(sqlite3_column_text(sym_stmt, 1)));
    yyjson_mut_obj_add_strcpy(doc.doc, symbol, "name",
        reinterpret_cast<const char*>(sqlite3_column_text(sym_stmt, 2)));
    auto* qn = sqlite3_column_text(sym_stmt, 3);
    if (qn) yyjson_mut_obj_add_strcpy(doc.doc, symbol, "qualname", reinterpret_cast<const char*>(qn));
    auto* sig = sqlite3_column_text(sym_stmt, 4);
    if (sig) yyjson_mut_obj_add_strcpy(doc.doc, symbol, "signature", reinterpret_cast<const char*>(sig));
    auto* fp = sqlite3_column_text(sym_stmt, 5);
    std::string file_path = fp ? reinterpret_cast<const char*>(fp) : "";
    if (!file_path.empty()) {
        yyjson_mut_obj_add_strcpy(doc.doc, symbol, "file_path", file_path.c_str());
        if (include_source) {
            int sl = sqlite3_column_int(sym_stmt, 6);
            int el = sqlite3_column_int(sym_stmt, 7);
            auto source = read_source_snippet(repo_root, file_path, sl, el);
            source = truncate_source_lines(source, max_source_lines);
            if (!source.empty()) yyjson_mut_obj_add_strcpy(doc.doc, symbol, "source", source.c_str());
        }
    }

    yyjson_mut_obj_add_val(doc.doc, root, "symbol", symbol);

    // Callers
    auto* callers_stmt = cache.get("context_callers",
        "SELECT e.src_id, n.name, f.path, e.confidence "
        "FROM edges e JOIN nodes n ON e.src_id = n.id "
        "LEFT JOIN files f ON n.file_id = f.id "
        "WHERE e.dst_id = ? AND e.kind = 'calls'");
    sqlite3_bind_int64(callers_stmt, 1, node_id);

    struct ContextCallerRow {
        std::string name;
        std::string file_path;
        double confidence = 0.0;
    };
    std::vector<ContextCallerRow> caller_rows;
    while (sqlite3_step(callers_stmt) == SQLITE_ROW) {
        ContextCallerRow row;
        row.name = reinterpret_cast<const char*>(sqlite3_column_text(callers_stmt, 1));
        auto* cfp = sqlite3_column_text(callers_stmt, 2);
        row.file_path = cfp ? reinterpret_cast<const char*>(cfp) : "";
        row.confidence = sqlite3_column_double(callers_stmt, 3);
        caller_rows.push_back(std::move(row));
    }
    bool exact_callers_empty = caller_rows.empty();
    bool callers_truncated = max_callers > 0 && static_cast<int64_t>(caller_rows.size()) > max_callers;
    if (callers_truncated) caller_rows.resize(static_cast<size_t>(max_callers));

    auto* callers_arr = doc.new_arr();
    for (const auto& row : caller_rows) {
        auto* c = doc.new_obj();
        yyjson_mut_obj_add_strcpy(doc.doc, c, "caller_name", row.name.c_str());
        if (!row.file_path.empty()) yyjson_mut_obj_add_strcpy(doc.doc, c, "file_path", row.file_path.c_str());
        yyjson_mut_obj_add_real(doc.doc, c, "confidence", row.confidence);
        yyjson_mut_arr_append(callers_arr, c);
    }
    yyjson_mut_obj_add_val(doc.doc, root, "callers", callers_arr);
    if (callers_truncated) yyjson_mut_obj_add_bool(doc.doc, root, "callers_truncated", true);
    if (should_collect_candidates(candidate_mode, exact_callers_empty)) {
        int64_t candidate_limit = max_callers > 0 ? max_callers : 500;
        auto candidates = collect_callsite_candidates(node_id, candidate_limit, conn, cache, candidate_options, repo_root);
        auto* candidate_callers = doc.new_arr();
        for (const auto& candidate : candidates.rows)
            yyjson_mut_arr_append(candidate_callers,
                emit_callsite_candidate(doc, candidate, candidate_options.include_handles));
        yyjson_mut_obj_add_val(doc.doc, root, "candidate_callers", candidate_callers);
        yyjson_mut_obj_add_str(doc.doc, root, "candidate_callers_source", "refs");
        add_callsite_candidate_metadata(doc, root, "candidate_callers", candidates, candidate_options);
    }

    // Callees
    auto* callees_stmt = cache.get("context_callees",
        "SELECT e.dst_id, n.name, n.qualname, n.signature, e.confidence "
        "FROM edges e JOIN nodes n ON e.dst_id = n.id "
        "WHERE e.src_id = ? AND e.kind = 'calls'");
    sqlite3_bind_int64(callees_stmt, 1, node_id);

    struct ContextCalleeRow {
        std::string name;
        std::string qualname;
        double confidence = 0.0;
    };
    std::vector<ContextCalleeRow> callee_rows;
    while (sqlite3_step(callees_stmt) == SQLITE_ROW) {
        ContextCalleeRow row;
        row.name = reinterpret_cast<const char*>(sqlite3_column_text(callees_stmt, 1));
        auto* cqn = sqlite3_column_text(callees_stmt, 2);
        row.qualname = cqn ? reinterpret_cast<const char*>(cqn) : "";
        row.confidence = sqlite3_column_double(callees_stmt, 4);
        callee_rows.push_back(std::move(row));
    }
    bool callees_truncated = max_callees > 0 && static_cast<int64_t>(callee_rows.size()) > max_callees;
    if (callees_truncated) callee_rows.resize(static_cast<size_t>(max_callees));

    auto* callees_arr = doc.new_arr();
    for (const auto& row : callee_rows) {
        auto* c = doc.new_obj();
        yyjson_mut_obj_add_strcpy(doc.doc, c, "callee_name", row.name.c_str());
        if (!row.qualname.empty()) yyjson_mut_obj_add_strcpy(doc.doc, c, "qualname", row.qualname.c_str());
        yyjson_mut_obj_add_real(doc.doc, c, "confidence", row.confidence);
        yyjson_mut_arr_append(callees_arr, c);
    }
    yyjson_mut_obj_add_val(doc.doc, root, "callees", callees_arr);
    if (callees_truncated) yyjson_mut_obj_add_bool(doc.doc, root, "callees_truncated", true);

    // Container info: enclosing type/namespace/module (via 'contains' edge where dst = node_id)
    {
        auto* cont_stmt = cache.get("context_container",
            "SELECT n.id, n.kind, n.name, n.qualname "
            "FROM edges e JOIN nodes n ON e.src_id = n.id "
            "WHERE e.dst_id = ? AND e.kind = 'contains' LIMIT 1");
        sqlite3_bind_int64(cont_stmt, 1, node_id);
        if (sqlite3_step(cont_stmt) == SQLITE_ROW) {
            auto* container = doc.new_obj();
            int64_t container_id = sqlite3_column_int64(cont_stmt, 0);
            yyjson_mut_obj_add_int(doc.doc, container, "node_id", container_id);
            yyjson_mut_obj_add_strcpy(doc.doc, container, "kind",
                reinterpret_cast<const char*>(sqlite3_column_text(cont_stmt, 1)));
            yyjson_mut_obj_add_strcpy(doc.doc, container, "name",
                reinterpret_cast<const char*>(sqlite3_column_text(cont_stmt, 2)));
            auto* cqn = sqlite3_column_text(cont_stmt, 3);
            if (cqn) yyjson_mut_obj_add_strcpy(doc.doc, container, "qualname",
                reinterpret_cast<const char*>(cqn));
            yyjson_mut_obj_add_val(doc.doc, root, "container", container);

            // Sibling members: other symbols contained by the same container
            auto* sib_stmt = cache.get("context_siblings",
                "SELECT n.id, n.kind, n.name, n.signature "
                "FROM edges e JOIN nodes n ON e.dst_id = n.id "
                "WHERE e.src_id = ? AND e.kind = 'contains' AND n.id != ? "
                "ORDER BY n.start_line LIMIT 30");
            sqlite3_bind_int64(sib_stmt, 1, container_id);
            sqlite3_bind_int64(sib_stmt, 2, node_id);

            auto* siblings_arr = doc.new_arr();
            while (sqlite3_step(sib_stmt) == SQLITE_ROW) {
                auto* s = doc.new_obj();
                yyjson_mut_obj_add_int(doc.doc, s, "node_id", sqlite3_column_int64(sib_stmt, 0));
                yyjson_mut_obj_add_strcpy(doc.doc, s, "kind",
                    reinterpret_cast<const char*>(sqlite3_column_text(sib_stmt, 1)));
                yyjson_mut_obj_add_strcpy(doc.doc, s, "name",
                    reinterpret_cast<const char*>(sqlite3_column_text(sib_stmt, 2)));
                auto* ssig = sqlite3_column_text(sib_stmt, 3);
                if (ssig) yyjson_mut_obj_add_strcpy(doc.doc, s, "signature",
                    reinterpret_cast<const char*>(ssig));
                yyjson_mut_arr_append(siblings_arr, s);
            }
            yyjson_mut_obj_add_val(doc.doc, root, "siblings", siblings_arr);

            // Base/implements: what types this container inherits from
            auto* base_stmt = cache.get("context_bases",
                "SELECT n.id, n.kind, n.name, n.qualname "
                "FROM edges e JOIN nodes n ON e.dst_id = n.id "
                "WHERE e.src_id = ? AND e.kind = 'inherits'");
            sqlite3_bind_int64(base_stmt, 1, container_id);

            auto* bases_arr = doc.new_arr();
            while (sqlite3_step(base_stmt) == SQLITE_ROW) {
                auto* b = doc.new_obj();
                yyjson_mut_obj_add_int(doc.doc, b, "node_id", sqlite3_column_int64(base_stmt, 0));
                yyjson_mut_obj_add_strcpy(doc.doc, b, "kind",
                    reinterpret_cast<const char*>(sqlite3_column_text(base_stmt, 1)));
                yyjson_mut_obj_add_strcpy(doc.doc, b, "name",
                    reinterpret_cast<const char*>(sqlite3_column_text(base_stmt, 2)));
                auto* bqn = sqlite3_column_text(base_stmt, 3);
                if (bqn) yyjson_mut_obj_add_strcpy(doc.doc, b, "qualname",
                    reinterpret_cast<const char*>(bqn));
                yyjson_mut_arr_append(bases_arr, b);
            }
            yyjson_mut_obj_add_val(doc.doc, root, "base_types", bases_arr);
        }
    }

    return doc.to_string();
}

// T068b: context_by_name — resolve by symbol name, then return context
std::string context_by_name(yyjson_val* params, Connection& conn,
                            QueryCache& cache, const std::string& repo_root) {
    const char* name = params ? json_get_str(params, "name") : nullptr;
    if (!name || std::strlen(name) == 0)
        return McpError::invalid_input("Missing 'name' parameter").to_json_rpc(0);

    const char* file_pattern = params ? json_get_str(params, "file_pattern") : nullptr;

    auto run_match_query = [&](bool prefix) -> std::vector<std::tuple<int64_t, std::string, std::string, std::string, std::string, int>> {
        std::string sql =
            "SELECT n.id, n.name, n.qualname, n.kind, f.path, n.start_line "
            "FROM nodes n "
            "LEFT JOIN files f ON n.file_id = f.id "
            "WHERE n.node_type = 'symbol' AND ";
        sql += prefix ? "n.name GLOB ?" : "n.name = ?";
        if (file_pattern && std::strlen(file_pattern) > 0) sql += " AND f.path GLOB ?";
        sql += " ORDER BY length(n.name), length(COALESCE(n.qualname, n.name)), f.path, n.start_line LIMIT 25";

        sqlite3_stmt* stmt = nullptr;
        std::vector<std::tuple<int64_t, std::string, std::string, std::string, std::string, int>> rows;
        if (sqlite3_prepare_v2(conn.raw(), sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            return rows;
        }

        int bind_idx = 1;
        std::string prefix_pat;
        if (prefix) prefix_pat = std::string(name) + "*";
        sqlite3_bind_text(stmt, bind_idx++, prefix ? prefix_pat.c_str() : name, -1, SQLITE_TRANSIENT);
        if (file_pattern && std::strlen(file_pattern) > 0)
            sqlite3_bind_text(stmt, bind_idx++, file_pattern, -1, SQLITE_TRANSIENT);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            auto* name_txt = sqlite3_column_text(stmt, 1);
            auto* qual_txt = sqlite3_column_text(stmt, 2);
            auto* kind_txt = sqlite3_column_text(stmt, 3);
            auto* file_txt = sqlite3_column_text(stmt, 4);
            rows.emplace_back(
                sqlite3_column_int64(stmt, 0),
                name_txt ? reinterpret_cast<const char*>(name_txt) : "",
                qual_txt ? reinterpret_cast<const char*>(qual_txt) : "",
                kind_txt ? reinterpret_cast<const char*>(kind_txt) : "",
                file_txt ? reinterpret_cast<const char*>(file_txt) : "",
                sqlite3_column_int(stmt, 5));
        }
        sqlite3_finalize(stmt);
        return rows;
    };

    auto exact_matches = run_match_query(false);
    auto prefix_matches = exact_matches.empty() ? run_match_query(true) : std::vector<std::tuple<int64_t, std::string, std::string, std::string, std::string, int>>();
    const auto& matches = exact_matches.empty() ? prefix_matches : exact_matches;

    if (matches.size() == 1) {
        JsonMutDoc param_doc;
        auto* root = param_doc.new_obj();
        param_doc.set_root(root);
        yyjson_mut_obj_add_int(param_doc.doc, root, "node_id", std::get<0>(matches[0]));

        auto parsed = json_parse(param_doc.to_string());
        return context_for(parsed.root(), conn, cache, repo_root);
    }

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);
    yyjson_mut_obj_add_bool(doc.doc, root, "ambiguous", !matches.empty());
    auto* candidates = doc.new_arr();

    for (const auto& match : matches) {
        auto* item = doc.new_obj();
        yyjson_mut_obj_add_int(doc.doc, item, "node_id", std::get<0>(match));
        yyjson_mut_obj_add_strcpy(doc.doc, item, "name", std::get<1>(match).c_str());
        if (!std::get<2>(match).empty())
            yyjson_mut_obj_add_strcpy(doc.doc, item, "qualname", std::get<2>(match).c_str());
        yyjson_mut_obj_add_strcpy(doc.doc, item, "kind", std::get<3>(match).c_str());
        if (!std::get<4>(match).empty())
            yyjson_mut_obj_add_strcpy(doc.doc, item, "file_path", std::get<4>(match).c_str());
        yyjson_mut_obj_add_int(doc.doc, item, "start_line", std::get<5>(match));
        yyjson_mut_arr_append(candidates, item);
    }

    yyjson_mut_obj_add_val(doc.doc, root, "candidates", candidates);
    return doc.to_string();
}

// ===== US3: Graph Exploration Tools =====

// T084: entrypoints — find natural starting points
std::string entrypoints(yyjson_val* params, Connection& conn,
                                QueryCache& cache, const std::string& /*repo_root*/) {
    int64_t limit = params ? json_get_int(params, "limit", 20) : 20;
    const char* scope = params ? json_get_str(params, "scope") : nullptr;

    // Build scope GLOB pattern if provided
    std::string scope_glob;
    if (scope && strlen(scope) > 0) {
        scope_glob = scope;
        // If scope doesn't contain a wildcard, treat it as a prefix
        if (scope_glob.find('*') == std::string::npos && scope_glob.find('?') == std::string::npos) {
            if (scope_glob.back() != '/') scope_glob += '/';
            scope_glob += '*';
        }
    }

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);
    auto* results = doc.new_arr();

    // 1. Main functions
    {
        std::string sql =
            "SELECT n.id, n.kind, n.name, f.path, n.start_line, n.end_line "
            "FROM nodes n LEFT JOIN files f ON n.file_id = f.id "
            "WHERE n.node_type = 'symbol' AND n.kind = 'function' "
            "AND (n.name = 'main' OR n.name = 'Main' OR n.name = 'wmain') ";
        std::string cache_key = "entrypoints_main";
        if (!scope_glob.empty()) {
            sql += "AND f.path GLOB ? ";
            cache_key += "_scoped";
        }
        sql += "LIMIT ?";

        auto* stmt = cache.get(cache_key, sql);
        int bind_idx = 1;
        if (!scope_glob.empty())
            sqlite3_bind_text(stmt, bind_idx++, scope_glob.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, bind_idx++, limit);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            auto* item = doc.new_obj();
            yyjson_mut_obj_add_int(doc.doc, item, "node_id", sqlite3_column_int64(stmt, 0));
            yyjson_mut_obj_add_strcpy(doc.doc, item, "kind",
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
            yyjson_mut_obj_add_strcpy(doc.doc, item, "name",
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
            auto* fp = sqlite3_column_text(stmt, 3);
            if (fp) yyjson_mut_obj_add_strcpy(doc.doc, item, "file_path", reinterpret_cast<const char*>(fp));
            auto* span = doc.new_obj();
            yyjson_mut_obj_add_int(doc.doc, span, "start_line", sqlite3_column_int(stmt, 4));
            yyjson_mut_obj_add_int(doc.doc, span, "end_line", sqlite3_column_int(stmt, 5));
            yyjson_mut_obj_add_val(doc.doc, item, "span", span);
            yyjson_mut_obj_add_str(doc.doc, item, "reason", "main_function");
            yyjson_mut_arr_append(results, item);
        }
    }

    // 2. High in-degree symbols (most referenced)
    {
        std::string sql;
        std::string cache_key;
        if (!scope_glob.empty()) {
            sql = "SELECT n.id, n.kind, n.name, f.path, n.start_line, n.end_line, top.cnt "
                  "FROM (SELECT dst_id, COUNT(*) as cnt FROM edges GROUP BY dst_id ORDER BY cnt DESC LIMIT ?) top "
                  "JOIN nodes n ON n.id = top.dst_id "
                  "LEFT JOIN files f ON n.file_id = f.id "
                  "WHERE n.node_type = 'symbol' AND f.path GLOB ? "
                  "ORDER BY top.cnt DESC";
            cache_key = "entrypoints_indegree_scoped";
        } else {
            sql = "SELECT n.id, n.kind, n.name, f.path, n.start_line, n.end_line, top.cnt "
                  "FROM (SELECT dst_id, COUNT(*) as cnt FROM edges GROUP BY dst_id ORDER BY cnt DESC LIMIT ?) top "
                  "JOIN nodes n ON n.id = top.dst_id "
                  "LEFT JOIN files f ON n.file_id = f.id "
                  "WHERE n.node_type = 'symbol' "
                  "ORDER BY top.cnt DESC";
            cache_key = "entrypoints_indegree";
        }

        auto* stmt = cache.get(cache_key, sql);
        int bind_idx = 1;
        sqlite3_bind_int64(stmt, bind_idx++, limit);
        if (!scope_glob.empty())
            sqlite3_bind_text(stmt, bind_idx++, scope_glob.c_str(), -1, SQLITE_TRANSIENT);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            auto* item = doc.new_obj();
            yyjson_mut_obj_add_int(doc.doc, item, "node_id", sqlite3_column_int64(stmt, 0));
            yyjson_mut_obj_add_strcpy(doc.doc, item, "kind",
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
            yyjson_mut_obj_add_strcpy(doc.doc, item, "name",
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
            auto* fp = sqlite3_column_text(stmt, 3);
            if (fp) yyjson_mut_obj_add_strcpy(doc.doc, item, "file_path", reinterpret_cast<const char*>(fp));
            auto* span = doc.new_obj();
            yyjson_mut_obj_add_int(doc.doc, span, "start_line", sqlite3_column_int(stmt, 4));
            yyjson_mut_obj_add_int(doc.doc, span, "end_line", sqlite3_column_int(stmt, 5));
            yyjson_mut_obj_add_val(doc.doc, item, "span", span);
            yyjson_mut_obj_add_str(doc.doc, item, "reason", "high_in_degree");
            yyjson_mut_arr_append(results, item);
        }
    }

    yyjson_mut_obj_add_val(doc.doc, root, "results", results);
    return doc.to_string();
}

// T085: impact_of — transitive dependents via BFS
std::string impact_of(yyjson_val* params, Connection& conn,
                              QueryCache& cache, const std::string& repo_root) {
    int64_t node_id = resolve_node_id(params, conn, cache);
    if (node_id < 0) return McpError::invalid_input("Missing 'node_id'").to_json_rpc(0);

    int64_t depth = params ? json_get_int(params, "depth", 2) : 2;
    if (depth > 3) depth = 3;
    int64_t max_nodes = params ? json_get_int(params, "max_nodes", 50) : 50;
    if (max_nodes > 200) max_nodes = 200;
    CandidateMode candidate_mode = parse_candidate_mode(params);
    CallsiteCandidateOptions candidate_options = parse_callsite_candidate_options(params);

    // BFS from node_id following reverse edges
    std::vector<int64_t> frontier = {node_id};
    std::unordered_set<int64_t> visited = {node_id};

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);

    // Symbol info
    auto* sym = doc.new_obj();
    {
        auto* stmt = cache.get("impact_symbol",
            "SELECT name, f.path FROM nodes n LEFT JOIN files f ON n.file_id = f.id WHERE n.id = ?");
        sqlite3_bind_int64(stmt, 1, node_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            yyjson_mut_obj_add_strcpy(doc.doc, sym, "name",
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
            auto* fp = sqlite3_column_text(stmt, 1);
            if (fp) yyjson_mut_obj_add_strcpy(doc.doc, sym, "file_path", reinterpret_cast<const char*>(fp));
        }
    }
    yyjson_mut_obj_add_val(doc.doc, root, "symbol", sym);

    auto* impacted = doc.new_arr();
    std::unordered_set<std::string> impacted_files;
    bool truncated = false;
    bool exact_seed_inbound_call_found = false;
    int total = 0;

    for (int d = 1; d <= depth && !truncated; ++d) {
        std::vector<int64_t> next_frontier;
        for (int64_t nid : frontier) {
            // Find nodes that depend on nid (reverse edges: dst_id = nid)
            auto* stmt = cache.get("impact_reverse",
                "SELECT e.src_id, n.name, f.path, e.kind, e.confidence "
                "FROM edges e JOIN nodes n ON e.src_id = n.id "
                "LEFT JOIN files f ON n.file_id = f.id "
                "WHERE e.dst_id = ? AND e.kind != 'contains'");
            sqlite3_bind_int64(stmt, 1, nid);

            while (sqlite3_step(stmt) == SQLITE_ROW) {
                int64_t src_id = sqlite3_column_int64(stmt, 0);
                auto* kind_txt = sqlite3_column_text(stmt, 3);
                const char* relationship = kind_txt ? reinterpret_cast<const char*>(kind_txt) : "";
                if (d == 1 && strcmp(relationship, "calls") == 0) exact_seed_inbound_call_found = true;
                if (visited.count(src_id)) continue;
                visited.insert(src_id);

                if (total >= max_nodes) { truncated = true; break; }
                total++;

                auto* item = doc.new_obj();
                yyjson_mut_obj_add_int(doc.doc, item, "node_id", src_id);
                yyjson_mut_obj_add_strcpy(doc.doc, item, "name",
                    reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
                auto* fp = sqlite3_column_text(stmt, 2);
                if (fp) {
                    std::string fpath(reinterpret_cast<const char*>(fp));
                    yyjson_mut_obj_add_strcpy(doc.doc, item, "file_path", fpath.c_str());
                    impacted_files.insert(fpath);
                }
                yyjson_mut_obj_add_strcpy(doc.doc, item, "relationship", relationship);
                yyjson_mut_obj_add_int(doc.doc, item, "distance", d);
                yyjson_mut_arr_append(impacted, item);

                next_frontier.push_back(src_id);
            }
            if (truncated) break;
        }
        frontier = std::move(next_frontier);
    }

    yyjson_mut_obj_add_val(doc.doc, root, "impacted", impacted);

    if (should_collect_candidates(candidate_mode, !exact_seed_inbound_call_found)) {
        auto candidates = collect_callsite_candidates(node_id, max_nodes, conn, cache, candidate_options, repo_root);
        auto* candidate_impacted = doc.new_arr();
        std::unordered_set<std::string> candidate_files;
        for (const auto& candidate : candidates.rows) {
            auto* item = emit_callsite_candidate(doc, candidate, candidate_options.include_handles);
            yyjson_mut_obj_add_str(doc.doc, item, "relationship", "calls_candidate");
            yyjson_mut_obj_add_int(doc.doc, item, "distance", 1);
            yyjson_mut_arr_append(candidate_impacted, item);
            if (!candidate.file_path.empty()) candidate_files.insert(candidate.file_path);
        }
        yyjson_mut_obj_add_val(doc.doc, root, "candidate_impacted", candidate_impacted);
        auto* candidate_files_arr = doc.new_arr();
        for (const auto& f : candidate_files)
            yyjson_mut_arr_add_strcpy(doc.doc, candidate_files_arr, f.c_str());
        yyjson_mut_obj_add_val(doc.doc, root, "candidate_impacted_files", candidate_files_arr);
        yyjson_mut_obj_add_str(doc.doc, root, "candidate_impacted_source", "refs");
        add_callsite_candidate_metadata(doc, root, "candidate_impacted", candidates, candidate_options);
    }

    auto* files_arr = doc.new_arr();
    for (auto& f : impacted_files) {
        yyjson_mut_arr_add_strcpy(doc.doc, files_arr, f.c_str());
    }
    yyjson_mut_obj_add_val(doc.doc, root, "impacted_files", files_arr);
    yyjson_mut_obj_add_bool(doc.doc, root, "truncated", truncated);

    return doc.to_string();
}

// T086: file_deps — include relationships for a file
std::string file_deps(yyjson_val* params, Connection& conn,
                              QueryCache& cache, const std::string& repo_root) {
    const char* path = params ? json_get_str(params, "path") : nullptr;
    if (!path) return McpError::invalid_input("Missing 'path'").to_json_rpc(0);

    std::string resolved = resolve_db_path(conn.raw(), path, repo_root);
    if (resolved.empty()) return McpError::invalid_input("Invalid path").to_json_rpc(0);

    // Find file node
    auto* file_stmt = cache.get("file_deps_file",
        "SELECT n.id FROM nodes n WHERE n.kind = 'file' AND n.name = ?");
    sqlite3_bind_text(file_stmt, 1, resolved.c_str(), -1, SQLITE_TRANSIENT);

    int64_t file_node_id = -1;
    if (sqlite3_step(file_stmt) == SQLITE_ROW) {
        file_node_id = sqlite3_column_int64(file_stmt, 0);
    }

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);
    yyjson_mut_obj_add_strcpy(doc.doc, root, "path", path);

    auto* includes = doc.new_arr();
    auto* included_by = doc.new_arr();

    if (file_node_id >= 0) {
        // Files this file includes (outgoing include edges)
        auto* out_stmt = cache.get("file_deps_out",
            "SELECT n.name FROM edges e JOIN nodes n ON e.dst_id = n.id "
            "WHERE e.src_id = ? AND e.kind = 'includes'");
        sqlite3_bind_int64(out_stmt, 1, file_node_id);
        while (sqlite3_step(out_stmt) == SQLITE_ROW) {
            yyjson_mut_arr_add_strcpy(doc.doc, includes,
                reinterpret_cast<const char*>(sqlite3_column_text(out_stmt, 0)));
        }

        // Files that include this file (incoming include edges)
        auto* in_stmt = cache.get("file_deps_in",
            "SELECT n.name FROM edges e JOIN nodes n ON e.src_id = n.id "
            "WHERE e.dst_id = ? AND e.kind = 'includes'");
        sqlite3_bind_int64(in_stmt, 1, file_node_id);
        while (sqlite3_step(in_stmt) == SQLITE_ROW) {
            yyjson_mut_arr_add_strcpy(doc.doc, included_by,
                reinterpret_cast<const char*>(sqlite3_column_text(in_stmt, 0)));
        }
    }

    yyjson_mut_obj_add_val(doc.doc, root, "includes", includes);
    yyjson_mut_obj_add_val(doc.doc, root, "included_by", included_by);
    return doc.to_string();
}

// T087: subgraph — multi-seed BFS extraction
std::string subgraph(yyjson_val* params, Connection& conn,
                             QueryCache& cache, const std::string& /*repo_root*/) {
    if (!params) return McpError::invalid_input("Missing parameters").to_json_rpc(0);

    auto* seeds_val = yyjson_obj_get(params, "seed_symbols");
    if (!seeds_val || !yyjson_is_arr(seeds_val))
        return McpError::invalid_input("Missing 'seed_symbols' array").to_json_rpc(0);

    int64_t depth = json_get_int(params, "depth", 1);
    if (depth > 5) depth = 5;
    int64_t max_nodes = json_get_int(params, "max_nodes", 200);
    if (max_nodes > 1000) max_nodes = 1000;

    // Parse optional edge_kinds filter
    std::unordered_set<std::string> allowed_kinds;
    bool has_edge_filter = false;
    auto* edge_kinds_val = yyjson_obj_get(params, "edge_kinds");
    if (edge_kinds_val) {
        has_edge_filter = true;
        if (yyjson_is_arr(edge_kinds_val)) {
            yyjson_val* ek_item;
            size_t ek_idx, ek_max;
            yyjson_arr_foreach(edge_kinds_val, ek_idx, ek_max, ek_item) {
                auto* s = yyjson_get_str(ek_item);
                if (s) allowed_kinds.insert(s);
            }
        } else if (yyjson_is_str(edge_kinds_val)) {
            auto* s = yyjson_get_str(edge_kinds_val);
            if (s) allowed_kinds.insert(s);
        }
    }

    std::vector<int64_t> frontier;
    yyjson_val* seed_item;
    size_t idx, max;
    yyjson_arr_foreach(seeds_val, idx, max, seed_item) {
        frontier.push_back(yyjson_get_sint(seed_item));
    }

    std::unordered_set<int64_t> visited_nodes(frontier.begin(), frontier.end());

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);
    auto* nodes_arr = doc.new_arr();
    auto* edges_arr = doc.new_arr();
    bool truncated = false;

    // Add seed nodes
    for (int64_t nid : frontier) {
        auto* stmt = cache.get("subgraph_node",
            "SELECT n.id, n.kind, n.name, f.path "
            "FROM nodes n LEFT JOIN files f ON n.file_id = f.id WHERE n.id = ?");
        sqlite3_bind_int64(stmt, 1, nid);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            auto* item = doc.new_obj();
            yyjson_mut_obj_add_int(doc.doc, item, "node_id", sqlite3_column_int64(stmt, 0));
            yyjson_mut_obj_add_strcpy(doc.doc, item, "kind",
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
            yyjson_mut_obj_add_strcpy(doc.doc, item, "name",
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
            auto* fp = sqlite3_column_text(stmt, 3);
            if (fp) yyjson_mut_obj_add_strcpy(doc.doc, item, "file_path", reinterpret_cast<const char*>(fp));
            yyjson_mut_arr_append(nodes_arr, item);
        }
    }

    // BFS
    for (int d = 0; d < depth && !truncated; ++d) {
        std::vector<int64_t> next;
        for (int64_t nid : frontier) {
            // Forward edges
            auto* fwd = cache.get("subgraph_fwd",
                "SELECT dst_id, kind, confidence FROM edges WHERE src_id = ?");
            sqlite3_bind_int64(fwd, 1, nid);
            while (sqlite3_step(fwd) == SQLITE_ROW) {
                int64_t dst = sqlite3_column_int64(fwd, 0);
                std::string edge_kind(reinterpret_cast<const char*>(sqlite3_column_text(fwd, 1)));

                // Filter by edge_kinds if specified
                if (has_edge_filter && !allowed_kinds.count(edge_kind)) continue;

                // Add edge
                auto* e = doc.new_obj();
                yyjson_mut_obj_add_int(doc.doc, e, "src_id", nid);
                yyjson_mut_obj_add_int(doc.doc, e, "dst_id", dst);
                yyjson_mut_obj_add_strcpy(doc.doc, e, "kind", edge_kind.c_str());
                yyjson_mut_obj_add_real(doc.doc, e, "confidence", sqlite3_column_double(fwd, 2));
                yyjson_mut_arr_append(edges_arr, e);

                if (!visited_nodes.count(dst)) {
                    if (static_cast<int64_t>(visited_nodes.size()) >= max_nodes) {
                        truncated = true; break;
                    }
                    visited_nodes.insert(dst);
                    next.push_back(dst);

                    // Add node
                    auto* ns = cache.get("subgraph_node",
                        "SELECT n.id, n.kind, n.name, f.path "
                        "FROM nodes n LEFT JOIN files f ON n.file_id = f.id WHERE n.id = ?");
                    sqlite3_bind_int64(ns, 1, dst);
                    if (sqlite3_step(ns) == SQLITE_ROW) {
                        auto* ni = doc.new_obj();
                        yyjson_mut_obj_add_int(doc.doc, ni, "node_id", sqlite3_column_int64(ns, 0));
                        yyjson_mut_obj_add_strcpy(doc.doc, ni, "kind",
                            reinterpret_cast<const char*>(sqlite3_column_text(ns, 1)));
                        yyjson_mut_obj_add_strcpy(doc.doc, ni, "name",
                            reinterpret_cast<const char*>(sqlite3_column_text(ns, 2)));
                        auto* fp = sqlite3_column_text(ns, 3);
                        if (fp) yyjson_mut_obj_add_strcpy(doc.doc, ni, "file_path",
                            reinterpret_cast<const char*>(fp));
                        yyjson_mut_arr_append(nodes_arr, ni);
                    }
                }
            }
            if (truncated) break;
        }
        frontier = std::move(next);
    }

    yyjson_mut_obj_add_val(doc.doc, root, "nodes", nodes_arr);
    yyjson_mut_obj_add_val(doc.doc, root, "edges", edges_arr);
    yyjson_mut_obj_add_bool(doc.doc, root, "truncated", truncated);
    return doc.to_string();
}

// T088: shortest_path — BFS between two nodes
std::string shortest_path(yyjson_val* params, Connection& conn,
                                  QueryCache& cache, const std::string& /*repo_root*/) {
    int64_t src_id = params ? json_get_int(params, "src_node_id", -1) : -1;
    int64_t dst_id = params ? json_get_int(params, "dst_node_id", -1) : -1;
    if (src_id < 0 || dst_id < 0)
        return McpError::invalid_input("Missing src_node_id or dst_node_id").to_json_rpc(0);

    int64_t max_depth = params ? json_get_int(params, "max_depth", 10) : 10;
    if (max_depth > 20) max_depth = 20;
    int64_t max_paths = params ? json_get_int(params, "max_paths", 1) : 1;
    if (max_paths > 5) max_paths = 5;
    if (max_paths < 1) max_paths = 1;

    // Parse optional relation_types filter
    std::unordered_set<std::string> allowed_relations;
    auto* rel_val = params ? yyjson_obj_get(params, "relation_types") : nullptr;
    if (rel_val) {
        if (yyjson_is_arr(rel_val)) {
            yyjson_val* rt_item;
            size_t rt_idx, rt_max;
            yyjson_arr_foreach(rel_val, rt_idx, rt_max, rt_item) {
                auto* s = yyjson_get_str(rt_item);
                if (s) allowed_relations.insert(s);
            }
        } else if (yyjson_is_str(rel_val)) {
            auto* s = yyjson_get_str(rel_val);
            if (s) allowed_relations.insert(s);
        }
    }

    // Helper: reconstruct a single path and emit as JSON array
    struct BfsNode { int64_t id; int64_t parent; std::string edge_kind; double confidence; };

    auto reconstruct_path = [&](const std::vector<BfsNode>& queue,
                                const std::unordered_map<int64_t, size_t>& visited_map,
                                JsonMutDoc& doc) -> std::pair<yyjson_mut_val*, int64_t> {
        std::vector<size_t> path_indices;
        size_t idx = visited_map.at(dst_id);
        while (idx != 0) {
            path_indices.push_back(idx);
            idx = static_cast<size_t>(queue[idx].parent);
        }
        path_indices.push_back(0);
        std::reverse(path_indices.begin(), path_indices.end());

        auto* path_arr = doc.new_arr();
        for (size_t i = 0; i < path_indices.size(); ++i) {
            auto& node = queue[path_indices[i]];

            auto* stmt = cache.get("sp_node",
                "SELECT n.kind, n.name FROM nodes n WHERE n.id = ?");
            sqlite3_bind_int64(stmt, 1, node.id);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                auto* ni = doc.new_obj();
                yyjson_mut_obj_add_int(doc.doc, ni, "node_id", node.id);
                yyjson_mut_obj_add_strcpy(doc.doc, ni, "kind",
                    reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
                yyjson_mut_obj_add_strcpy(doc.doc, ni, "name",
                    reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
                yyjson_mut_arr_append(path_arr, ni);
            }

            if (i + 1 < path_indices.size()) {
                auto& next = queue[path_indices[i + 1]];
                auto* ei = doc.new_obj();
                yyjson_mut_obj_add_strcpy(doc.doc, ei, "edge_kind", next.edge_kind.c_str());
                yyjson_mut_obj_add_real(doc.doc, ei, "confidence", next.confidence);
                yyjson_mut_arr_append(path_arr, ei);
            }
        }
        return {path_arr, static_cast<int64_t>(path_indices.size() - 1)};
    };

    // BFS to find shortest path(s)
    // For max_paths=1, use simple BFS; for >1, collect multiple paths via repeated BFS
    // with previously-used intermediate nodes excluded

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);

    // Collect sets of "blocked" intermediate nodes for path diversity
    std::unordered_set<int64_t> globally_blocked;
    auto* paths_arr = doc.new_arr();
    int paths_found = 0;

    for (int p = 0; p < max_paths; ++p) {
        std::vector<BfsNode> queue = {{src_id, -1, "", 0}};
        std::unordered_map<int64_t, size_t> visited;
        visited[src_id] = 0;
        size_t head = 0;
        bool found = false;

        while (head < queue.size() && !found) {
            auto& current = queue[head];
            if (static_cast<int64_t>(head) > max_depth * 100) break;

            int depth = 0;
            int64_t trace = static_cast<int64_t>(head);
            while (trace > 0) { trace = static_cast<int64_t>(queue[trace].parent); depth++; }
            if (depth >= max_depth) { head++; continue; }

            auto* stmt = cache.get("sp_edges",
                "SELECT dst_id, kind, confidence FROM edges WHERE src_id = ? "
                "UNION ALL "
                "SELECT src_id, kind, confidence FROM edges WHERE dst_id = ?");
            sqlite3_bind_int64(stmt, 1, current.id);
            sqlite3_bind_int64(stmt, 2, current.id);

            while (sqlite3_step(stmt) == SQLITE_ROW) {
                int64_t next_id = sqlite3_column_int64(stmt, 0);
                if (visited.count(next_id)) continue;

                // Block intermediate nodes from previous paths (not src/dst)
                if (next_id != dst_id && globally_blocked.count(next_id)) continue;

                std::string kind(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));

                // Filter by relation_types if specified
                if (!allowed_relations.empty() && !allowed_relations.count(kind)) continue;

                double conf = sqlite3_column_double(stmt, 2);

                size_t new_idx = queue.size();
                queue.push_back({next_id, static_cast<int64_t>(head), kind, conf});
                visited[next_id] = new_idx;

                if (next_id == dst_id) { found = true; break; }
            }
            head++;
        }

        if (!found) break;

        auto [path_arr, distance] = reconstruct_path(queue, visited, doc);

        if (max_paths == 1) {
            // Single path: backward-compatible output
            yyjson_mut_obj_add_val(doc.doc, root, "path", path_arr);
            yyjson_mut_obj_add_int(doc.doc, root, "distance", distance);
            return doc.to_string();
        }

        // Multi-path: add to paths array
        auto* path_obj = doc.new_obj();
        yyjson_mut_obj_add_val(doc.doc, path_obj, "path", path_arr);
        yyjson_mut_obj_add_int(doc.doc, path_obj, "distance", distance);
        yyjson_mut_arr_append(paths_arr, path_obj);
        paths_found++;

        // Block intermediate nodes from this path for diversity
        size_t idx = visited[dst_id];
        while (idx != 0) {
            auto& node = queue[idx];
            if (node.id != src_id && node.id != dst_id) {
                globally_blocked.insert(node.id);
            }
            idx = static_cast<size_t>(node.parent);
        }
    }

    if (max_paths > 1) {
        yyjson_mut_obj_add_val(doc.doc, root, "paths", paths_arr);
        yyjson_mut_obj_add_int(doc.doc, root, "paths_found", paths_found);
    } else {
        // No path found, single-path mode
        auto* empty_arr = doc.new_arr();
        yyjson_mut_obj_add_val(doc.doc, root, "path", empty_arr);
        yyjson_mut_obj_add_int(doc.doc, root, "distance", 0);
    }

    return doc.to_string();
}

// T089: find_implementations — find types that inherit from a base
std::string find_implementations(yyjson_val* params, Connection& conn,
                                         QueryCache& cache, const std::string& /*repo_root*/) {
    const char* symbol = params ? json_get_str(params, "symbol") : nullptr;
    if (!symbol) return McpError::invalid_input("Missing 'symbol' parameter").to_json_rpc(0);

    int64_t limit = params ? json_get_int(params, "limit", 50) : 50;
    if (limit > 500) limit = 500;

    // First, find the base type node(s) by name
    auto* name_stmt = cache.get("find_impl_base",
        "SELECT id FROM nodes WHERE name = ? AND node_type = 'symbol' "
        "AND kind IN ('class', 'struct', 'interface', 'type', 'trait') LIMIT 10");
    sqlite3_bind_text(name_stmt, 1, symbol, -1, SQLITE_TRANSIENT);

    std::vector<int64_t> base_ids;
    while (sqlite3_step(name_stmt) == SQLITE_ROW) {
        base_ids.push_back(sqlite3_column_int64(name_stmt, 0));
    }

    if (base_ids.empty()) {
        return McpError::not_found(std::string("No base type found: ") + symbol).to_json_rpc(0);
    }

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);
    yyjson_mut_obj_add_strcpy(doc.doc, root, "base_symbol", symbol);
    auto* results = doc.new_arr();

    std::unordered_set<int64_t> seen;
    for (int64_t base_id : base_ids) {
        auto* stmt = cache.get("find_impl_inherits",
            "SELECT n.id, n.name, n.qualname, n.kind, n.signature, f.path "
            "FROM edges e JOIN nodes n ON e.src_id = n.id "
            "LEFT JOIN files f ON n.file_id = f.id "
            "WHERE e.dst_id = ? AND e.kind = 'inherits' "
            "LIMIT ?");
        sqlite3_bind_int64(stmt, 1, base_id);
        sqlite3_bind_int64(stmt, 2, limit);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            int64_t nid = sqlite3_column_int64(stmt, 0);
            if (!seen.insert(nid).second) continue;

            auto* item = doc.new_obj();
            yyjson_mut_obj_add_int(doc.doc, item, "node_id", nid);
            yyjson_mut_obj_add_strcpy(doc.doc, item, "name",
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
            auto* qn = sqlite3_column_text(stmt, 2);
            if (qn) yyjson_mut_obj_add_strcpy(doc.doc, item, "qualname",
                reinterpret_cast<const char*>(qn));
            yyjson_mut_obj_add_strcpy(doc.doc, item, "kind",
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)));
            auto* sig = sqlite3_column_text(stmt, 4);
            if (sig) yyjson_mut_obj_add_strcpy(doc.doc, item, "signature",
                reinterpret_cast<const char*>(sig));
            auto* fp = sqlite3_column_text(stmt, 5);
            if (fp) yyjson_mut_obj_add_strcpy(doc.doc, item, "file_path",
                reinterpret_cast<const char*>(fp));
            yyjson_mut_arr_append(results, item);
        }
    }

    yyjson_mut_obj_add_val(doc.doc, root, "results", results);
    return doc.to_string();
}

// T090: method_fields — field accesses and calls made by a method
std::string method_fields(yyjson_val* params, Connection& conn,
                                  QueryCache& cache, const std::string& /*repo_root*/) {
    int64_t node_id = resolve_node_id(params, conn, cache);
    if (node_id < 0) return McpError::invalid_input("Missing 'node_id'").to_json_rpc(0);

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);
    yyjson_mut_obj_add_int(doc.doc, root, "node_id", node_id);

    // Method name
    {
        auto* name_stmt = cache.get("method_fields_name",
            "SELECT name FROM nodes WHERE id = ?");
        sqlite3_bind_int64(name_stmt, 1, node_id);
        if (sqlite3_step(name_stmt) == SQLITE_ROW) {
            yyjson_mut_obj_add_strcpy(doc.doc, root, "method_name",
                reinterpret_cast<const char*>(sqlite3_column_text(name_stmt, 0)));
        }
    }

    // --- Field accesses (this.X) ---
    // Group by name: collect read/write counts and earliest line
    {
        struct FieldEntry {
            int reads = 0;
            int writes = 0;
            int first_line = INT_MAX;
        };
        std::unordered_map<std::string, FieldEntry> field_map;
        // preserve insertion order for stable output
        std::vector<std::string> field_order;

        auto* fa_stmt = cache.get("method_fields_fa",
            "SELECT r.name, r.evidence, r.start_line "
            "FROM refs r "
            "WHERE r.containing_node_id = ? AND r.kind = 'field_access' "
            "ORDER BY r.name, r.start_line");
        sqlite3_bind_int64(fa_stmt, 1, node_id);

        while (sqlite3_step(fa_stmt) == SQLITE_ROW) {
            std::string name(reinterpret_cast<const char*>(sqlite3_column_text(fa_stmt, 0)));
            auto* ev_raw = sqlite3_column_text(fa_stmt, 1);
            std::string ev = ev_raw ? reinterpret_cast<const char*>(ev_raw) : "";
            int line = sqlite3_column_int(fa_stmt, 2);

            if (!field_map.count(name)) {
                field_order.push_back(name);
                field_map[name] = {};
            }
            auto& fe = field_map[name];
            if (ev == "this_member_write") fe.writes++;
            else fe.reads++;  // this_member_read or unknown
            if (line < fe.first_line) fe.first_line = line;
        }

        auto* fields_arr = doc.new_arr();
        for (const auto& fname : field_order) {
            const auto& fe = field_map[fname];
            auto* item = doc.new_obj();
            yyjson_mut_obj_add_strcpy(doc.doc, item, "name", fname.c_str());
            yyjson_mut_obj_add_int(doc.doc, item, "reads", fe.reads);
            yyjson_mut_obj_add_int(doc.doc, item, "writes", fe.writes);
            if (fe.first_line != INT_MAX)
                yyjson_mut_obj_add_int(doc.doc, item, "first_line", fe.first_line);
            yyjson_mut_arr_append(fields_arr, item);
        }
        yyjson_mut_obj_add_val(doc.doc, root, "fields", fields_arr);
    }

    // --- Calls from this method: classify as calls_self vs calls_external ---
    // calls_self = callee name matches a symbol contained by the same parent class
    {
        struct CallEntry {
            int count = 0;
            int first_line = INT_MAX;
            std::string relationship;
        };
        std::unordered_map<std::string, CallEntry> call_map;
        std::vector<std::string> call_order;

        auto* call_stmt = cache.get("method_fields_calls",
            "SELECT r.name, r.start_line, "
            "CASE WHEN EXISTS("
            "  SELECT 1 FROM edges e1 JOIN edges e2 ON e1.src_id = e2.src_id"
            "  JOIN nodes sib ON e2.dst_id = sib.id"
            "  WHERE e1.dst_id = ? AND e1.kind = 'contains'"
            "  AND e2.kind = 'contains' AND sib.name = "
            "  CASE WHEN r.name LIKE 'this.%' AND INSTR(SUBSTR(r.name,6),'.')=0"
            "  THEN SUBSTR(r.name,6) ELSE r.name END"
            ") THEN 'calls_self' ELSE 'calls_external' END AS relationship "
            "FROM refs r "
            "WHERE r.containing_node_id = ? AND r.kind = 'call' "
            "ORDER BY r.name, r.start_line");
        sqlite3_bind_int64(call_stmt, 1, node_id);
        sqlite3_bind_int64(call_stmt, 2, node_id);

        while (sqlite3_step(call_stmt) == SQLITE_ROW) {
            std::string name(reinterpret_cast<const char*>(sqlite3_column_text(call_stmt, 0)));
            int line = sqlite3_column_int(call_stmt, 1);
            std::string rel(reinterpret_cast<const char*>(sqlite3_column_text(call_stmt, 2)));

            if (!call_map.count(name)) {
                call_order.push_back(name);
                call_map[name] = {};
                call_map[name].relationship = rel;
            }
            auto& ce = call_map[name];
            ce.count++;
            if (line < ce.first_line) ce.first_line = line;
        }

        auto* calls_arr = doc.new_arr();
        for (const auto& cname : call_order) {
            const auto& ce = call_map[cname];
            auto* item = doc.new_obj();
            yyjson_mut_obj_add_strcpy(doc.doc, item, "name", cname.c_str());
            yyjson_mut_obj_add_strcpy(doc.doc, item, "relationship", ce.relationship.c_str());
            yyjson_mut_obj_add_int(doc.doc, item, "count", ce.count);
            if (ce.first_line != INT_MAX)
                yyjson_mut_obj_add_int(doc.doc, item, "first_line", ce.first_line);
            yyjson_mut_arr_append(calls_arr, item);
        }
        yyjson_mut_obj_add_val(doc.doc, root, "calls", calls_arr);
    }

    return doc.to_string();
}

// T091: dependency_cluster — group methods by shared field access with read/write weighting
std::string dependency_cluster(yyjson_val* params, Connection& conn,
                               QueryCache& cache, const std::string& /*repo_root*/) {
    // Accept either file path or class node_id
    auto* path_val = params ? yyjson_obj_get(params, "path") : nullptr;
    int64_t class_id = params ? json_get_int(params, "class_id", -1) : -1;

    if (!path_val && class_id < 0)
        return McpError::invalid_input("Provide 'path' (file path) or 'class_id'").to_json_rpc(0);

    // Resolve file_id
    int64_t file_id = -1;
    if (path_val) {
        std::string path(yyjson_get_str(path_val));
        auto* f_stmt = cache.get("dc_file", "SELECT id FROM files WHERE path = ?");
        sqlite3_bind_text(f_stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(f_stmt) == SQLITE_ROW) file_id = sqlite3_column_int64(f_stmt, 0);
        else return McpError::invalid_input("File not found: " + path).to_json_rpc(0);
    }

    // Get all methods in the file/class
    struct MethodInfo {
        int64_t id;
        std::string name;
        int lines;
    };
    std::vector<MethodInfo> methods;
    {
        std::string sql = class_id >= 0
            ? "SELECT n.id, n.name, n.end_line - n.start_line + 1 FROM nodes n "
              "JOIN edges e ON e.dst_id = n.id WHERE e.src_id = ? AND e.kind = 'contains' "
              "AND n.kind = 'method' ORDER BY n.start_line"
            : "SELECT n.id, n.name, n.end_line - n.start_line + 1 FROM nodes n "
              "WHERE n.file_id = ? AND n.kind = 'method' ORDER BY n.start_line";
        auto* stmt = cache.get("dc_methods", sql.c_str());
        sqlite3_bind_int64(stmt, 1, class_id >= 0 ? class_id : file_id);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            methods.push_back({
                sqlite3_column_int64(stmt, 0),
                std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1))),
                sqlite3_column_int(stmt, 2)
            });
        }
    }

    // Build method name set for filtering self-method refs
    std::unordered_set<std::string> method_names;
    for (const auto& m : methods) method_names.insert(m.name);

    // Build method→{field→(reads,writes)} map
    struct FieldAccess { int reads = 0; int writes = 0; };
    std::unordered_map<int64_t, std::unordered_map<std::string, FieldAccess>> method_field_map;

    auto* fa_stmt = cache.get("dc_fa",
        "SELECT r.containing_node_id, r.name, r.evidence FROM refs r "
        "WHERE r.file_id = ? AND r.kind = 'field_access'");
    sqlite3_bind_int64(fa_stmt, 1, file_id >= 0 ? file_id : 0);
    // If using class_id, we need file_id from any method
    if (file_id < 0 && !methods.empty()) {
        sqlite3_reset(fa_stmt);
        auto* fid_stmt = cache.get("dc_fid", "SELECT file_id FROM nodes WHERE id = ?");
        sqlite3_bind_int64(fid_stmt, 1, methods[0].id);
        if (sqlite3_step(fid_stmt) == SQLITE_ROW) file_id = sqlite3_column_int64(fid_stmt, 0);
        sqlite3_bind_int64(fa_stmt, 1, file_id);
    }

    while (sqlite3_step(fa_stmt) == SQLITE_ROW) {
        int64_t containing = sqlite3_column_int64(fa_stmt, 0);
        std::string field(reinterpret_cast<const char*>(sqlite3_column_text(fa_stmt, 1)));
        auto* ev = sqlite3_column_text(fa_stmt, 2);
        std::string evidence = ev ? std::string(reinterpret_cast<const char*>(ev)) : "";

        if (method_names.count(field)) continue; // skip self-method refs

        auto& fa = method_field_map[containing][field];
        if (evidence == "this_member_write") fa.writes++;
        else fa.reads++;
    }

    // Compute weighted coupling between method pairs
    // Weight: read-only=0.3, write-only=1.0, read+write=1.5
    auto field_weight = [](const FieldAccess& fa) -> double {
        if (fa.writes > 0 && fa.reads > 0) return 1.5;
        if (fa.writes > 0) return 1.0;
        return 0.3;
    };

    // For each method, compute total write count (extractability signal)
    struct MethodScore {
        int total_reads = 0;
        int total_writes = 0;
        int field_count = 0;
        double extractability = 0.0; // 0=deeply coupled, 1=pure read
    };
    std::unordered_map<int64_t, MethodScore> scores;
    for (const auto& m : methods) {
        auto it = method_field_map.find(m.id);
        if (it == method_field_map.end()) continue;
        auto& ms = scores[m.id];
        for (const auto& [field, fa] : it->second) {
            ms.total_reads += fa.reads;
            ms.total_writes += fa.writes;
            ms.field_count++;
        }
        int total = ms.total_reads + ms.total_writes;
        ms.extractability = total > 0 ? static_cast<double>(ms.total_reads) / total : 1.0;
    }

    // Greedy clustering by weighted Jaccard on field access
    std::vector<bool> assigned(methods.size(), false);
    struct Cluster {
        std::vector<size_t> members; // indices into methods
        std::set<std::string> shared_fields;
        std::set<std::string> all_fields;
        double avg_extractability = 0.0;
    };
    std::vector<Cluster> clusters;

    for (size_t i = 0; i < methods.size(); ++i) {
        if (assigned[i]) continue;
        auto it_i = method_field_map.find(methods[i].id);
        if (it_i == method_field_map.end() || it_i->second.size() < 2) {
            // Singleton — only cluster if ≥2 fields
            Cluster c;
            c.members.push_back(i);
            if (it_i != method_field_map.end()) {
                for (const auto& [f, _] : it_i->second) c.all_fields.insert(f);
                c.shared_fields = c.all_fields;
            }
            auto sit = scores.find(methods[i].id);
            c.avg_extractability = sit != scores.end() ? sit->second.extractability : 1.0;
            clusters.push_back(std::move(c));
            assigned[i] = true;
            continue;
        }

        Cluster c;
        c.members.push_back(i);
        assigned[i] = true;

        for (size_t j = i + 1; j < methods.size(); ++j) {
            if (assigned[j]) continue;
            auto it_j = method_field_map.find(methods[j].id);
            if (it_j == method_field_map.end()) continue;

            // Weighted Jaccard
            std::set<std::string> all_fields;
            for (const auto& [f, _] : it_i->second) all_fields.insert(f);
            for (const auto& [f, _] : it_j->second) all_fields.insert(f);

            double intersection_w = 0, union_w = 0;
            for (const auto& f : all_fields) {
                auto fi = it_i->second.find(f);
                auto fj = it_j->second.find(f);
                double wi = fi != it_i->second.end() ? field_weight(fi->second) : 0;
                double wj = fj != it_j->second.end() ? field_weight(fj->second) : 0;
                union_w += std::max(wi, wj);
                if (wi > 0 && wj > 0) intersection_w += std::min(wi, wj);
            }
            double sim = union_w > 0 ? intersection_w / union_w : 0;
            if (sim >= 0.20) {
                c.members.push_back(j);
                assigned[j] = true;
            }
        }

        // Compute shared/all fields
        for (size_t mi : c.members) {
            auto it = method_field_map.find(methods[mi].id);
            if (it != method_field_map.end())
                for (const auto& [f, _] : it->second) c.all_fields.insert(f);
        }
        if (c.members.size() > 1) {
            c.shared_fields = c.all_fields; // start with all, intersect
            for (size_t mi : c.members) {
                std::set<std::string> mf;
                auto it = method_field_map.find(methods[mi].id);
                if (it != method_field_map.end())
                    for (const auto& [f, _] : it->second) mf.insert(f);
                std::set<std::string> isect;
                std::set_intersection(c.shared_fields.begin(), c.shared_fields.end(),
                                      mf.begin(), mf.end(), std::inserter(isect, isect.begin()));
                c.shared_fields = isect;
            }
        } else {
            c.shared_fields = c.all_fields;
        }

        // Average extractability
        double sum = 0;
        for (size_t mi : c.members) {
            auto sit = scores.find(methods[mi].id);
            sum += sit != scores.end() ? sit->second.extractability : 1.0;
        }
        c.avg_extractability = c.members.empty() ? 0 : sum / c.members.size();

        clusters.push_back(std::move(c));
    }

    // Sort clusters: most extractable first
    std::sort(clusters.begin(), clusters.end(), [](const Cluster& a, const Cluster& b) {
        return a.avg_extractability > b.avg_extractability;
    });

    // Build response
    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);
    yyjson_mut_obj_add_int(doc.doc, root, "method_count", static_cast<int>(methods.size()));
    yyjson_mut_obj_add_int(doc.doc, root, "cluster_count", static_cast<int>(clusters.size()));

    auto* clusters_arr = doc.new_arr();
    for (const auto& c : clusters) {
        auto* cobj = doc.new_obj();

        auto* members_arr = doc.new_arr();
        for (size_t mi : c.members) {
            auto* mobj = doc.new_obj();
            yyjson_mut_obj_add_int(doc.doc, mobj, "node_id", methods[mi].id);
            yyjson_mut_obj_add_strcpy(doc.doc, mobj, "name", methods[mi].name.c_str());
            yyjson_mut_obj_add_int(doc.doc, mobj, "lines", methods[mi].lines);
            auto sit = scores.find(methods[mi].id);
            if (sit != scores.end()) {
                yyjson_mut_obj_add_int(doc.doc, mobj, "reads", sit->second.total_reads);
                yyjson_mut_obj_add_int(doc.doc, mobj, "writes", sit->second.total_writes);
                yyjson_mut_obj_add_real(doc.doc, mobj, "extractability", sit->second.extractability);
            }
            yyjson_mut_arr_append(members_arr, mobj);
        }
        yyjson_mut_obj_add_val(doc.doc, cobj, "members", members_arr);

        auto* shared_arr = doc.new_arr();
        for (const auto& f : c.shared_fields)
            yyjson_mut_arr_add_strcpy(doc.doc, shared_arr, f.c_str());
        yyjson_mut_obj_add_val(doc.doc, cobj, "shared_fields", shared_arr);

        yyjson_mut_obj_add_int(doc.doc, cobj, "total_fields", static_cast<int>(c.all_fields.size()));
        yyjson_mut_obj_add_real(doc.doc, cobj, "avg_extractability", c.avg_extractability);

        yyjson_mut_arr_append(clusters_arr, cobj);
    }
    yyjson_mut_obj_add_val(doc.doc, root, "clusters", clusters_arr);

    return doc.to_string();
}

// T092: source_at — read raw source lines from a file
std::string source_at(yyjson_val* params, Connection& conn,
                      QueryCache& /*cache*/, const std::string& repo_root) {
    const char* path = params ? json_get_str(params, "path") : nullptr;
    if (!path) return McpError::invalid_input("Missing 'path' parameter").to_json_rpc(0);

    int64_t start_line = params ? json_get_int(params, "start_line", -1) : -1;
    int64_t end_line = params ? json_get_int(params, "end_line", -1) : -1;

    if (start_line <= 0 || end_line <= 0 || end_line < start_line) {
        return McpError::invalid_input("Invalid parameters: start_line and end_line must be positive and end_line >= start_line.").to_json_rpc(0);
    }

    if (end_line - start_line > 500) {
        return McpError::invalid_input("Line range too large. Maximum 500 lines per request.").to_json_rpc(0);
    }

    // Resolve relative paths via DB (handles workspace mode with absolute paths)
    std::string resolved_path = resolve_db_path(conn.raw(), path, repo_root);
    if (resolved_path.empty()) {
        if (!std::filesystem::path(path).is_absolute()) {
            auto validated = path_util::validate_mcp_path(path, repo_root);
            if (validated.empty()) return McpError::invalid_input("Invalid path: rejected by traversal guard").to_json_rpc(0);
            resolved_path = validated;
        } else {
            return McpError::not_found(std::string("File not found: ") + path).to_json_rpc(0);
        }
    }

    auto source = read_source_snippet(repo_root, resolved_path, static_cast<int>(start_line), static_cast<int>(end_line));
    if (source.empty()) {
        return McpError::not_found(std::string("Could not read file or line range is out of bounds: ") + path).to_json_rpc(0);
    }

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);
    yyjson_mut_obj_add_strcpy(doc.doc, root, "path", path);
    yyjson_mut_obj_add_int(doc.doc, root, "start_line", start_line);
    yyjson_mut_obj_add_int(doc.doc, root, "end_line", end_line);
    yyjson_mut_obj_add_strcpy(doc.doc, root, "source", source.c_str());

    return doc.to_string();
}

// T093: code_search — search source file contents using per-line trigram FTS index.
// Queries content_fts which stores one row per source line with file_id and line_no.
// MATCH returns exact line numbers directly — no full-file scanning needed.
std::string code_search(yyjson_val* params, Connection& conn,
                        QueryCache& cache, const std::string& repo_root) {
    const char* query = params ? json_get_str(params, "query") : nullptr;
    if (!query || std::strlen(query) == 0) {
        return McpError::invalid_input("Missing 'query' parameter").to_json_rpc(0);
    }

    // Trigram FTS requires at least 3 characters for a meaningful match
    if (std::strlen(query) < 3) {
        return McpError::invalid_input("Query must be at least 3 characters for content search").to_json_rpc(0);
    }

    int limit = params ? static_cast<int>(json_get_int(params, "limit", 20)) : 20;
    if (limit > 100) limit = 100;
    if (limit < 1) limit = 1;

    int context_lines = params ? static_cast<int>(json_get_int(params, "context_lines", 0)) : 0;
    if (context_lines > 5) context_lines = 5;
    if (context_lines < 0) context_lines = 0;

    int64_t max_bytes = params ? json_get_int(params, "max_bytes", 16000) : 16000;
    if (max_bytes < 0) max_bytes = 0;
    if (max_bytes > 100000) max_bytes = 100000;

    bool case_sensitive = params ? json_get_bool(params, "case_sensitive", false) : false;

    const char* file_pattern = params ? json_get_str(params, "file_pattern") : nullptr;

    // Query content_fts — each row is a single source line with file_id + line_no.
    std::string sql;
    if (file_pattern) {
        sql = "SELECT cf.file_id, cf.line_no, f.path "
              "FROM content_fts cf "
              "JOIN files f ON f.id = cf.file_id "
              "WHERE cf.content MATCH ? AND f.path GLOB ? "
              "ORDER BY cf.file_id, cf.line_no";
    } else {
        sql = "SELECT cf.file_id, cf.line_no, f.path "
              "FROM content_fts cf "
              "JOIN files f ON f.id = cf.file_id "
              "WHERE cf.content MATCH ? "
              "ORDER BY cf.file_id, cf.line_no";
    }

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(conn.raw(), sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return McpError::db_error(
            std::string("FTS query error: ") + sqlite3_errmsg(conn.raw())
        ).to_json_rpc(0);
    }

    std::string match_expr = "\"" + std::string(query) + "\"";
    sqlite3_bind_text(stmt, 1, match_expr.c_str(), -1, SQLITE_TRANSIENT);
    if (file_pattern) {
        sqlite3_bind_text(stmt, 2, file_pattern, -1, SQLITE_TRANSIENT);
    }

    // Collect matching lines grouped by file (stop after `limit` distinct files)
    struct FileHit {
        std::string path;
        std::vector<int> line_nos;
    };
    std::vector<FileHit> file_hits;
    int64_t current_file_id = -1;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t fid = sqlite3_column_int64(stmt, 0);
        int line_no = sqlite3_column_int(stmt, 1);
        auto* p = sqlite3_column_text(stmt, 2);

        if (fid != current_file_id) {
            if (static_cast<int>(file_hits.size()) >= limit) break;
            file_hits.push_back({p ? reinterpret_cast<const char*>(p) : "", {}});
            current_file_id = fid;
        }
        file_hits.back().line_nos.push_back(line_no);
    }
    sqlite3_finalize(stmt);

    // For case-insensitive post-filtering
    std::string query_str(query);
    std::string query_lower;
    if (!case_sensitive) {
        query_lower.reserve(query_str.size());
        for (char c : query_str) query_lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    JsonMutDoc doc;
    auto* root = doc.new_obj();
    doc.set_root(root);
    auto* results_arr = doc.new_arr();
    int total_matches = 0;
    int files_returned = 0;
    bool budget_exceeded = false;
    size_t approx_bytes = 384;

    for (auto& fh : file_hits) {
        if (budget_exceeded) break;
        auto fh_path = std::filesystem::path(fh.path);
        auto full_path = fh_path.is_absolute() ? fh_path : std::filesystem::path(repo_root) / fh.path;
        std::ifstream f(full_path);
        if (!f) continue;

        // Determine the range of lines we need to read
        int max_line_needed = 0;
        for (int ln : fh.line_nos) {
            int upper = ln + context_lines;
            if (upper > max_line_needed) max_line_needed = upper;
        }

        // Build a set of matched line numbers for O(1) lookup
        std::unordered_set<int> match_set(fh.line_nos.begin(), fh.line_nos.end());

        // Read only the lines we need (stop after max_line_needed)
        std::vector<std::pair<int, std::string>> needed_lines; // (line_no, text)
        int min_line_needed = fh.line_nos.empty() ? 1 : fh.line_nos.front() - context_lines;
        if (min_line_needed < 1) min_line_needed = 1;

        std::string line;
        int ln = 0;
        while (std::getline(f, line)) {
            ln++;
            if (ln > max_line_needed) break;
            if (ln >= min_line_needed) {
                needed_lines.push_back({ln, line});
            }
        }

        // Build line lookup
        std::unordered_map<int, std::string> line_map;
        for (auto& [num, text] : needed_lines) {
            line_map[num] = std::move(text);
        }

        // Filter matches: verify substring is actually present (case-sensitivity)
        struct LineMatch { int line_num; std::string line_text; };
        std::vector<LineMatch> verified;
        for (int match_ln : fh.line_nos) {
            auto it = line_map.find(match_ln);
            if (it == line_map.end()) continue;
            const auto& text = it->second;

            bool found;
            if (case_sensitive) {
                found = text.find(query_str) != std::string::npos;
            } else {
                std::string text_lower;
                text_lower.reserve(text.size());
                for (char c : text) text_lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                found = text_lower.find(query_lower) != std::string::npos;
            }
            if (found) {
                verified.push_back({match_ln, text});
            }
        }

        if (verified.empty()) continue;

        auto* file_obj = doc.new_obj();
        yyjson_mut_obj_add_strcpy(doc.doc, file_obj, "file", fh.path.c_str());
        yyjson_mut_obj_add_int(doc.doc, file_obj, "match_count", static_cast<int>(verified.size()));

        auto* matches_arr = doc.new_arr();
        int max_per_file = 20;
        int shown = 0;
        for (const auto& lm : verified) {
            if (shown >= max_per_file) break;

            std::string ctx;
            if (context_lines > 0) {
                int ctx_start = std::max(1, lm.line_num - context_lines);
                int ctx_end = lm.line_num + context_lines;
                for (int i = ctx_start; i <= ctx_end; i++) {
                    auto it = line_map.find(i);
                    if (it == line_map.end()) continue;
                    if (!ctx.empty()) ctx += "\n";
                    ctx += std::to_string(i) + ": " + it->second;
                }
            }

            size_t match_bytes = 48 + lm.line_text.size() + ctx.size();
            if (max_bytes > 0 && (files_returned > 0 || shown > 0) &&
                approx_bytes + match_bytes > static_cast<size_t>(max_bytes)) {
                budget_exceeded = true;
                break;
            }

            shown++;
            approx_bytes += match_bytes + 1;

            auto* match_obj = doc.new_obj();
            yyjson_mut_obj_add_int(doc.doc, match_obj, "line", lm.line_num);
            if (context_lines > 0) {
                yyjson_mut_obj_add_strcpy(doc.doc, match_obj, "context", ctx.c_str());
            } else {
                yyjson_mut_obj_add_strcpy(doc.doc, match_obj, "text", lm.line_text.c_str());
            }

            yyjson_mut_arr_append(matches_arr, match_obj);
        }

        if (shown == 0 && budget_exceeded) break;
        yyjson_mut_obj_add_val(doc.doc, file_obj, "matches", matches_arr);
        if (static_cast<int>(verified.size()) > max_per_file || budget_exceeded) {
            yyjson_mut_obj_add_bool(doc.doc, file_obj, "truncated", true);
        }
        yyjson_mut_arr_append(results_arr, file_obj);
        files_returned++;
        approx_bytes += 48 + fh.path.size();
        total_matches += static_cast<int>(verified.size());
    }

    yyjson_mut_obj_add_val(doc.doc, root, "results", results_arr);
    yyjson_mut_obj_add_int(doc.doc, root, "total_files", static_cast<int>(file_hits.size()));
    yyjson_mut_obj_add_int(doc.doc, root, "files_returned", files_returned);
    yyjson_mut_obj_add_int(doc.doc, root, "total_matches", total_matches);
    yyjson_mut_obj_add_int(doc.doc, root, "max_bytes", max_bytes);
    if (budget_exceeded) yyjson_mut_obj_add_bool(doc.doc, root, "truncated", true);

    return doc.to_string();
}

} // namespace tools
} // namespace codetopo

// Workspace tool implementations — in a separate section to keep the main tools.cpp clean.
#include "db/workspace.h"
#include "util/repo.h"

namespace codetopo {
namespace tools {

std::string workspace_add(yyjson_val* params, Connection& /*conn*/,
                          QueryCache& cache, const std::string& repo_root) {
    auto* path_val = params ? yyjson_obj_get(params, "path") : nullptr;
    if (!path_val || !yyjson_get_str(path_val)) {
        return R"({"error":"missing required parameter: path"})";
    }
    std::string target_path = yyjson_get_str(path_val);

    if (!std::filesystem::exists(target_path)) {
        return R"({"error":"path does not exist: )" + target_path + R"("})";
    }

    try {
        auto main_db = default_db(repo_root);
        ensure_codetopo_dir(repo_root);

        if (!std::filesystem::exists(main_db)) {
            return R"({"error":"index.sqlite not found — run 'codetopo index' first"})";
        }

        Config cfg;
        cfg.repo_root = target_path;
        cfg.thread_count = 0;
        cfg.arena_size_mb = 128;
        cfg.max_file_size_kb = 10240;
        cfg.parse_timeout_s = 5;
        cfg.turbo = false;

        WorkspaceDB ws(main_db);
        auto result = ws.add_root(target_path, cfg);

        cache.clear();

        JsonMutDoc doc;
        auto* root = doc.new_obj();
        doc.set_root(root);
        yyjson_mut_obj_add_int(doc.doc, root, "root_id", result.root_id);
        yyjson_mut_obj_add_int(doc.doc, root, "file_count", result.files);
        yyjson_mut_obj_add_int(doc.doc, root, "symbol_count", result.symbols);
        yyjson_mut_obj_add_int(doc.doc, root, "edge_count", result.edges);
        yyjson_mut_obj_add_str(doc.doc, root, "status", "added");
        return doc.to_string();
    } catch (const std::exception& e) {
        return std::string(R"({"error":")") + e.what() + R"("})";
    }
}

std::string workspace_remove(yyjson_val* params, Connection& /*conn*/,
                             QueryCache& cache, const std::string& repo_root) {
    auto* path_val = params ? yyjson_obj_get(params, "path") : nullptr;
    if (!path_val || !yyjson_get_str(path_val)) {
        return R"({"error":"missing required parameter: path"})";
    }
    std::string target_path = yyjson_get_str(path_val);

    try {
        auto main_db = default_db(repo_root);
        if (!std::filesystem::exists(main_db)) {
            return R"({"error":"index.sqlite not found"})";
        }

        WorkspaceDB ws(main_db);
        auto result = ws.remove_root(target_path);

        cache.clear();

        JsonMutDoc doc;
        auto* root = doc.new_obj();
        doc.set_root(root);
        yyjson_mut_obj_add_int(doc.doc, root, "files_removed", result.files);
        yyjson_mut_obj_add_int(doc.doc, root, "symbols_removed", result.symbols);
        yyjson_mut_obj_add_int(doc.doc, root, "edges_removed", result.edges);
        yyjson_mut_obj_add_str(doc.doc, root, "status", "removed");
        return doc.to_string();
    } catch (const std::exception& e) {
        return std::string(R"({"error":")") + e.what() + R"("})";
    }
}

std::string workspace_list(yyjson_val* /*params*/, Connection& /*conn*/,
                           QueryCache& /*cache*/, const std::string& repo_root) {
    try {
        auto main_db = default_db(repo_root);

        JsonMutDoc doc;
        auto* root = doc.new_obj();
        doc.set_root(root);
        auto* arr = doc.new_arr();

        if (!std::filesystem::exists(main_db)) {
            yyjson_mut_obj_add_val(doc.doc, root, "roots", arr);
            return doc.to_string();
        }

        WorkspaceDB ws(main_db);
        auto roots = ws.list_roots();

        for (const auto& r : roots) {
            auto* item = doc.new_obj();
            yyjson_mut_obj_add_int(doc.doc, item, "root_id", r.id);
            yyjson_mut_obj_add_strcpy(doc.doc, item, "path", r.path.c_str());
            yyjson_mut_obj_add_int(doc.doc, item, "file_count", r.files);
            yyjson_mut_obj_add_int(doc.doc, item, "symbol_count", r.symbols);
            yyjson_mut_obj_add_int(doc.doc, item, "edge_count", r.edges);
            yyjson_mut_arr_append(arr, item);
        }

        yyjson_mut_obj_add_val(doc.doc, root, "roots", arr);
        return doc.to_string();
    } catch (const std::exception& e) {
        return std::string(R"({"error":")") + e.what() + R"("})";
    }
}

} // namespace tools
} // namespace codetopo
