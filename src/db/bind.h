#pragma once

#include <sqlite3.h>
#include <string>
#include <string_view>
#include <optional>
#include <utility>
#include <concepts>
#include <type_traits>
#include <cstdint>
#include <cstddef>

namespace codetopo::db {

// Type traits for bindable SQLite parameters
template <typename T>
struct is_bindable : std::false_type {};

template <typename T>
    requires std::integral<T> || std::floating_point<T>
struct is_bindable<T> : std::true_type {};

template <> struct is_bindable<std::string> : std::true_type {};
template <> struct is_bindable<std::string_view> : std::true_type {};
template <> struct is_bindable<const char*> : std::true_type {};
template <> struct is_bindable<char*> : std::true_type {};
template <size_t N> struct is_bindable<char[N]> : std::true_type {};
template <size_t N> struct is_bindable<const char[N]> : std::true_type {};
template <> struct is_bindable<std::nullptr_t> : std::true_type {};
template <typename T> struct is_bindable<std::optional<T>> : is_bindable<std::remove_cvref_t<T>> {};

template <typename T>
inline constexpr bool is_bindable_v = is_bindable<std::remove_cvref_t<T>>::value;

// Single-value SQLite binders
template <typename T>
    requires std::integral<T> && (!std::is_same_v<T, bool>)
inline void bind_one(sqlite3_stmt* stmt, int idx, T val) {
    if constexpr (sizeof(T) <= sizeof(int)) {
        sqlite3_bind_int(stmt, idx, static_cast<int>(val));
    } else {
        sqlite3_bind_int64(stmt, idx, static_cast<int64_t>(val));
    }
}

template <typename T>
    requires std::floating_point<T>
inline void bind_one(sqlite3_stmt* stmt, int idx, T val) {
    sqlite3_bind_double(stmt, idx, static_cast<double>(val));
}

inline void bind_one(sqlite3_stmt* stmt, int idx, bool val) {
    sqlite3_bind_int(stmt, idx, val ? 1 : 0);
}

inline void bind_one(sqlite3_stmt* stmt, int idx, const std::string& val) {
    sqlite3_bind_text(stmt, idx, val.c_str(), static_cast<int>(val.size()), SQLITE_TRANSIENT);
}

inline void bind_one(sqlite3_stmt* stmt, int idx, std::string_view val) {
    sqlite3_bind_text(stmt, idx, val.data(), static_cast<int>(val.size()), SQLITE_TRANSIENT);
}

inline void bind_one(sqlite3_stmt* stmt, int idx, const char* val) {
    if (val) {
        sqlite3_bind_text(stmt, idx, val, -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, idx);
    }
}

inline void bind_one(sqlite3_stmt* stmt, int idx, std::nullptr_t) {
    sqlite3_bind_null(stmt, idx);
}

template <typename T>
inline void bind_one(sqlite3_stmt* stmt, int idx, const std::optional<T>& val) {
    if (val.has_value()) {
        bind_one(stmt, idx, *val);
    } else {
        sqlite3_bind_null(stmt, idx);
    }
}

// C++26 Pack-indexed statement binder
// Binds all variadic arguments to 1-based parameter indices with diagnostic static_assert
template <typename... Args>
void bind_all(sqlite3_stmt* stmt, const Args&... args) {
    constexpr size_t N = sizeof...(Args);
    if constexpr (N > 0) {
        [&]<size_t... Is>(std::index_sequence<Is...>) {
            (
                []<typename T>(sqlite3_stmt* s, int idx, const T& val) {
                    static_assert(
                        is_bindable_v<T>,
                        "Type cannot be bound to SQLite statement. Supported types: "
                        "integral types, floating point, bool, std::string, "
                        "std::string_view, const char*, std::nullptr_t, or std::optional<T>."
                    );
                    if constexpr (is_bindable_v<T>) {
                        bind_one(s, idx, val);
                    }
                }(stmt, static_cast<int>(Is + 1), args...[Is]),
                ...
            );
        }(std::make_index_sequence<N>{});
    }
}

// Reset, clear existing bindings, and bind all parameters
template <typename... Args>
void bind(sqlite3_stmt* stmt, const Args&... args) {
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    bind_all(stmt, args...);
}

} // namespace codetopo::db
