#pragma once

#include "util/process.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace codetopo {

namespace semantic {

inline bool is_ascii_upper(unsigned char c) {
    return c >= 'A' && c <= 'Z';
}

inline bool is_ascii_lower(unsigned char c) {
    return c >= 'a' && c <= 'z';
}

inline char to_ascii_lower(unsigned char c) {
    if (is_ascii_upper(c)) return static_cast<char>(c - 'A' + 'a');
    return static_cast<char>(c);
}

inline std::string lower_ascii(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (unsigned char c : value) lowered.push_back(to_ascii_lower(c));
    return lowered;
}

inline bool camel_should_split(unsigned char prev, unsigned char curr, unsigned char next) {
    return (is_ascii_lower(prev) && is_ascii_upper(curr)) ||
           (is_ascii_upper(prev) && is_ascii_upper(curr) && next != 0 && is_ascii_lower(next));
}

inline std::vector<std::string> split_identifier_pieces(std::string_view value) {
    std::vector<std::string> pieces;
    std::string current;
    current.reserve(value.size());

    auto flush = [&]() {
        if (!current.empty()) {
            pieces.push_back(lower_ascii(current));
            current.clear();
        }
    };

    for (size_t i = 0; i < value.size(); ++i) {
        unsigned char curr = static_cast<unsigned char>(value[i]);
        if (curr == '_' || curr == '-' || curr == ' ' || curr == ':' || curr == '/' || curr == '.') {
            flush();
            continue;
        }

        if (!current.empty()) {
            unsigned char prev = static_cast<unsigned char>(value[i - 1]);
            unsigned char next = (i + 1 < value.size())
                ? static_cast<unsigned char>(value[i + 1])
                : 0;
            if (camel_should_split(prev, curr, next)) flush();
        }
        current.push_back(static_cast<char>(curr));
    }
    flush();
    return pieces;
}

inline void append_token_vector_candidate(std::vector<std::pair<std::string, std::string>>& out,
                                          std::unordered_set<std::string>& seen,
                                          const std::filesystem::path& dir) {
    if (dir.empty()) return;
    std::error_code ec;
    auto normalized = std::filesystem::weakly_canonical(dir, ec);
    std::string key = ec ? dir.lexically_normal().generic_string() : normalized.generic_string();
    if (key.empty() || !seen.insert(key).second) return;

    out.emplace_back((dir / "token_vectors_128d.bin").string(),
                     (dir / "token_list.txt").string());
}

inline std::vector<std::pair<std::string, std::string>> token_vector_candidate_paths(
    const std::string& repo_root_hint = {},
    const std::string& exe_path_hint = {}) {
    std::vector<std::pair<std::string, std::string>> candidates;
    std::unordered_set<std::string> seen;

    if (const char* embed_dir = std::getenv("CODETOPO_EMBED_DIR")) {
        append_token_vector_candidate(candidates, seen, std::filesystem::path(embed_dir));
    }

    const std::string exe_path = !exe_path_hint.empty() ? exe_path_hint : get_self_executable_path();
    if (!exe_path.empty()) {
        auto exe_dir = std::filesystem::path(exe_path).parent_path();
        append_token_vector_candidate(candidates, seen, exe_dir);
        append_token_vector_candidate(candidates, seen, exe_dir / "tools");
        append_token_vector_candidate(candidates, seen, exe_dir / ".." / "tools");
    }

    if (!repo_root_hint.empty()) {
        append_token_vector_candidate(candidates, seen, std::filesystem::path(repo_root_hint) / "tools");
    }

    append_token_vector_candidate(candidates, seen, std::filesystem::current_path() / "tools");

    // ~/.local/share/codetopo/ — installed by install.sh
    if (const char* home = std::getenv("HOME")) {
        append_token_vector_candidate(candidates, seen,
            std::filesystem::path(home) / ".local" / "share" / "codetopo");
    }

    return candidates;
}

} // namespace semantic

class TokenVectorTable {
public:
    static constexpr int DIM = 256;

