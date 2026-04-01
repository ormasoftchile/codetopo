#include "core/arena.h"
#include <tree_sitter/api.h>
#include <cstdlib>

namespace codetopo {

// T008: Thread-local arena pointer for Tree-sitter allocator dispatch.
// ts_set_allocator() is global, so we use thread_local to route each
// thread's allocations to its own leased arena.
thread_local Arena* t_current_arena = nullptr;

void set_thread_arena(Arena* arena) {
    t_current_arena = arena;
}

Arena* get_thread_arena() {
    return t_current_arena;
}

void register_arena_allocator() {
    ts_set_allocator(
        ts_arena_malloc,
        ts_arena_calloc,
        ts_arena_realloc,
        ts_arena_free
    );
}

} // namespace codetopo

// C-linkage wrappers that dispatch to the thread-local arena.
// Fallback to stdlib if no arena is set or arena is full.
// Uses Arena::contains() to correctly route realloc/free for pointers
// that may have been allocated by stdlib (arena overflow) vs arena.
extern "C" {

void* ts_arena_malloc(size_t size) {
    auto* arena = codetopo::get_thread_arena();
    if (arena) {
        void* p = codetopo::arena_malloc(*arena, size);
        if (p) return p;
        // Arena full — fall back to stdlib (no header prepended)
    }
    return std::malloc(size);
}

void* ts_arena_calloc(size_t count, size_t size) {
    auto* arena = codetopo::get_thread_arena();
    if (arena) {
        void* p = codetopo::arena_calloc(*arena, count, size);
        if (p) return p;
    }
    return std::calloc(count, size);
}

void* ts_arena_realloc(void* ptr, size_t new_size) {
    auto* arena = codetopo::get_thread_arena();
    if (arena) {
        if (!ptr) {
            void* p = codetopo::arena_malloc(*arena, new_size);
            if (p) return p;
            return std::malloc(new_size);
        }
        if (arena->contains(ptr)) {
            void* p = codetopo::arena_realloc(*arena, ptr, new_size);
            if (p) return p;
            // Arena full — copy from arena to stdlib allocation
            void* new_p = std::malloc(new_size);
            if (!new_p) return nullptr;
            auto* header = reinterpret_cast<codetopo::ArenaAllocHeader*>(
                static_cast<uint8_t*>(ptr) - codetopo::HEADER_SIZE);
            size_t old_size = header->size;
            std::memcpy(new_p, ptr, old_size < new_size ? old_size : new_size);
            return new_p;
        }
        // ptr is from stdlib (previous overflow) — use stdlib realloc
        return std::realloc(ptr, new_size);
    }
    return std::realloc(ptr, new_size);
}

void ts_arena_free(void* ptr) {
    if (!ptr) return;
    auto* arena = codetopo::get_thread_arena();
    if (arena && arena->contains(ptr)) {
        return;  // Arena pointer — no-op, reclaimed on reset
    }
    std::free(ptr);
}

} // extern "C"
