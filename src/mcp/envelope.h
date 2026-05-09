#pragma once
// v2-Tokens Phase 1: MCP response envelope utilities
// Provides _meta block construction, cursor encoding/decoding, and ranking metadata.

#include "util/json.h"
#include <string>
#include <cstdint>
#include <vector>

namespace codetopo {
namespace mcp {

// Cursor state for pagination.
// Encodes: ranking seed (if any) + offset into ranked result list.
// Cursors are opaque base64 strings — callers pass them back to get next page.
struct Cursor {
    int64_t offset = 0;
    int32_t ranking_seed = 0; // Future: for randomized tie-breaking

    // Encode cursor to opaque base64 string
    std::string encode() const;
    
    // Decode cursor from opaque string. Returns true on success.
    static bool decode(const std::string& cursor_str, Cursor& out);
};

// _meta block fields for v2-Tokens envelope
struct MetaBlock {
    int64_t tokens_estimated = 0;      // Estimated tokens in response (heuristic)
    int64_t tokens_budget = -1;        // -1 = unbounded, else the max_tokens param
    bool truncated = false;            // true if results were cut to fit budget
    std::string next_cursor;           // opaque string for pagination (empty if no more)
    std::string ranking_algorithm;     // e.g. "heuristic-v1", "pagerank-v1"
    std::vector<std::string> ranking_signals; // e.g. ["visibility", "proximity", "term_frequency"]

    // Add _meta object to a JSON document root.
    // The root must be a yyjson_mut_obj.
    void add_to_json(yyjson_mut_doc* doc, yyjson_mut_val* root) const;
};

// Helper: extract max_tokens param from MCP request params (default: -1 = unbounded)
int64_t get_max_tokens(yyjson_val* params);

// Helper: extract cursor param from MCP request params (empty string if absent)
std::string get_cursor_param(yyjson_val* params);

} // namespace mcp
} // namespace codetopo
