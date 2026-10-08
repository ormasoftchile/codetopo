#include <catch2/catch_test_macros.hpp>
#include "core/config.h"
#include "index/scanner.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

using namespace codetopo;
namespace fs = std::filesystem;

namespace {

struct ScannerFixture {
    fs::path base = fs::current_path() / "build" / "scanner-correctness-fixture";
    fs::path root = base / "primary";
    ScannerFixture() {
        REQUIRE_FALSE(fs::exists(base));
        fs::create_directories(root);
    }
    ~ScannerFixture() {
        std::error_code ec;
        fs::remove_all(base, ec);
    }
    void write(const fs::path& relative, const std::string& text) {
        auto path = root / relative;
        fs::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary);
        output << text;
        output.close();
        REQUIRE(output.good());
    }
};

std::map<std::string, ScannedFile> by_path(const std::vector<ScannedFile>& files) {
    std::map<std::string, ScannedFile> result;
    for (const auto& file : files) result.emplace(file.relative_path, file);
    return result;
}

} // namespace

TEST_CASE("Filesystem scanner reuses directory enumeration and canonical root metadata",
          "[unit][scanner-correctness][scan-performance-regression]") {
    ScannerFixture fixture;
    fixture.write(".gitignore", "*.generated.cpp\n!keep.generated.cpp\nignored/\n");
    fixture.write("ignored/hidden.cpp", "int hidden();\n");
    fixture.write("skip.generated.cpp", "int skip();\n");
    fixture.write("keep.generated.cpp", "int keep();\n");
    fixture.write("vendor/skipped.cpp", "int vendor();\n");
    fixture.write("notes.txt", "not source\n");
    for (int folder = 0; folder < 16; ++folder) {
        for (int file = 0; file < 32; ++file) {
            fixture.write(fs::path("src") / ("folder-" + std::to_string(folder)) /
                ("file-" + std::to_string(file) + ".cpp"), "int value() { return 42; }\n");
        }
    }
    Config config;
    config.repo_root = fixture.root;
    Scanner scanner(config);
    auto files = by_path(scanner.scan());
    REQUIRE(files.size() == 513);
    CHECK(files.contains("keep.generated.cpp"));
    CHECK_FALSE(files.contains("skip.generated.cpp"));
    CHECK_FALSE(files.contains("ignored/hidden.cpp"));
    CHECK_FALSE(files.contains("vendor/skipped.cpp"));
    CHECK(scanner.metrics().directory_enumerations == 19);
    CHECK(scanner.metrics().canonicalizations == 1);
#ifdef _WIN32
    CHECK(scanner.metrics().metadata_probes == 0);
#endif
    for (const auto& [relative, file] : files) {
        auto expected = fs::canonical(fixture.root / fs::path(relative));
        CHECK(file.absolute_path == expected);
        CHECK(file.size_bytes == static_cast<int64_t>(fs::file_size(expected)));
        auto expected_mtime = std::chrono::duration_cast<std::chrono::nanoseconds>(
            fs::last_write_time(expected).time_since_epoch()).count();
        CHECK(file.mtime_ns == expected_mtime);
    }
}

TEST_CASE("Nested ignore patterns and exclusion selection remain unchanged",
          "[unit][scanner-correctness]") {
    ScannerFixture fixture;
    fixture.write(".gitignore", "/root-only.cpp\n");
    fixture.write("src/.gitignore", "generated.cpp\n!retained.cpp\n");
    fixture.write("root-only.cpp", "int root();\n");
    fixture.write("src/root-only.cpp", "int nested();\n");
    fixture.write("src/generated.cpp", "int generated();\n");
    fixture.write("src/retained.cpp", "int retained();\n");
    fixture.write("other/generated.cpp", "int other();\n");
    fixture.write("src/exclude-me.cpp", "int excluded();\n");
    Config config;
    config.repo_root = fixture.root;
    config.exclude_patterns = {"exclude-me.cpp"};
    Scanner scanner(config);
    auto files = by_path(scanner.scan());
    REQUIRE(files.size() == 3);
    CHECK(files.contains("src/root-only.cpp"));
    CHECK(files.contains("src/retained.cpp"));
    CHECK(files.contains("other/generated.cpp"));
}

TEST_CASE("Source symlink containment and directory-cycle prevention are retained",
          "[unit][scanner-correctness][symlink]") {
    ScannerFixture fixture;
    fixture.write("src/target.cpp", "int target();\n");
    auto outside = fixture.base / "outside";
    fs::create_directories(outside);
    std::ofstream(outside / "outside.cpp") << "int outside();\n";
    std::error_code ec;
    fs::create_directory_symlink(outside, fixture.root / "outside-link", ec);
    if (ec) SKIP("Directory symlink creation is not available in this environment.");
    fs::create_directory_symlink(fixture.root, fixture.root / "src" / "cycle", ec);
    REQUIRE_FALSE(ec);
    fs::create_symlink(fixture.root / "src" / "target.cpp",
                       fixture.root / "target-link.cpp", ec);
    REQUIRE_FALSE(ec);
    Config config;
    config.repo_root = fixture.root;
    Scanner scanner(config);
    auto started = std::chrono::steady_clock::now();
    auto files = scanner.scan();
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(5));
    REQUIRE(files.size() == 2);
    for (const auto& file : files) {
        CHECK(file.relative_path == "src/target.cpp");
        CHECK(file.absolute_path == fs::canonical(fixture.root / "src" / "target.cpp"));
    }
}

TEST_CASE("Scan without gitignore still honors directory exclusions and current file metadata",
          "[unit][scanner-correctness]") {
    ScannerFixture fixture;
    fixture.write(".gitignore", "*.cpp\n");
    fixture.write("src/one.cpp", "int one();\n");
    fixture.write("build/not-indexed.cpp", "int skipped();\n");
    Config config;
    config.repo_root = fixture.root;
    config.no_gitignore = true;
    Scanner scanner(config);
    auto first = scanner.scan();
    REQUIRE(first.size() == 1);
    CHECK(scanner.metrics().directory_enumerations == 2);
    CHECK(scanner.metrics().canonicalizations == 1);
    fixture.write("src/one.cpp", "int one() { return 1000; }\n");
    auto second = scanner.scan();
    REQUIRE(second.size() == 1);
    CHECK(second.front().size_bytes != first.front().size_bytes);
    CHECK(second.front().size_bytes ==
          static_cast<int64_t>(fs::file_size(fixture.root / "src" / "one.cpp")));
}
