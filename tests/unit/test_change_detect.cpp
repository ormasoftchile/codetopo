// T023: Unit test for change detection logic
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>

namespace fs = std::filesystem;

// Simple change detection logic to test
struct FileInfo {
    int64_t mtime_ns;
    int64_t size_bytes;
};

bool needs_rehash(const FileInfo& old_info, const FileInfo& new_info) {
    return old_info.mtime_ns != new_info.mtime_ns ||
           old_info.size_bytes != new_info.size_bytes;
}

TEST_CASE("Change detection: same mtime+size means unchanged", "[change_detect]") {
    FileInfo a{1000, 500};
    FileInfo b{1000, 500};
    REQUIRE_FALSE(needs_rehash(a, b));
}

TEST_CASE("Change detection: different mtime triggers rehash", "[change_detect]") {
    FileInfo a{1000, 500};
    FileInfo b{2000, 500};
    REQUIRE(needs_rehash(a, b));
}

TEST_CASE("Change detection: different size triggers rehash", "[change_detect]") {
    FileInfo a{1000, 500};
    FileInfo b{1000, 600};
    REQUIRE(needs_rehash(a, b));
}

TEST_CASE("Change detection: real file modification detected", "[change_detect]") {
    auto tmp = fs::temp_directory_path() / "codetopo_test_change";
    fs::create_directories(tmp);
    auto file = tmp / "test.cpp";

    // Create file
    { std::ofstream f(file); f << "int main() {}"; }
    auto mtime1 = fs::last_write_time(file);
    auto size1 = fs::file_size(file);

    // Wait and modify
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    { std::ofstream f(file); f << "int main() { return 0; }"; }
    auto mtime2 = fs::last_write_time(file);
    auto size2 = fs::file_size(file);

    // Should detect the change
    REQUIRE((mtime1 != mtime2 || size1 != size2));

    fs::remove_all(tmp);
}
