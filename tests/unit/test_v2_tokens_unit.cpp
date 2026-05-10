// Tests for v2-Tokens Phase 1: token counting and cursor encoding
#include <catch2/catch_test_macros.hpp>
#include "util/tokens.h"
#include "mcp/envelope.h"
#include <string>

using namespace codetopo;

TEST_CASE("Token counting: estimate_token_count", "[v2-tokens][tokens]") {
    SECTION("Empty string") {
        REQUIRE(tokens::estimate_token_count("") == 0);
    }
    
    SECTION("Short ASCII") {
        REQUIRE(tokens::estimate_token_count("test") == 1);
    }
    
    SECTION("Medium code snippet") {
        std::string code = "void foo() { return 42; }";
        REQUIRE(tokens::estimate_token_count(code) == 6);
    }
    
    SECTION("Large text") {
        std::string large(1000, 'x');
        REQUIRE(tokens::estimate_token_count(large) == 250);
    }
}

TEST_CASE("Cursor: encode/decode round-trip", "[v2-tokens][cursor]") {
    SECTION("Zero cursor") {
        mcp::Cursor c{};
        c.offset = 0;
        c.ranking_seed = 0;
        std::string encoded = c.encode();
        REQUIRE(!encoded.empty());
        
        mcp::Cursor decoded;
        REQUIRE(mcp::Cursor::decode(encoded, decoded));
        REQUIRE(decoded.offset == 0);
        REQUIRE(decoded.ranking_seed == 0);
    }
    
    SECTION("Non-zero offset") {
        mcp::Cursor c{};
        c.offset = 42;
        c.ranking_seed = 0;
        std::string encoded = c.encode();
        
        mcp::Cursor decoded;
        REQUIRE(mcp::Cursor::decode(encoded, decoded));
        REQUIRE(decoded.offset == 42);
    }
    
    SECTION("Large offset") {
        mcp::Cursor c{};
        c.offset = 999999;
        c.ranking_seed = 12345;
        std::string encoded = c.encode();
        
        mcp::Cursor decoded;
        REQUIRE(mcp::Cursor::decode(encoded, decoded));
        REQUIRE(decoded.offset == 999999);
        REQUIRE(decoded.ranking_seed == 12345);
    }
    
    SECTION("Invalid cursor string") {
        mcp::Cursor decoded;
        REQUIRE_FALSE(mcp::Cursor::decode("invalid", decoded));
        REQUIRE_FALSE(mcp::Cursor::decode("", decoded));
    }
}
