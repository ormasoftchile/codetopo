#include <catch2/catch_test_macros.hpp>
#include "core/config.h"
#include "index/scanner.h"
#include "util/log.h"
#include "util/process.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace codetopo;
namespace fs = std::filesystem;

namespace {

struct ProgressFixture {
    fs::path root = fs::current_path() / "build" / "startup-progress-fixture";
    ProgressFixture() {
        REQUIRE_FALSE(fs::exists(root));
        fs::create_directories(root / "src");
    }
    ~ProgressFixture() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

std::string read_text(const fs::path& path) {
    std::ifstream input(path);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

struct CaptureProtocol {
    std::ostringstream output;
    std::streambuf* previous = std::cout.rdbuf(output.rdbuf());
    bool previous_notifications = mcp_notify_active().exchange(false);
    ~CaptureProtocol() {
        std::cout.rdbuf(previous);
        mcp_notify_active() = previous_notifications;
    }
};

struct CaptureStderr {
    std::ostringstream output;
    std::streambuf* previous = std::cerr.rdbuf(output.rdbuf());
    ~CaptureStderr() { std::cerr.rdbuf(previous); }
};

} // namespace

TEST_CASE("Filesystem scanner reports real counts without changing its file selection",
          "[unit][startup-progress][scanner]") {
    ProgressFixture fixture;
    std::ofstream(fixture.root / "one.cpp") << "int one() { return 1; }\n";
    std::ofstream(fixture.root / "src" / "two.cpp") << "int two() { return 2; }\n";
    std::ofstream(fixture.root / "src" / "ignored.txt") << "not source\n";
    Config config;
    config.repo_root = fixture.root;
    config.no_gitignore = true;
    std::vector<ScanProgress> progress;
    Scanner scanner(config, [&](const ScanProgress& state) { progress.push_back(state); });
    auto files = scanner.scan();
    REQUIRE(files.size() == 2);
    REQUIRE_FALSE(progress.empty());
    CHECK(progress.back().phase == "complete");
    CHECK(progress.back().directories == 2);
    CHECK(progress.back().entries == 4);
    CHECK(progress.back().sources == 2);
    std::vector<std::string> deleted;
    auto targeted = scanner.scan_paths({"one.cpp", "gone.cpp"}, deleted);
    REQUIRE(targeted.size() == 1);
    REQUIRE(deleted.size() == 1);
    CHECK(deleted.front() == "gone.cpp");
    CHECK(progress.back().phase == "complete");
    CHECK(progress.back().sources == 1);
    CHECK(progress.back().entries == 2);
}

TEST_CASE("Scan heartbeat persists current phase and shuts down without waiting its interval",
          "[unit][startup-progress]") {
    ProgressFixture fixture;
    auto path = fixture.root / "index.sqlite.progress";
    {
        ScanProgressReporter reporter(path, std::chrono::milliseconds(20));
        reporter.update({"filesystem", 3, 500, 123});
        CHECK(read_text(path).find("sources=123") != std::string::npos);
        auto initial = fs::last_write_time(path);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (fs::last_write_time(path) == initial &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK((fs::last_write_time(path) != initial));
    }
    auto started = std::chrono::steady_clock::now();
    {
        ScanProgressReporter reporter(path, std::chrono::hours(1));
        reporter.update({"complete", 3, 500, 123});
    }
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
    CHECK(read_text(path).find("phase=complete") != std::string::npos);
}

TEST_CASE("MCP startup diagnostics persist before initialization without writing protocol stdout",
          "[unit][startup-progress][logging]") {
    ProgressFixture fixture;
    auto path = fixture.root / "mcp.log";
    CaptureProtocol protocol;
    {
        ScopedMcpLogFile file(path);
        REQUIRE(file.enabled());
        mcp_log("stdio: ready; waiting for client initialize");
        mcp_log("reindex: progress scan: phase=filesystem sources=123");
    }
    CHECK(protocol.output.str().empty());
    auto text = read_text(path);
    CHECK(text.find("waiting for client initialize") != std::string::npos);
    CHECK(text.find("sources=123") != std::string::npos);
    CHECK(get_current_process_id() > 0);
}

TEST_CASE("Concurrent child lifecycle and MCP logs remain complete stderr lines",
          "[unit][startup-progress][logging]") {
    CaptureProtocol protocol;
    CaptureStderr diagnostics;
    std::thread first([] {
        for (int i = 0; i < 100; ++i) {
            mcp_log("tool: diagnostic-" + std::to_string(i));
        }
    });
    std::thread second([] {
        for (int i = 0; i < 100; ++i) {
            write_stderr_line("[child] exited pid=" + std::to_string(i) +
                              " exit=0 elapsed_ms=1");
        }
    });
    first.join();
    second.join();
    std::istringstream lines(diagnostics.output.str());
    std::string line;
    int count = 0;
    while (std::getline(lines, line)) {
        ++count;
        if (line.starts_with("[child]")) {
            CHECK(line.find(" exit=0 elapsed_ms=1") != std::string::npos);
            CHECK(line.find("tool:") == std::string::npos);
        } else {
            CHECK(line.find("] tool: diagnostic-") != std::string::npos);
            CHECK(line.find("[child]") == std::string::npos);
        }
    }
    CHECK(count == 200);
}
