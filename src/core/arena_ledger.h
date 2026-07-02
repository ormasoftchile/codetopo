#pragma once
// Arena allocation ledger — thread-local ring buffer that records the last
// N arena events for post-mortem debugging. Enabled by defining ARENA_LEDGER.
// Dump with arena_ledger_dump() or the SIGSEGV handler installed by
// arena_ledger_install_crash_handler().

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace codetopo {

#ifdef ARENA_LEDGER

constexpr int LEDGER_SIZE = 512; // ring buffer entries per thread

enum class LedgerEvent : uint8_t {
    SET_ARENA,    // set_thread_arena(non-null)
    CLEAR_ARENA,  // set_thread_arena(null)
    TS_MALLOC,    // ts_arena_malloc called
    TS_CALLOC,    // ts_arena_calloc called
    TS_REALLOC,   // ts_arena_realloc called
    TS_FREE,      // ts_arena_free called
    ARENA_OVERFLOW,     // arena bumped into overflow (malloc fallback)
    ARENA_OOM,          // malloc fallback returned null
};

struct LedgerEntry {
    LedgerEvent event;
    uint8_t     pad[3];
    uint32_t    size;          // requested size (0 for SET/CLEAR/FREE)
    uint32_t    arena_used;    // arena offset at time of call (MB*1000 + KB)
    uint32_t    arena_cap;     // arena capacity (bytes / 1024)
    uintptr_t   result;        // returned pointer (0 = null)
    uintptr_t   arena_ptr;     // arena pointer (identifies which arena)
};

extern thread_local LedgerEntry  t_ledger[LEDGER_SIZE];
extern thread_local int          t_ledger_pos;   // next write index (wraps)
extern thread_local int          t_ledger_count; // total entries written

inline void ledger_push(LedgerEvent ev, size_t sz, void* res, void* arena_raw) {
    // arena_raw is cast from Arena* in arena.cpp
    auto* entry = &t_ledger[t_ledger_pos % LEDGER_SIZE];
    entry->event      = ev;
    entry->size       = static_cast<uint32_t>(sz < 0xFFFFFFFF ? sz : 0xFFFFFFFF);
    entry->result     = reinterpret_cast<uintptr_t>(res);
    entry->arena_ptr  = reinterpret_cast<uintptr_t>(arena_raw);
    entry->arena_used = 0;
    entry->arena_cap  = 0;
    t_ledger_pos      = (t_ledger_pos + 1) % LEDGER_SIZE;
    t_ledger_count++;
}

// Fill arena_used / arena_cap from the Arena object.
// Called from arena.cpp where Arena internals are visible.
void ledger_update_arena_state(LedgerEntry* entry, size_t used, size_t cap, bool overflow);

// Dump the current thread's ledger to stderr.
void arena_ledger_dump(const char* prefix);

// Install a SIGSEGV handler that dumps all threads' ledgers on crash.
void arena_ledger_install_crash_handler();

#else  // ARENA_LEDGER not defined — all ops are no-ops

inline void arena_ledger_dump(const char*) {}
inline void arena_ledger_install_crash_handler() {}

#endif // ARENA_LEDGER

} // namespace codetopo
