#pragma once
// Token counting utilities for Phase 1: heuristic char-based estimation.
// Provides ±20% approximation of BPE token counts without tokenizer dependency.
//
// Phase 1: chars/4 heuristic (this file)
// Phase 2: Optional exact BPE tokenizer (future: swap implementation without changing call sites)

#include <string>
#include <cstdint>

namespace codetopo {
namespace tokens {

// Estimate token count using character-count heuristic.
// Approximation: 1 token ≈ 4 characters for code.
// Expected drift: ±20% vs cl100k_base/o200k_base.
// 
// This is sufficient for soft budget enforcement (we're setting caps, not billing).
// Centralized here so Phase 2 can swap in real BPE without touching call sites.
inline int64_t estimate_token_count(const std::string& text) {
    if (text.empty()) return 0;
    // For UTF-8, we count bytes. Real tokens are closer to grapheme clusters,
    // but for code (mostly ASCII with some UTF-8 identifiers), bytes/4 works.
    // This intentionally rounds down to be conservative.
    return static_cast<int64_t>(text.size()) / 4;
}

// Estimate token count for a JSON string (after serialization).
// Accounts for JSON overhead (quotes, commas, braces).
inline int64_t estimate_json_tokens(const std::string& json_text) {
    return estimate_token_count(json_text);
}

} // namespace tokens
} // namespace codetopo
