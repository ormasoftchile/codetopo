#include <catch2/catch_test_macros.hpp>
#include "core/inplace_vector.h"
#include <string>
#include <vector>

using namespace codetopo;

namespace {
struct LifetimeTracker {
    static int construct_count;
    static int destruct_count;

    static void reset() {
        construct_count = 0;
        destruct_count = 0;
    }

    int value = 0;
    LifetimeTracker(int v = 0) : value(v) { ++construct_count; }
    LifetimeTracker(const LifetimeTracker& other) : value(other.value) { ++construct_count; }
    LifetimeTracker(LifetimeTracker&& other) noexcept : value(other.value) { ++construct_count; }
    LifetimeTracker& operator=(const LifetimeTracker& other) = default;
    LifetimeTracker& operator=(LifetimeTracker&& other) noexcept = default;
    ~LifetimeTracker() { ++destruct_count; }
};

int LifetimeTracker::construct_count = 0;
int LifetimeTracker::destruct_count = 0;
} // namespace

TEST_CASE("inplace_vector basic operations", "[core][inplace_vector]") {
    inplace_vector<int, 4> vec;
    CHECK(vec.empty());
    CHECK(vec.size() == 0);
    CHECK(vec.capacity() == 4);
    CHECK(vec.max_size() == 4);

    vec.push_back(10);
    vec.push_back(20);
    vec.emplace_back(30);

    CHECK_FALSE(vec.empty());
    CHECK(vec.size() == 3);
    CHECK(vec[0] == 10);
    CHECK(vec[1] == 20);
    CHECK(vec[2] == 30);
    CHECK(vec.front() == 10);
    CHECK(vec.back() == 30);
    CHECK(vec.at(1) == 20);

    CHECK_THROWS_AS(vec.at(3), std::out_of_range);

    vec.push_back(40);
    CHECK(vec.size() == 4);
    CHECK_THROWS_AS(vec.push_back(50), std::bad_alloc);

    vec.pop_back();
    CHECK(vec.size() == 3);
    CHECK(vec.back() == 30);

    vec.clear();
    CHECK(vec.empty());
    CHECK(vec.size() == 0);
}

TEST_CASE("inplace_vector constructors and copy/move", "[core][inplace_vector]") {
    inplace_vector<std::string, 5> v1 = {"alpha", "beta", "gamma"};
    CHECK(v1.size() == 3);
    CHECK(v1[0] == "alpha");
    CHECK(v1[1] == "beta");
    CHECK(v1[2] == "gamma");

    // Copy construction
    inplace_vector<std::string, 5> v2 = v1;
    CHECK(v2.size() == 3);
    CHECK(v2[0] == "alpha");

    // Move construction
    inplace_vector<std::string, 5> v3 = std::move(v2);
    CHECK(v3.size() == 3);
    CHECK(v3[1] == "beta");

    // Iterator constructor
    std::vector<std::string> std_vec = {"one", "two"};
    inplace_vector<std::string, 5> v4(std_vec.begin(), std_vec.end());
    CHECK(v4.size() == 2);
    CHECK(v4[0] == "one");
    CHECK(v4[1] == "two");
}

TEST_CASE("inplace_vector non-trivial lifetime tracking", "[core][inplace_vector]") {
    LifetimeTracker::reset();
    {
        inplace_vector<LifetimeTracker, 3> vec;
        vec.emplace_back(1);
        vec.emplace_back(2);
        CHECK(LifetimeTracker::construct_count == 2);
        CHECK(LifetimeTracker::destruct_count == 0);

        vec.pop_back();
        CHECK(LifetimeTracker::destruct_count == 1);
    }
    CHECK(LifetimeTracker::construct_count == 2);
    CHECK(LifetimeTracker::destruct_count == 2);
}
