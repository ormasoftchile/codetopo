#pragma once

#include <yyjson.h>
#include <string>
#include <iostream>
#include <cstdint>

namespace codetopo {

// T016: yyjson wrapper helpers for MCP server I/O.

// RAII wrapper for yyjson_doc (read)
struct JsonDoc {
    yyjson_doc* doc = nullptr;

    explicit JsonDoc(yyjson_doc* d) : doc(d) {}
    ~JsonDoc() { if (doc) yyjson_doc_free(doc); }
    JsonDoc(const JsonDoc&) = delete;
    JsonDoc& operator=(const JsonDoc&) = delete;
    JsonDoc(JsonDoc&& o) noexcept : doc(o.doc) { o.doc = nullptr; }

    yyjson_val* root() { return doc ? yyjson_doc_get_root(doc) : nullptr; }
    explicit operator bool() const { return doc != nullptr; }
};

// RAII wrapper for yyjson_mut_doc (write)
struct JsonMutDoc {
    yyjson_mut_doc* doc = nullptr;

    JsonMutDoc() : doc(yyjson_mut_doc_new(nullptr)) {}
    ~JsonMutDoc() { if (doc) yyjson_mut_doc_free(doc); }
    JsonMutDoc(const JsonMutDoc&) = delete;
    JsonMutDoc& operator=(const JsonMutDoc&) = delete;

    yyjson_mut_val* new_obj() { return yyjson_mut_obj(doc); }
    yyjson_mut_val* new_arr() { return yyjson_mut_arr(doc); }
    yyjson_mut_val* new_str(const char* s) { return yyjson_mut_str(doc, s); }
    yyjson_mut_val* new_strcpy(const char* s) { return yyjson_mut_strcpy(doc, s); }
    yyjson_mut_val* new_int(int64_t v) { return yyjson_mut_sint(doc, v); }
    yyjson_mut_val* new_uint(uint64_t v) { return yyjson_mut_uint(doc, v); }
    yyjson_mut_val* new_real(double v) { return yyjson_mut_real(doc, v); }
    yyjson_mut_val* new_bool(bool v) { return yyjson_mut_bool(doc, v); }
    yyjson_mut_val* new_null() { return yyjson_mut_null(doc); }

    void set_root(yyjson_mut_val* val) { yyjson_mut_doc_set_root(doc, val); }

    // Serialize to string (caller owns the result via free)
    std::string to_string() {
        size_t len = 0;
        char* json = yyjson_mut_write(doc, 0, &len);
        if (!json) return "";
        std::string result(json, len);
        free(json);
        return result;
    }
};

// Parse a JSON string
inline JsonDoc json_parse(const std::string& input) {
    return JsonDoc(yyjson_read(input.c_str(), input.size(), 0));
}

// Read a line from stdin (for NDJSON MCP protocol)
inline std::string json_read_line() {
    std::string line;
    if (!std::getline(std::cin, line)) return "";
    return line;
}

// Write a JSON string to stdout followed by newline (NDJSON)
inline void json_write_line(const std::string& json) {
    std::cout << json << "\n";
    std::cout.flush();
}

// Helper: get string from object
inline const char* json_get_str(yyjson_val* obj, const char* key) {
    yyjson_val* val = yyjson_obj_get(obj, key);
    return val ? yyjson_get_str(val) : nullptr;
}

// Helper: get int from object
inline int64_t json_get_int(yyjson_val* obj, const char* key, int64_t def = 0) {
    yyjson_val* val = yyjson_obj_get(obj, key);
    return val ? yyjson_get_sint(val) : def;
}

// Helper: get bool from object
inline bool json_get_bool(yyjson_val* obj, const char* key, bool def = false) {
    yyjson_val* val = yyjson_obj_get(obj, key);
    return val ? yyjson_get_bool(val) : def;
}

} // namespace codetopo
