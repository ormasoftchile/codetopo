#pragma once

#include <xxhash.h>
#include <string>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <filesystem>

namespace codetopo {

// T013: xxHash64 content hashing wrapper.
inline std::string hash_file(const std::filesystem::path& file_path) {
    std::ifstream f(file_path, std::ios::binary);
    if (!f) return "";

    XXH3_state_t* state = XXH3_createState();
    XXH3_64bits_reset(state);

    char buf[8192];
    while (f.read(buf, sizeof(buf)) || f.gcount() > 0) {
        XXH3_64bits_update(state, buf, static_cast<size_t>(f.gcount()));
    }

    XXH64_hash_t hash = XXH3_64bits_digest(state);
    XXH3_freeState(state);

    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << hash;
    return oss.str();
}

inline std::string hash_string(const std::string& data) {
    XXH64_hash_t hash = XXH3_64bits(data.data(), data.size());
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << hash;
    return oss.str();
}

} // namespace codetopo
