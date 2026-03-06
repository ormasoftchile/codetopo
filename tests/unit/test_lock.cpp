// T024: Unit test for lock file
#include <catch2/catch_test_macros.hpp>
#include "util/lock.h"
#include <filesystem>

namespace fs = std::filesystem;
using namespace codetopo;

TEST_CASE("Lock acquire on new file succeeds", "[lock]") {
    auto tmp = fs::temp_directory_path() / "codetopo_test_lock";
    fs::create_directories(tmp);
    auto lock_path = tmp / "test.lock";

    {
        FileLock lock(lock_path);
        REQUIRE(lock.acquire());
    }
    REQUIRE_FALSE(fs::exists(lock_path));

    fs::remove_all(tmp);
}

TEST_CASE("Lock detects live process holding lock", "[lock]") {
    auto tmp = fs::temp_directory_path() / "codetopo_test_lock2";
    fs::create_directories(tmp);
    auto lock_path = tmp / "test.lock";

    {
        FileLock lock1(lock_path);
        REQUIRE(lock1.acquire());

        FileLock lock2(lock_path);
        REQUIRE_FALSE(lock2.acquire());
        REQUIRE(lock2.holder_pid() > 0);
    }

    fs::remove_all(tmp);
}

TEST_CASE("Lock breaks stale lock from dead PID", "[lock]") {
    auto tmp = fs::temp_directory_path() / "codetopo_test_lock3";
    fs::create_directories(tmp);
    auto lock_path = tmp / "test.lock";

    {
        std::ofstream f(lock_path);
        f << "99999999";
    }

    FileLock lock(lock_path);
    REQUIRE(lock.acquire());
    REQUIRE(lock.was_stale_broken());

    fs::remove_all(tmp);
}
