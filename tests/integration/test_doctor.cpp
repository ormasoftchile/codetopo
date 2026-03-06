// T102-T103: Integration tests for doctor command.
#include <catch2/catch_test_macros.hpp>
#include "cli/cmd_doctor.h"
#include "db/connection.h"
#include "db/schema.h"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace codetopo;

TEST_CASE("Doctor reports ok on valid DB", "[integration][us5]") {
    auto tmp = fs::temp_directory_path() / "codetopo_test_doctor";
    fs::create_directories(tmp);
    auto db_path = tmp / "test.sqlite";

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);
        schema::set_kv(conn, "indexer_version", "1.0.0");
        schema::set_kv(conn, "repo_root", tmp.string());
        schema::set_kv(conn, "last_index_time", "2026-03-04T12:00:00Z");
    }

    int rc = run_doctor(db_path.string());
    REQUIRE(rc == 0);

    fs::remove_all(tmp);
}

TEST_CASE("Doctor detects version mismatch", "[integration][us5]") {
    auto tmp = fs::temp_directory_path() / "codetopo_test_doctor2";
    fs::create_directories(tmp);
    auto db_path = tmp / "test.sqlite";

    {
        Connection conn(db_path);
        schema::ensure_schema(conn);
        schema::set_kv(conn, "schema_version", "999");
    }

    int rc = run_doctor(db_path.string());
    REQUIRE(rc == 3);

    fs::remove_all(tmp);
}
