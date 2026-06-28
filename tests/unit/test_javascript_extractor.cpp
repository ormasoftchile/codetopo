// Tests for JavaScript constructor_fn extraction from legacy factory patterns.

#include <catch2/catch_test_macros.hpp>
#include "core/arena.h"
#include "core/arena_pool.h"
#include "index/parser.h"
#include "index/extractor.h"
#include <algorithm>
#include <string>

using namespace codetopo;

namespace codetopo {
    void set_thread_arena(Arena* arena);
    void register_arena_allocator();
}

static void setup_arena() {
    static bool initialized = false;
    if (!initialized) {
        register_arena_allocator();
        initialized = true;
    }
}

static ExtractionResult extract_javascript(const std::string& source) {
    setup_arena();
    ArenaPool pool(1, 32 * 1024 * 1024);
    auto lease = ArenaLease(pool);
    set_thread_arena(lease.get());

    Parser parser;
    REQUIRE(parser.set_language("javascript"));
    auto tree = TreeGuard(parser.parse(source));
    REQUIRE(tree.tree);

    Extractor extractor(5000, 100, 0);
    return extractor.extract(tree.tree, source, "javascript", "test.js");
}

static const ExtractedSymbol* find_symbol(const ExtractionResult& result,
                                          const std::string& kind,
                                          const std::string& name) {
    auto it = std::find_if(result.symbols.begin(), result.symbols.end(),
        [&](const ExtractedSymbol& symbol) {
            return symbol.kind == kind && symbol.name == name;
        });
    return it == result.symbols.end() ? nullptr : &*it;
}

static int find_symbol_index(const ExtractionResult& result,
                             const std::string& kind,
                             const std::string& name) {
    auto it = std::find_if(result.symbols.begin(), result.symbols.end(),
        [&](const ExtractedSymbol& symbol) {
            return symbol.kind == kind && symbol.name == name;
        });
    return it == result.symbols.end() ? -1 : static_cast<int>(it - result.symbols.begin());
}

TEST_CASE("JavaScript constructor function variable declarators become constructor_fn",
          "[unit][javascript][extractor]") {
    std::string source = R"(let Core = function(opts) {
  this._private = { container: null, renderer: null };
  this.name = opts.name || 'default';
};

let helper = function(x) {
  return x * 2;
};)";

    auto result = extract_javascript(source);

    auto* core = find_symbol(result, "constructor_fn", "Core");
    REQUIRE(core != nullptr);
    REQUIRE(core->qualname == "Core");

    auto* private_field = find_symbol(result, "field", "_private");
    REQUIRE(private_field != nullptr);
    REQUIRE(private_field->qualname == "Core._fields._private");
    REQUIRE(private_field->start_line == 2);
    REQUIRE(private_field->end_line == 2);

    auto* name_field = find_symbol(result, "field", "name");
    REQUIRE(name_field != nullptr);
    REQUIRE(name_field->qualname == "Core._fields.name");
    REQUIRE(name_field->start_line == 3);
    REQUIRE(name_field->end_line == 3);

    REQUIRE(find_symbol(result, "constructor_fn", "helper") == nullptr);
    REQUIRE(find_symbol(result, "variable", "helper") != nullptr);

    int core_index = find_symbol_index(result, "constructor_fn", "Core");
    int private_index = find_symbol_index(result, "field", "_private");
    int name_index = find_symbol_index(result, "field", "name");
    REQUIRE(core_index >= 0);
    REQUIRE(private_index >= 0);
    REQUIRE(name_index >= 0);

    auto contains_edge = [&](int dst_index) {
        return std::any_of(result.edges.begin(), result.edges.end(),
            [&](const ExtractedEdge& edge) {
                return edge.kind == "contains" &&
                       edge.src_index == core_index &&
                       edge.dst_index == dst_index &&
                       edge.evidence == "constructor_field";
            });
    };

    REQUIRE(contains_edge(private_index));
    REQUIRE(contains_edge(name_index));
}

TEST_CASE("JavaScript assignment-based constructors become constructor_fn",
          "[unit][javascript][extractor]") {
    std::string source = R"(Foo = function() {
  this.value = 1;
};

module.exports = function ExportedThing() {
  this.ready = true;
};)";

    auto result = extract_javascript(source);

    REQUIRE(find_symbol(result, "constructor_fn", "Foo") != nullptr);
    REQUIRE(find_symbol(result, "field", "value") != nullptr);

    auto* exported = find_symbol(result, "constructor_fn", "ExportedThing");
    REQUIRE(exported != nullptr);
    REQUIRE(exported->qualname == "ExportedThing");

    auto* ready = find_symbol(result, "field", "ready");
    REQUIRE(ready != nullptr);
    REQUIRE(ready->qualname == "ExportedThing._fields.ready");
    REQUIRE(ready->start_line == 6);
    REQUIRE(ready->end_line == 6);
}
