#include "core/arena_ledger.h"

#ifdef ARENA_LEDGER

#include "core/arena.h"
#include <cstdio>
#include <cstring>
#include <csignal>
#include <pthread.h>
#include <mutex>
#include <vector>
#include <unistd.h>  // STDERR_FILENO, write()

namespace codetopo {

// Thread-local ring buffer
thread_local LedgerEntry  t_ledger[LEDGER_SIZE] = {};
thread_local int          t_ledger_pos   = 0;
thread_local int          t_ledger_count = 0;

// Registry so crash handler can dump all threads
static std::mutex          s_reg_mutex;
static std::vector<pthread_t> s_registered_threads;

// Register current thread (called from ledger_push or set_thread_arena)
static void register_thread() {
    static thread_local bool registered = false;
    if (registered) return;
    registered = true;
    std::lock_guard<std::mutex> lk(s_reg_mutex);
    s_registered_threads.push_back(pthread_self());
}

void ledger_update_arena_state(LedgerEntry* entry, size_t used, size_t cap, bool overflow) {
    // Store used in KB (fits in uint32_t for reasonable arena sizes)
    entry->arena_used = static_cast<uint32_t>(used / 1024);
    entry->arena_cap  = static_cast<uint32_t>(cap  / 1024);
    if (overflow) entry->size |= 0x80000000u;  // flag overflow in high bit
}

static const char* event_name(LedgerEvent ev) {
    switch (ev) {
        case LedgerEvent::SET_ARENA:   return "SET_ARENA ";
        case LedgerEvent::CLEAR_ARENA: return "CLR_ARENA ";
        case LedgerEvent::TS_MALLOC:      return "malloc    ";
        case LedgerEvent::TS_CALLOC:      return "calloc    ";
        case LedgerEvent::TS_REALLOC:     return "realloc   ";
        case LedgerEvent::TS_FREE:        return "free      ";
        case LedgerEvent::ARENA_OVERFLOW:    return "OVERFLOW! ";
        case LedgerEvent::ARENA_OOM:         return "OOM!!!    ";
        default:                       return "?         ";
    }
}

void arena_ledger_dump(const char* prefix) {
    char buf[256];
    int total = t_ledger_count;
    int start = total >= LEDGER_SIZE ? t_ledger_pos : 0;
    int count = total >= LEDGER_SIZE ? LEDGER_SIZE : total;

    snprintf(buf, sizeof(buf),
        "%s[LEDGER] thread=%p  total_events=%d  showing last %d:\n",
        prefix ? prefix : "",
        (void*)pthread_self(), total, count);
    fputs(buf, stderr);

    for (int i = 0; i < count; i++) {
        const LedgerEntry& e = t_ledger[(start + i) % LEDGER_SIZE];
        bool overflow_flag = (e.size & 0x80000000u) != 0;
        uint32_t sz = e.size & 0x7FFFFFFFu;

        snprintf(buf, sizeof(buf),
            "%s  [%4d] %s  sz=%-8u  arena=%p  used=%uKB/%uKB  result=%p%s\n",
            prefix ? prefix : "",
            (total >= LEDGER_SIZE ? (start + i - LEDGER_SIZE + count) : i),
            event_name(e.event),
            sz,
            (void*)e.arena_ptr,
            e.arena_used, e.arena_cap,
            (void*)e.result,
            overflow_flag ? "  [OVERFLOW]" : "");
        fputs(buf, stderr);
    }
}

static void sigsegv_handler(int /*sig*/) {
    const char* msg = "\n=== SIGSEGV — Arena Ledger Dump ===\n";
    write(STDERR_FILENO, msg, strlen(msg));
    arena_ledger_dump("CRASH ");
    // Re-raise with default handler
    signal(SIGSEGV, SIG_DFL);
    raise(SIGSEGV);
}

void arena_ledger_install_crash_handler() {
    struct sigaction sa = {};
    sa.sa_handler = sigsegv_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;  // one-shot, then default handler
    sigaction(SIGSEGV, &sa, nullptr);
    register_thread();
}

} // namespace codetopo

#endif // ARENA_LEDGER
