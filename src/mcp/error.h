#pragma once

#include "util/json.h"
#include <string>
#include <cstdint>

namespace codetopo {

// T017: Error envelope matching JSON-RPC 2.0 format from contracts/mcp-tools.md.
struct McpError {
    int json_rpc_code;
    std::string error_code;  // not_found, invalid_input, query_timeout, db_error, limit_exceeded
    std::string message;

    static McpError not_found(const std::string& msg) {
        return {-32000, "not_found", msg};
    }
    static McpError invalid_input(const std::string& msg) {
        return {-32602, "invalid_input", msg};
    }
    static McpError query_timeout(const std::string& msg) {
        return {-32001, "query_timeout", msg};
    }
    static McpError db_error(const std::string& msg) {
        return {-32002, "db_error", msg};
    }
    static McpError limit_exceeded(const std::string& msg) {
        return {-32003, "limit_exceeded", msg};
    }

    // Serialize to JSON-RPC error response
    std::string to_json_rpc(int64_t id) {
        JsonMutDoc doc;
        auto* root = doc.new_obj();
        doc.set_root(root);

        yyjson_mut_obj_add_str(doc.doc, root, "jsonrpc", "2.0");
        yyjson_mut_obj_add_int(doc.doc, root, "id", id);

        auto* err = doc.new_obj();
        yyjson_mut_obj_add_int(doc.doc, err, "code", json_rpc_code);
        yyjson_mut_obj_add_strcpy(doc.doc, err, "message", message.c_str());

        auto* data = doc.new_obj();
        yyjson_mut_obj_add_strcpy(doc.doc, data, "error_code", error_code.c_str());
        yyjson_mut_obj_add_val(doc.doc, err, "data", data);

        yyjson_mut_obj_add_val(doc.doc, root, "error", err);

        return doc.to_string();
    }
};

} // namespace codetopo