    bool load(const std::string& bin_path, const std::string& txt_path) {
        clear();

        std::ifstream bin(bin_path, std::ios::binary);
        if (!bin) return false;

        int32_t count = 0;
        int32_t dim = 0;
        bin.read(reinterpret_cast<char*>(&count), sizeof(count));
        bin.read(reinterpret_cast<char*>(&dim), sizeof(dim));
        if (!bin || count <= 0 || dim != DIM) {
            clear();
            return false;
        }

        std::vector<int8_t> blob(static_cast<size_t>(count) * DIM);
        bin.read(reinterpret_cast<char*>(blob.data()),
                 static_cast<std::streamsize>(blob.size() * sizeof(int8_t)));
        if (!bin) {
            clear();
            return false;
        }

        std::ifstream txt(txt_path);
        if (!txt) {
            clear();
            return false;
        }

        std::unordered_map<std::string, int> token_to_idx;
        token_to_idx.reserve(static_cast<size_t>(count) * 2);
        std::string line;
        int idx = 0;
        while (std::getline(txt, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            token_to_idx.emplace(line, idx++);
        }

        if (idx != count) {
            clear();
            return false;
        }

        blob_ = std::move(blob);
        token_to_idx_ = std::move(token_to_idx);
        token_count_ = count;
        return true;
    }

    bool ready() const { return !token_to_idx_.empty(); }

    std::vector<int8_t> embed(const std::string& symbol_name) const {
        if (!ready() || symbol_name.empty()) return {};

        std::vector<std::string> candidates;
        candidates.reserve(16);
        std::unordered_set<std::string> seen;
        seen.reserve(16);

        auto add_candidate = [&](std::string token) {
            if (token.empty() || !seen.insert(token).second) return;
            candidates.push_back(std::move(token));
        };

        add_candidate(semantic::lower_ascii(symbol_name));
        auto pieces = semantic::split_identifier_pieces(symbol_name);
        for (size_t i = 0; i < pieces.size(); ++i) {
            add_candidate(pieces[i]);
            if (i > 0) add_candidate("##" + pieces[i]);
        }

        std::vector<float> accum(DIM, 0.0f);
        float total_weight = 0.0f;
        for (const auto& token : candidates) {
            auto it = token_to_idx_.find(token);
            if (it == token_to_idx_.end()) continue;
            float weight = heuristic_idf(token);
            if (weight <= 0.0f) continue;
            const int8_t* vec = blob_.data() + static_cast<size_t>(it->second) * DIM;
            total_weight += weight;
            for (int i = 0; i < DIM; ++i) {
                accum[i] += weight * static_cast<float>(vec[i]);
            }
        }

        if (total_weight <= 0.0f) return {};

        for (float& value : accum) value /= total_weight;

        float norm_sq = 0.0f;
        for (float v : accum) norm_sq += v * v;
        if (norm_sq <= 0.0f) return {};

        const float scale = 127.0f / std::sqrt(norm_sq);
        std::vector<int8_t> result(DIM, 0);
        for (int i = 0; i < DIM; ++i) {
            int q = static_cast<int>(std::lround(accum[i] * scale));
            q = (std::max)(-127, (std::min)(127, q));
            result[i] = static_cast<int8_t>(q);
        }
        return result;
    }

    static float cosine(const int8_t* a, const int8_t* b, int dim) {
        int32_t dot = 0, sq_a = 0, sq_b = 0;
        for (int i = 0; i < dim; i++) {
            dot  += static_cast<int32_t>(a[i]) * static_cast<int32_t>(b[i]);
            sq_a += static_cast<int32_t>(a[i]) * static_cast<int32_t>(a[i]);
            sq_b += static_cast<int32_t>(b[i]) * static_cast<int32_t>(b[i]);
        }
        if (sq_a == 0 || sq_b == 0) return 0.0f;
        return static_cast<float>(dot) /
               (std::sqrt(static_cast<float>(sq_a)) * std::sqrt(static_cast<float>(sq_b)));
    }

private:
    static float heuristic_idf(const std::string& piece) {
        std::string lower = piece;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        std::string_view clean = lower;
        if (clean.size() > 2 && clean.substr(0, 2) == "##") clean.remove_prefix(2);

        static const std::unordered_set<std::string_view> low_idf = {
            "test", "tests", "mock", "fake", "stub", "base", "abstract",
            "get", "set", "is", "has", "on", "do", "run", "to", "by", "of",
            "a", "an", "the", "new", "old", "my", "this", "that", "some",
            "impl", "interface", "class", "type", "enum", "struct"
        };
        if (low_idf.count(clean)) return 0.3f;

        if (clean.size() <= 1) return 0.1f;
        if (clean.size() == 2) return 0.5f;

        return 1.0f;
    }

    void clear() {
        blob_.clear();
        token_to_idx_.clear();
        token_count_ = 0;
    }

    std::vector<int8_t> blob_;
    std::unordered_map<std::string, int> token_to_idx_;
    int token_count_ = 0;
};

} // namespace codetopo
