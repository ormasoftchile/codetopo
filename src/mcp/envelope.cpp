// v2-Tokens Phase 1: MCP response envelope implementation

#include "mcp/envelope.h"
#include "util/json.h"
#include <cstring>
#include <sstream>
#include <iomanip>

// Simple base64 encoding/decoding (no external dependency)
namespace {

static const char base64_chars[] = 
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const uint8_t* data, size_t len) {
    std::string ret;
    int i = 0;
    uint8_t char_array_3[3], char_array_4[4];
    
    while (len--) {
        char_array_3[i++] = *(data++);
        if (i == 3) {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;
            for (i = 0; i < 4; i++) ret += base64_chars[char_array_4[i]];
            i = 0;
        }
    }
    if (i) {
        for (int j = i; j < 3; j++) char_array_3[j] = '\0';
        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
        for (int j = 0; j < i + 1; j++) ret += base64_chars[char_array_4[j]];
        while (i++ < 3) ret += '=';
    }
    return ret;
}

std::vector<uint8_t> base64_decode(const std::string& encoded_string) {
    auto in_len = encoded_string.size();
    int i = 0, in_ = 0;
    uint8_t char_array_4[4], char_array_3[3];
    std::vector<uint8_t> ret;
    
    while (in_len-- && (encoded_string[in_] != '=')) {
        if (!isalnum(encoded_string[in_]) && encoded_string[in_] != '+' && encoded_string[in_] != '/') break;
        char_array_4[i++] = encoded_string[in_]; in_++;
        if (i == 4) {
            for (i = 0; i < 4; i++) {
                const char* pos = strchr(base64_chars, char_array_4[i]);
                char_array_4[i] = pos ? static_cast<uint8_t>(pos - base64_chars) : 0;
            }
            char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
            char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
            char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];
            for (i = 0; i < 3; i++) ret.push_back(char_array_3[i]);
            i = 0;
        }
    }
    if (i) {
        for (int j = 0; j < i; j++) {
            const char* pos = strchr(base64_chars, char_array_4[j]);
            char_array_4[j] = pos ? static_cast<uint8_t>(pos - base64_chars) : 0;
        }
        char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
        char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
        for (int j = 0; j < i - 1; j++) ret.push_back(char_array_3[j]);
    }
    return ret;
}

} // anonymous namespace

namespace codetopo {
namespace mcp {

std::string Cursor::encode() const {
    // Simple encoding: offset as 8 bytes + seed as 4 bytes
    uint8_t buf[12];
    // Little-endian encoding (portable across platforms)
    for (int i = 0; i < 8; ++i) buf[i] = (offset >> (i * 8)) & 0xFF;
    for (int i = 0; i < 4; ++i) buf[8 + i] = (ranking_seed >> (i * 8)) & 0xFF;
    return base64_encode(buf, 12);
}

bool Cursor::decode(const std::string& cursor_str, Cursor& out) {
    if (cursor_str.empty()) return false;
    auto buf = base64_decode(cursor_str);
    if (buf.size() != 12) return false;
    
    out.offset = 0;
    for (int i = 0; i < 8; ++i) {
        out.offset |= (static_cast<int64_t>(buf[i]) << (i * 8));
    }
    out.ranking_seed = 0;
    for (int i = 0; i < 4; ++i) {
        out.ranking_seed |= (static_cast<int32_t>(buf[8 + i]) << (i * 8));
    }
    return true;
}

void MetaBlock::add_to_json(yyjson_mut_doc* doc, yyjson_mut_val* root) const {
    auto* meta = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc, meta, "tokens_estimated", tokens_estimated);
    
    if (tokens_budget >= 0) {
        yyjson_mut_obj_add_int(doc, meta, "tokens_budget", tokens_budget);
    } else {
        yyjson_mut_obj_add_null(doc, meta, "tokens_budget");
    }
    
    yyjson_mut_obj_add_bool(doc, meta, "truncated", truncated);
    
    if (!next_cursor.empty()) {
        yyjson_mut_obj_add_strcpy(doc, meta, "next_cursor", next_cursor.c_str());
    } else {
        yyjson_mut_obj_add_null(doc, meta, "next_cursor");
    }
    
    // Add ranking metadata
    auto* ranking = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, ranking, "algorithm", ranking_algorithm.c_str());
    
    auto* signals = yyjson_mut_arr(doc);
    for (const auto& sig : ranking_signals) {
        yyjson_mut_arr_add_strcpy(doc, signals, sig.c_str());
    }
    yyjson_mut_obj_add_val(doc, ranking, "signals", signals);
    yyjson_mut_obj_add_val(doc, meta, "ranking", ranking);
    
    yyjson_mut_obj_add_val(doc, root, "_meta", meta);
}

int64_t get_max_tokens(yyjson_val* params) {
    if (!params) return -1;
    auto* val = yyjson_obj_get(params, "max_tokens");
    if (!val || yyjson_is_null(val)) return -1;
    if (yyjson_is_int(val)) {
        int64_t mt = yyjson_get_int(val);
        return mt > 0 ? mt : -1;
    }
    return -1;
}

std::string get_cursor_param(yyjson_val* params) {
    if (!params) return "";
    auto* val = yyjson_obj_get(params, "cursor");
    if (!val || !yyjson_is_str(val)) return "";
    const char* str = yyjson_get_str(val);
    return str ? std::string(str) : "";
}

} // namespace mcp
} // namespace codetopo
