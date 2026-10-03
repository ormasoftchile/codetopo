#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <stdexcept>
#include <new>
#include <iterator>
#include <initializer_list>
#include <algorithm>

#if __has_include(<inplace_vector>) && defined(__cpp_lib_inplace_vector)
#include <inplace_vector>
namespace codetopo {
template <typename T, size_t N>
using inplace_vector = std::inplace_vector<T, N>;
}
#else

namespace codetopo {

// C++26 std::inplace_vector polyfill (P0843R14)
// A sequence container with contiguous fixed-capacity storage and dynamic size.
// Operates with zero heap allocations.
template <typename T, size_t N>
class inplace_vector {
public:
    using value_type = T;
    using size_type = size_t;
    using difference_type = ptrdiff_t;
    using reference = value_type&;
    using const_reference = const value_type&;
    using pointer = value_type*;
    using const_pointer = const value_type*;
    using iterator = pointer;
    using const_iterator = const_pointer;
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    constexpr inplace_vector() noexcept : size_(0) {}

    constexpr inplace_vector(size_type count, const T& value) : size_(0) {
        if (count > N) {
            throw std::bad_alloc();
        }
        for (size_type i = 0; i < count; ++i) {
            push_back(value);
        }
    }

    constexpr explicit inplace_vector(size_type count) : size_(0) {
        if (count > N) {
            throw std::bad_alloc();
        }
        for (size_type i = 0; i < count; ++i) {
            emplace_back();
        }
    }

    template <std::input_iterator InputIt>
    constexpr inplace_vector(InputIt first, InputIt last) : size_(0) {
        for (; first != last; ++first) {
            push_back(*first);
        }
    }

    constexpr inplace_vector(std::initializer_list<T> init) : size_(0) {
        if (init.size() > N) {
            throw std::bad_alloc();
        }
        for (const auto& item : init) {
            push_back(item);
        }
    }

    ~inplace_vector() {
        clear();
    }

    inplace_vector(const inplace_vector& other) : size_(0) {
        for (size_type i = 0; i < other.size_; ++i) {
            push_back(other[i]);
        }
    }

    inplace_vector(inplace_vector&& other) noexcept(std::is_nothrow_move_constructible_v<T>) : size_(0) {
        for (size_type i = 0; i < other.size_; ++i) {
            push_back(std::move(other[i]));
        }
    }

    inplace_vector& operator=(const inplace_vector& other) {
        if (this != &other) {
            clear();
            for (size_type i = 0; i < other.size_; ++i) {
                push_back(other[i]);
            }
        }
        return *this;
    }

    inplace_vector& operator=(inplace_vector&& other) noexcept(
        std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T>) {
        if (this != &other) {
            clear();
            for (size_type i = 0; i < other.size_; ++i) {
                push_back(std::move(other[i]));
            }
        }
        return *this;
    }

    constexpr size_type size() const noexcept { return size_; }
    constexpr size_type max_size() const noexcept { return N; }
    constexpr size_type capacity() const noexcept { return N; }
    constexpr bool empty() const noexcept { return size_ == 0; }

    pointer data() noexcept {
        return reinterpret_cast<pointer>(&storage_);
    }

    const_pointer data() const noexcept {
        return reinterpret_cast<const_pointer>(&storage_);
    }

    reference operator[](size_type pos) noexcept {
        return data()[pos];
    }

    const_reference operator[](size_type pos) const noexcept {
        return data()[pos];
    }

    reference at(size_type pos) {
        if (pos >= size_) {
            throw std::out_of_range("inplace_vector::at out of range");
        }
        return data()[pos];
    }

    const_reference at(size_type pos) const {
        if (pos >= size_) {
            throw std::out_of_range("inplace_vector::at out of range");
        }
        return data()[pos];
    }

    reference front() noexcept { return data()[0]; }
    const_reference front() const noexcept { return data()[0]; }
    reference back() noexcept { return data()[size_ - 1]; }
    const_reference back() const noexcept { return data()[size_ - 1]; }

    iterator begin() noexcept { return data(); }
    const_iterator begin() const noexcept { return data(); }
    const_iterator cbegin() const noexcept { return data(); }
    iterator end() noexcept { return data() + size_; }
    const_iterator end() const noexcept { return data() + size_; }
    const_iterator cend() const noexcept { return data() + size_; }

    reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
    const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }
    reverse_iterator rend() noexcept { return reverse_iterator(begin()); }
    const_reverse_iterator rend() const noexcept { return const_reverse_iterator(begin()); }

    template <typename... Args>
    reference emplace_back(Args&&... args) {
        if (size_ >= N) {
            throw std::bad_alloc();
        }
        pointer ptr = ::new (static_cast<void*>(data() + size_)) T(std::forward<Args>(args)...);
        ++size_;
        return *ptr;
    }

    reference push_back(const T& value) {
        return emplace_back(value);
    }

    reference push_back(T&& value) {
        return emplace_back(std::move(value));
    }

    void pop_back() noexcept {
        if (size_ > 0) {
            --size_;
            std::destroy_at(data() + size_);
        }
    }

    void clear() noexcept {
        while (size_ > 0) {
            pop_back();
        }
    }

    void swap(inplace_vector& other) noexcept(std::is_nothrow_swappable_v<T>) {
        inplace_vector tmp = std::move(*this);
        *this = std::move(other);
        other = std::move(tmp);
    }

private:
    alignas(T) std::byte storage_[N > 0 ? N * sizeof(T) : 1];
    size_type size_ = 0;
};

template <typename T, size_t N>
inline void swap(inplace_vector<T, N>& lhs, inplace_vector<T, N>& rhs) noexcept(noexcept(lhs.swap(rhs))) {
    lhs.swap(rhs);
}

} // namespace codetopo

#endif
