// T024: Unit test for lock file
#include <catch2/catch_test_macros.hpp>
#include "util/lock.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <atomic>
#include <barrier>
#include <thread>

namespace fs = std::filesystem;
using namespace codetopo;

static fs::path lock_test_dir(const std::string& name) {
    auto dir = fs::current_path() / (".codetopo_lock_test_" + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

TEST_CASE("Lock acquire on new file succeeds", "[unit][lock]") {
    auto tmp = lock_test_dir("new_file");
    auto lock_path = tmp / "test.lock";

    {
        FileLock lock(lock_path);
        REQUIRE(lock.acquire());
    }
    REQUIRE_FALSE(fs::exists(lock_path));

    fs::remove_all(tmp);
}

TEST_CASE("Lock detects live process holding lock", "[unit][lock]") {
    auto tmp = lock_test_dir("live_holder");
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

TEST_CASE("Read-only lock observation never contends with other probes", "[unit][lock]") {
    auto tmp = lock_test_dir("readonly_probe");
    auto lock_path = tmp / "test.lock";
    FileLock first_probe(lock_path);
    FileLock second_probe(lock_path);

    REQUIRE_FALSE(first_probe.held_by_live_process());
    REQUIRE_FALSE(second_probe.held_by_live_process());
    REQUIRE_FALSE(fs::exists(lock_path));

    {
        FileLock writer(lock_path);
        REQUIRE(writer.acquire());
        REQUIRE(first_probe.held_by_live_process());
        REQUIRE(second_probe.held_by_live_process());
    }

    REQUIRE_FALSE(first_probe.held_by_live_process());
    REQUIRE_FALSE(second_probe.held_by_live_process());
    REQUIRE_FALSE(fs::exists(lock_path));

    {
        std::ofstream stale(lock_path);
        stale << "99999999";
    }
    REQUIRE_FALSE(first_probe.held_by_live_process());
    REQUIRE(fs::exists(lock_path));
    fs::remove(lock_path);
    fs::remove_all(tmp);
}

TEST_CASE("Lock breaks stale lock from dead PID", "[unit][lock]") {
    auto tmp = lock_test_dir("stale_holder");
    auto lock_path = tmp / "test.lock";

    {
        std::ofstream f(lock_path);
        f << "99999999";
    }

    FileLock lock(lock_path);
    REQUIRE(lock.acquire());
    REQUIRE(lock.was_stale_broken());

    lock.release();
    fs::remove_all(tmp);
}

TEST_CASE("Blocking acquire breaks stale lock immediately", "[unit][lock]") {
    auto tmp = lock_test_dir("blocking_stale");
    auto lock_path = tmp / "test.lock";

    {
        std::ofstream f(lock_path);
        f << "99999999";
    }

    FileLock lock(lock_path);
    auto start = std::chrono::steady_clock::now();
    REQUIRE(lock.acquire_blocking(std::chrono::seconds(5),
                                  std::chrono::milliseconds(100),
                                  std::chrono::milliseconds(200)));
    auto elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE(lock.was_stale_broken());
    REQUIRE(elapsed < std::chrono::seconds(1));

    lock.release();
    fs::remove_all(tmp);
}

TEST_CASE("Blocking acquire waits for live holder then times out", "[unit][lock]") {
    auto tmp = lock_test_dir("blocking_live_timeout");
    auto lock_path = tmp / "test.lock";

    {
        FileLock lock1(lock_path);
        REQUIRE(lock1.acquire());

        FileLock lock2(lock_path);
        int wait_messages = 0;
        auto start = std::chrono::steady_clock::now();
        REQUIRE_FALSE(lock2.acquire_blocking(
            std::chrono::milliseconds(350),
            std::chrono::milliseconds(100),
            std::chrono::milliseconds(200),
            [&wait_messages](int64_t pid, std::chrono::milliseconds timeout) {
                REQUIRE(pid > 0);
                REQUIRE(timeout == std::chrono::milliseconds(350));
                ++wait_messages;
            }));
        auto elapsed = std::chrono::steady_clock::now() - start;

        REQUIRE(wait_messages == 1);
        REQUIRE(lock2.holder_pid() > 0);
        REQUIRE(elapsed >= std::chrono::milliseconds(300));
        REQUIRE(elapsed < std::chrono::seconds(2));
    }

    fs::remove_all(tmp);
}

TEST_CASE("Simultaneous writer admission has exactly one owner", "[unit][lock][workspace]") {
    auto tmp = lock_test_dir("atomic_admission");
    for (int iteration = 0; iteration < 20; ++iteration) {
        auto path = tmp / ("race-" + std::to_string(iteration) + ".lock");
        std::barrier rendezvous(3);
        std::atomic<int> admitted{0};
        auto compete = [&] {
            FileLock lock(path);
            rendezvous.arrive_and_wait();
            if (lock.acquire()) ++admitted;
            rendezvous.arrive_and_wait();
            rendezvous.arrive_and_wait();
        };
        std::thread first(compete), second(compete);
        rendezvous.arrive_and_wait();
        rendezvous.arrive_and_wait();
        auto owners = admitted.load();
        rendezvous.arrive_and_wait();
        first.join();
        second.join();
        REQUIRE(owners == 1);
        REQUIRE_FALSE(fs::exists(path));
    }
    fs::remove_all(tmp);
}
