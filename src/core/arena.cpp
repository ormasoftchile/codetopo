#include "core/arena.h"
#include <tree_sitter/api.h>

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

// C-linkage wrappers for ts_set_allocator()
static void* ts_arena_malloc(size_t size) {
    if (!t_current_arena) return nullptr;
    return arena_malloc(*t_current_arena, size);
}

static void* ts_arena_calloc(size_t count, size_t size) {
    if (!t_current_arena) return nullptr;
    return arena_calloc(*t_current_arena, count, size);
}

static void* ts_arena_realloc(void* ptr, size_t new_size) {
    if (!t_current_arena) return nullptr;
    return arena_realloc(*t_current_arena, ptr, new_size);
}

static void ts_arena_free(void* ptr) {
    arena_free(ptr);
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
