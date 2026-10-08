#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <type_traits>

namespace std {

#if !defined(__cpp_lib_saturation_arithmetic) || __cpp_lib_saturation_arithmetic < 202311L
// Polyfill for C++26 std::add_sat and std::mul_sat (P0543R3)
template <typename T>
constexpr T add_sat(T x, T y) noexcept {
    static_assert(std::is_integral_v<T>, "add_sat requires an integral type");
    if constexpr (std::is_unsigned_v<T>) {
        return (x > std::numeric_limits<T>::max() - y) ? std::numeric_limits<T>::max() : (x + y);
    } else {
        if (x > 0 && y > std::numeric_limits<T>::max() - x) return std::numeric_limits<T>::max();
        if (x < 0 && y < std::numeric_limits<T>::min() - x) return std::numeric_limits<T>::min();
        return x + y;
    }
}

template <typename T>
constexpr T mul_sat(T x, T y) noexcept {
    static_assert(std::is_integral_v<T>, "mul_sat requires an integral type");
    if constexpr (std::is_unsigned_v<T>) {
        if (x == 0 || y == 0) return 0;
        return (x > std::numeric_limits<T>::max() / y) ? std::numeric_limits<T>::max() : (x * y);
    } else {
        if (x == 0 || y == 0) return 0;
        if (x > 0) {
            if (y > 0 && y > std::numeric_limits<T>::max() / x) return std::numeric_limits<T>::max();
            if (y < 0 && y < std::numeric_limits<T>::min() / x) return std::numeric_limits<T>::min();
        } else {
            if (y > 0 && x < std::numeric_limits<T>::min() / y) return std::numeric_limits<T>::min();
            if (y < 0 && (x == std::numeric_limits<T>::min() || -x > std::numeric_limits<T>::max() / (-y)))
                return std::numeric_limits<T>::max();
        }
        return x * y;
    }
}
#endif

// Convenience aliases for callers using saturating_add / saturating_mul
template <typename T>
constexpr T saturating_add(T x, T y) noexcept {
    return std::add_sat(x, y);
}

template <typename T>
constexpr T saturating_mul(T x, T y) noexcept {
    return std::mul_sat(x, y);
}

} // namespace std
