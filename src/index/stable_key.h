#pragma once

#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>

namespace codetopo {

// T038: Stable key generation — <relpath>::<kind>::<qualname_or_name>
// with collision ordinals for overloaded functions.

inline std::string make_stable_key(const std::string& relpath,
                                    const std::string& kind,
                                    const std::string& qualname_or_name) {
    return relpath + "::" + kind + "::" + qualname_or_name;
}

// File node special case
inline std::string make_file_stable_key(const std::string& relpath) {
    return relpath + "::file";
}

// Given a list of base keys for symbols in a file, resolve collisions
// by appending #2, #3, etc. for duplicates.
// Input: vector of (base_key, start_line) pairs, sorted by start_line.
// Output: vector of final stable_keys in the same order.
struct KeyCandidate {
    std::string base_key;
    int start_line;
};

inline std::vector<std::string> resolve_collisions(std::vector<KeyCandidate>& candidates) {
    // Sort by start_line to assign ordinals in source order
    std::sort(candidates.begin(), candidates.end(),
              [](const KeyCandidate& a, const KeyCandidate& b) {
                  return a.start_line < b.start_line;
              });

    // Count occurrences of each base_key
    std::vector<std::string> result;
    result.reserve(candidates.size());

    // Track how many times we've seen each key
    std::unordered_map<std::string, int> seen;

    for (auto& c : candidates) {
        int ordinal = ++seen[c.base_key];
        if (ordinal == 1) {
            result.push_back(c.base_key);
        } else {
            result.push_back(c.base_key + "#" + std::to_string(ordinal));
        }
    }

    return result;
}

} // namespace codetopo
