// T081: Contract test — verify error envelope format.
#include <catch2/catch_test_macros.hpp>
#include "mcp/error.h"
#include "util/json.h"

using namespace codetopo;

TEST_CASE("Error envelope has correct JSON-RPC format", "[contract][us2]") {
    auto err = McpError::not_found("Symbol 42 not found");
    auto json = err.to_json_rpc(5);

    auto doc = json_parse(json);
    REQUIRE(doc);
    auto* root = doc.root();

    // Must have jsonrpc, id, error fields
    REQUIRE(json_get_str(root, "jsonrpc") != nullptr);
    REQUIRE(std::string(json_get_str(root, "jsonrpc")) == "2.0");
    REQUIRE(json_get_int(root, "id") == 5);

    auto* error = yyjson_obj_get(root, "error");
    REQUIRE(error != nullptr);
    REQUIRE(json_get_int(error, "code") == -32000);  // not_found code

    auto* message = json_get_str(error, "message");
    REQUIRE(message != nullptr);
    REQUIRE(std::string(message) == "Symbol 42 not found");

    auto* data = yyjson_obj_get(error, "data");
    REQUIRE(data != nullptr);
    REQUIRE(std::string(json_get_str(data, "error_code")) == "not_found");
}

TEST_CASE("invalid_input error has correct code", "[contract][us2]") {
    auto err = McpError::invalid_input("Missing parameter");
    auto json = err.to_json_rpc(10);
    auto doc = json_parse(json);
    REQUIRE(doc);

    auto* error = yyjson_obj_get(doc.root(), "error");
    REQUIRE(json_get_int(error, "code") == -32602);
    auto* data = yyjson_obj_get(error, "data");
    REQUIRE(std::string(json_get_str(data, "error_code")) == "invalid_input");
}

TEST_CASE("query_timeout error has correct code", "[contract][us2]") {
    auto err = McpError::query_timeout("Exceeded 5s");
    auto json = err.to_json_rpc(99);
    auto doc = json_parse(json);
    REQUIRE(doc);

    auto* error = yyjson_obj_get(doc.root(), "error");
    REQUIRE(json_get_int(error, "code") == -32001);
    auto* data = yyjson_obj_get(error, "data");
    REQUIRE(std::string(json_get_str(data, "error_code")) == "query_timeout");
}

TEST_CASE("db_error has correct code", "[contract][us2]") {
    auto err = McpError::db_error("SQLITE_BUSY");
    REQUIRE(err.json_rpc_code == -32002);
    REQUIRE(err.error_code == "db_error");
}

TEST_CASE("limit_exceeded has correct code", "[contract][us2]") {
    auto err = McpError::limit_exceeded("Too many results");
    REQUIRE(err.json_rpc_code == -32003);
    REQUIRE(err.error_code == "limit_exceeded");
}
