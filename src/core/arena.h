#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>

namespace codetopo {

// T006: Arena bump allocator with malloc/calloc/realloc(copy-based)/free(no-op)
// and O(1) reset. Used as the per-thread memory pool for Tree-sitter and
// symbol extraction. See research.md R1.
//
// On overflow, falls back to stdlib malloc and sets an overflow flag.
// This prevents tree-sitter from receiving nullptr and crashing.
// The overflow flag lets callers detect and skip problematic files.
class Arena {
public:
    explicit Arena(size_t capacity)
        : buffer_(new uint8_t[capacity])
        , capacity_(capacity)
        , offset_(0)
        , overflowed_(false) {}

    ~Arena() {
        free_overflow_ptrs();
        delete[] buffer_;
    }

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    // Bump-allocate `size` bytes, aligned to `alignment`.
    // Falls back to malloc on overflow and sets overflowed_ flag.
    void* allocate(size_t size, size_t alignment = alignof(std::max_align_t)) {
        size_t current = offset_;
        size_t aligned = (current + alignment - 1) & ~(alignment - 1);
        size_t new_offset = aligned + size;

        if (new_offset > capacity_) {
            overflowed_ = true;
            void* p = std::malloc(size);
            if (p) overflow_ptrs_.push_back(p);
            return p;
        }

        offset_ = new_offset;
        return buffer_ + aligned;
    }

    // calloc semantics: allocate + zero-fill
    void* allocate_zeroed(size_t count, size_t size) {
        size_t total = count * size;
        size_t current = offset_;
        size_t aligned = (current + alignof(std::max_align_t) - 1)
                       & ~(alignof(std::max_align_t) - 1);
        size_t new_offset = aligned + total;

        if (new_offset > capacity_) {
            overflowed_ = true;
            void* p = std::calloc(count, size);
            if (p) overflow_ptrs_.push_back(p);
            return p;
        }

        offset_ = new_offset;
        void* ptr = buffer_ + aligned;
        std::memset(ptr, 0, total);
        return ptr;
    }

    // realloc semantics: allocate new block, copy old data, orphan old block.
    // `old_size` must be tracked by the caller (Tree-sitter doesn't expose it,
    // so we store it in a header — see arena_malloc below).
    void* reallocate(void* old_ptr, size_t old_size, size_t new_size) {
        if (!old_ptr) return allocate(new_size);
        if (new_size == 0) return nullptr;

        void* new_ptr = allocate(new_size);
        if (!new_ptr) return nullptr;

        std::memcpy(new_ptr, old_ptr, old_size < new_size ? old_size : new_size);
        // old_ptr is orphaned — reclaimed on reset() (arena) or free_overflow_ptrs() (malloc)
        return new_ptr;
    }

    // Check if ptr is within this arena's buffer.
    bool contains(const void* ptr) const {
        auto p = static_cast<const uint8_t*>(ptr);
        return p >= buffer_ && p < buffer_ + capacity_;
    }

    // O(1) reset — reclaims arena memory instantly and frees any overflow mallocs.
    void reset() {
        offset_ = 0;
        free_overflow_ptrs();
        overflowed_ = false;
    }

    size_t capacity() const { return capacity_; }
    size_t used() const { return offset_; }
    size_t remaining() const { return capacity_ - offset_; }
    bool overflowed() const { return overflowed_; }

private:
    void free_overflow_ptrs() {
        for (void* p : overflow_ptrs_) std::free(p);
        overflow_ptrs_.clear();
    }

    uint8_t* buffer_;
    size_t capacity_;
    size_t offset_;
    bool overflowed_;
    std::vector<void*> overflow_ptrs_;
};

// --- Allocation header for Tree-sitter compatibility ---
// Tree-sitter's realloc doesn't pass old_size, so we prepend a small header
// to each allocation that records the usable size.
struct ArenaAllocHeader {
    size_t size;
};

static constexpr size_t HEADER_SIZE = sizeof(ArenaAllocHeader);

// C-style allocator functions for ts_set_allocator() dispatch.
// These are called via thread_local arena pointer (see arena.cpp T008).
inline void* arena_malloc(Arena& arena, size_t size) {
    void* raw = arena.allocate(size + HEADER_SIZE);
    if (!raw) return nullptr;
    auto* header = static_cast<ArenaAllocHeader*>(raw);
    header->size = size;
    return reinterpret_cast<uint8_t*>(raw) + HEADER_SIZE;
}

inline void* arena_calloc(Arena& arena, size_t count, size_t size) {
    size_t total = count * size;
    void* raw = arena.allocate(total + HEADER_SIZE);
    if (!raw) return nullptr;
    auto* header = static_cast<ArenaAllocHeader*>(raw);
    header->size = total;
    std::memset(reinterpret_cast<uint8_t*>(raw) + HEADER_SIZE, 0, total);
    return reinterpret_cast<uint8_t*>(raw) + HEADER_SIZE;
}

inline void* arena_realloc(Arena& arena, void* ptr, size_t new_size) {
    if (!ptr) return arena_malloc(arena, new_size);
    if (new_size == 0) return nullptr;

    // Check if ptr came from this arena or from overflow malloc.
    // Tree-sitter may pass heap pointers when arena overflows mid-parse.
    size_t old_size;
    if (arena.contains(static_cast<uint8_t*>(ptr) - HEADER_SIZE)) {
        auto* header = reinterpret_cast<ArenaAllocHeader*>(
            static_cast<uint8_t*>(ptr) - HEADER_SIZE);
        old_size = header->size;
    } else {
        // Heap-allocated overflow pointer — header is still prepended
        auto* header = reinterpret_cast<ArenaAllocHeader*>(
            static_cast<uint8_t*>(ptr) - HEADER_SIZE);
        old_size = header->size;
    }

    void* new_raw = arena.allocate(new_size + HEADER_SIZE);
    if (!new_raw) return nullptr;

    auto* new_header = static_cast<ArenaAllocHeader*>(new_raw);
    new_header->size = new_size;
    void* new_data = reinterpret_cast<uint8_t*>(new_raw) + HEADER_SIZE;

    std::memcpy(new_data, ptr, old_size < new_size ? old_size : new_size);
    return new_data;
}

inline void arena_free(void* /*ptr*/) {
    // No-op — arena reset reclaims all memory (including overflow mallocs)
}

// Thread-local arena accessors (defined in arena.cpp)
void set_thread_arena(Arena* arena);
Arena* get_thread_arena();
void register_arena_allocator();

} // namespace codetopo
