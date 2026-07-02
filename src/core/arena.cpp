#include "core/arena.h"
#include "core/arena_ledger.h"
#include <tree_sitter/api.h>
#include <cstdlib>

namespace codetopo {

// T008: Thread-local arena pointer for Tree-sitter allocator dispatch.
// ts_set_allocator() is global, so we use thread_local to route each
// thread's allocations to its own leased arena.
thread_local Arena* t_current_arena = nullptr;

void set_thread_arena(Arena* arena) {
    t_current_arena = arena;
#ifdef ARENA_LEDGER
    if (arena) {
        ledger_push(LedgerEvent::SET_ARENA, 0, nullptr, arena);
        auto* e = &t_ledger[(t_ledger_pos - 1 + LEDGER_SIZE) % LEDGER_SIZE];
        ledger_update_arena_state(e, arena->used(), arena->capacity(), arena->overflowed());
    } else {
        ledger_push(LedgerEvent::CLEAR_ARENA, 0, nullptr, nullptr);
    }
#endif
}

Arena* get_thread_arena() {
    return t_current_arena;
}

// C-linkage wrappers for ts_set_allocator()
// IMPORTANT: tree-sitter may call these outside of a parse (e.g. lazy grammar
// table initialization triggered by new syntax patterns). When no arena is set,
// fall back to the system allocator so we don't return nullptr and crash.
static void* ts_arena_malloc(size_t size) {
    if (!t_current_arena) return nullptr;
    bool was_overflow = t_current_arena->overflowed();
    void* result = arena_malloc(*t_current_arena, size);
#ifdef ARENA_LEDGER
    bool now_overflow = t_current_arena->overflowed();
    if (!was_overflow && now_overflow)
        ledger_push(LedgerEvent::ARENA_OVERFLOW, size, result, t_current_arena);
    else
        ledger_push(LedgerEvent::TS_MALLOC, size, result, t_current_arena);
    if (!result)
        ledger_push(LedgerEvent::ARENA_OOM, size, nullptr, t_current_arena);
    auto* e = &t_ledger[(t_ledger_pos - 1 + LEDGER_SIZE) % LEDGER_SIZE];
    ledger_update_arena_state(e, t_current_arena->used(), t_current_arena->capacity(), now_overflow);
#endif
    return result;
}

static void* ts_arena_calloc(size_t count, size_t size) {
    if (!t_current_arena) return nullptr;
    bool was_overflow = t_current_arena->overflowed();
    void* result = arena_calloc(*t_current_arena, count, size);
#ifdef ARENA_LEDGER
    bool now_overflow = t_current_arena->overflowed();
    if (!was_overflow && now_overflow)
        ledger_push(LedgerEvent::ARENA_OVERFLOW, count * size, result, t_current_arena);
    else
        ledger_push(LedgerEvent::TS_CALLOC, count * size, result, t_current_arena);
    if (!result)
        ledger_push(LedgerEvent::ARENA_OOM, count * size, nullptr, t_current_arena);
    auto* e = &t_ledger[(t_ledger_pos - 1 + LEDGER_SIZE) % LEDGER_SIZE];
    ledger_update_arena_state(e, t_current_arena->used(), t_current_arena->capacity(), now_overflow);
#endif
    return result;
}

static void* ts_arena_realloc(void* ptr, size_t new_size) {
    if (!t_current_arena) return nullptr;
    void* result = arena_realloc(*t_current_arena, ptr, new_size);
#ifdef ARENA_LEDGER
    ledger_push(LedgerEvent::TS_REALLOC, new_size, result, t_current_arena);
    auto* e = &t_ledger[(t_ledger_pos - 1 + LEDGER_SIZE) % LEDGER_SIZE];
    ledger_update_arena_state(e, t_current_arena->used(), t_current_arena->capacity(), t_current_arena->overflowed());
#endif
    return result;
}

static void ts_arena_free(void* ptr) {
#ifdef ARENA_LEDGER
    ledger_push(LedgerEvent::TS_FREE, 0, ptr, t_current_arena);
#endif
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
