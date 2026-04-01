// Tests for C# extractor edge types: include (using), inherit (base_list), call.
// Validates that the extractor produces the correct ref kinds for C# patterns.
//
// Pattern: parse C# source via tree-sitter, run Extractor, inspect result.refs.

#include <catch2/catch_test_macros.hpp>
#include "core/arena.h"
#include "core/arena_pool.h"
#include "index/parser.h"
#include "index/extractor.h"
#include <string>
#include <algorithm>

using namespace codetopo;

namespace codetopo {
    void set_thread_arena(Arena* arena);
    void register_arena_allocator();
}

// Helper: check if any ref with given kind exists
static bool has_ref_kind(const ExtractionResult& result, const std::string& kind) {
    return std::any_of(result.refs.begin(), result.refs.end(),
        [&](const ExtractedRef& r) { return r.kind == kind; });
}

// Helper: count refs with given kind
static int count_refs(const ExtractionResult& result, const std::string& kind) {
    return static_cast<int>(std::count_if(result.refs.begin(), result.refs.end(),
        [&](const ExtractedRef& r) { return r.kind == kind; }));
}

// Helper: check if a ref with given kind and name exists
static bool has_ref(const ExtractionResult& result, const std::string& kind,
                    const std::string& name) {
    return std::any_of(result.refs.begin(), result.refs.end(),
        [&](const ExtractedRef& r) { return r.kind == kind && r.name == name; });
}

// Setup arena for tree-sitter
static void setup_arena() {
    static bool initialized = false;
    if (!initialized) {
        register_arena_allocator();
        initialized = true;
    }
}

// Parse and extract from C# source
static ExtractionResult extract_csharp(const std::string& source) {
    setup_arena();
    ArenaPool pool(1, 32 * 1024 * 1024);
    auto lease = ArenaLease(pool);
    set_thread_arena(lease.get());

    Parser parser;
    REQUIRE(parser.set_language("csharp"));
    auto tree = TreeGuard(parser.parse(source));
    REQUIRE(tree.tree);

    Extractor extractor(5000, 100, 0);
    return extractor.extract(tree.tree, source, "csharp", "test.cs");
}

// ── Test 1: C# using directives produce include refs ────────────────────────

TEST_CASE("C# using directives become include refs", "[unit][csharp][extractor]") {
    std::string source = R"(
using System;
using System.Collections.Generic;
using System.Linq;

namespace TestApp {
    class Program {
        static void Main() {}
    }
}
)";

    auto result = extract_csharp(source);

    // Must have include refs for using directives
    REQUIRE(has_ref_kind(result, "include"));

    // Verify specific using names appear
    // Tree-sitter may extract the namespace name in different forms;
    // check that at least one include ref contains "System"
    bool found_system = std::any_of(result.refs.begin(), result.refs.end(),
        [](const ExtractedRef& r) {
            return r.kind == "include" && r.name.find("System") != std::string::npos;
        });
    REQUIRE(found_system);

    // Should have at least 3 include refs (one per using directive)
    REQUIRE(count_refs(result, "include") >= 3);
}

// ── Test 2: C# class inheritance produces inherit refs ──────────────────────

TEST_CASE("C# class inheritance becomes inherit refs", "[unit][csharp][extractor]") {
    std::string source = R"(
using System;

namespace TestApp {
    class Foo : Bar, IDisposable {
        public void Dispose() {}
    }
}
)";

    auto result = extract_csharp(source);

    REQUIRE(has_ref_kind(result, "inherit"));

    // Should have inherit refs for both Bar and IDisposable
    bool found_bar = std::any_of(result.refs.begin(), result.refs.end(),
        [](const ExtractedRef& r) {
            return r.kind == "inherit" && r.name.find("Bar") != std::string::npos;
        });
    bool found_idisposable = std::any_of(result.refs.begin(), result.refs.end(),
        [](const ExtractedRef& r) {
            return r.kind == "inherit" && r.name.find("IDisposable") != std::string::npos;
        });
    REQUIRE(found_bar);
    REQUIRE(found_idisposable);
    REQUIRE(count_refs(result, "inherit") >= 2);
}

// ── Test 3: C# method invocations produce call refs ─────────────────────────

TEST_CASE("C# method invocations produce call refs", "[unit][csharp][extractor]") {
    std::string source = R"(
using System;

namespace TestApp {
    class Program {
        static void Main() {
            Console.WriteLine("test");
            var x = Math.Max(1, 2);
        }
    }
}
)";

    auto result = extract_csharp(source);

    REQUIRE(has_ref_kind(result, "call"));

    // Should find a call ref related to WriteLine or Console.WriteLine
    bool found_writeline = std::any_of(result.refs.begin(), result.refs.end(),
        [](const ExtractedRef& r) {
            return r.kind == "call" &&
                   (r.name.find("WriteLine") != std::string::npos ||
                    r.name.find("Console") != std::string::npos);
        });
    REQUIRE(found_writeline);
}

// ── Test 4: Comprehensive C# file with all edge types ───────────────────────

TEST_CASE("C# file with usings, inheritance, and calls produces all ref types",
          "[unit][csharp][extractor]") {
    std::string source = R"(
using System;
using System.Collections.Generic;

namespace MyApp {
    interface IGreeter {
        void Greet();
    }

    class HelloGreeter : IGreeter {
        public void Greet() {
            Console.WriteLine("Hello!");
        }

        public void Run() {
            var list = new List<string>();
            list.Add("test");
            Greet();
        }
    }
}
)";

    auto result = extract_csharp(source);

    // All three ref kinds must be present
    REQUIRE(has_ref_kind(result, "include"));
    REQUIRE(has_ref_kind(result, "inherit"));
    REQUIRE(has_ref_kind(result, "call"));

    // Symbols should include the interface and class at minimum
    REQUIRE(result.symbols.size() >= 2);

    // Include refs from using directives
    REQUIRE(count_refs(result, "include") >= 2);

    // Inherit ref from HelloGreeter : IGreeter
    bool found_igreeter_inherit = std::any_of(result.refs.begin(), result.refs.end(),
        [](const ExtractedRef& r) {
            return r.kind == "inherit" && r.name.find("IGreeter") != std::string::npos;
        });
    REQUIRE(found_igreeter_inherit);

    // Call refs from method invocations
    REQUIRE(count_refs(result, "call") >= 1);
}
