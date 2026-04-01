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

// C-linkage wrappers for ts_set_allocator() — inline definitions are in arena.h.
// This file only provides the thread-local storage, set/get accessors, and registration.

void register_arena_allocator() {
    ts_set_allocator(
        ts_arena_malloc,
        ts_arena_calloc,
        ts_arena_realloc,
        ts_arena_free
    );
}

} // namespace codetopo
