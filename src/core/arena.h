#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>

namespace codetopo {

// T006: Arena bump allocator with malloc/calloc/realloc(copy-based)/free(no-op)
// and O(1) reset. Used as the per-thread memory pool for Tree-sitter and
// symbol extraction. See research.md R1.
class Arena {
public:
    explicit Arena(size_t capacity)
        : buffer_(new uint8_t[capacity])
        , capacity_(capacity)
        , offset_(0) {}

    ~Arena() { delete[] buffer_; }

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    // Bump-allocate `size` bytes, aligned to `alignment`.
    // Returns nullptr if arena capacity exceeded.
    void* allocate(size_t size, size_t alignment = alignof(std::max_align_t)) {
        size_t current = offset_;
        size_t aligned = (current + alignment - 1) & ~(alignment - 1);
        size_t new_offset = aligned + size;

        if (new_offset > capacity_) {
            return nullptr;  // Overflow — caller handles partial parse
        }

        offset_ = new_offset;
        return buffer_ + aligned;
    }

    // calloc semantics: allocate + zero-fill
    void* allocate_zeroed(size_t count, size_t size) {
        size_t total = count * size;
        void* ptr = allocate(total);
        if (ptr) {
            std::memset(ptr, 0, total);
        }
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
        // old_ptr is orphaned — reclaimed on reset()
        return new_ptr;
    }

    // O(1) reset — reclaims all allocated memory instantly.
    void reset() {
        offset_ = 0;
    }

    size_t capacity() const { return capacity_; }
    size_t used() const { return offset_; }
    size_t remaining() const { return capacity_ - offset_; }
    bool overflowed() const { return false; }  // We return nullptr on overflow

private:
    uint8_t* buffer_;
    size_t capacity_;
    size_t offset_;
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

    auto* header = reinterpret_cast<ArenaAllocHeader*>(
        static_cast<uint8_t*>(ptr) - HEADER_SIZE);
    size_t old_size = header->size;

    void* new_raw = arena.allocate(new_size + HEADER_SIZE);
    if (!new_raw) return nullptr;

    auto* new_header = static_cast<ArenaAllocHeader*>(new_raw);
    new_header->size = new_size;
    void* new_data = reinterpret_cast<uint8_t*>(new_raw) + HEADER_SIZE;

    std::memcpy(new_data, ptr, old_size < new_size ? old_size : new_size);
    return new_data;
}

inline void arena_free(void* /*ptr*/) {
    // No-op — arena reset reclaims all memory
}

} // namespace codetopo
