#pragma once

#include <sqlite3.h>
#include <string>
#include <string_view>
#include <optional>
#include <utility>
#include <cstdint>
#include <cstddef>

namespace codetopo::db {

// Single-value SQLite binders
inline void bind_one(sqlite3_stmt* stmt, int idx, int val) {
    sqlite3_bind_int(stmt, idx, val);
}

inline void bind_one(sqlite3_stmt* stmt, int idx, int64_t val) {
    sqlite3_bind_int64(stmt, idx, val);
}

inline void bind_one(sqlite3_stmt* stmt, int idx, uint64_t val) {
    sqlite3_bind_int64(stmt, idx, static_cast<int64_t>(val));
}

inline void bind_one(sqlite3_stmt* stmt, int idx, bool val) {
    sqlite3_bind_int(stmt, idx, val ? 1 : 0);
}

inline void bind_one(sqlite3_stmt* stmt, int idx, double val) {
    sqlite3_bind_double(stmt, idx, val);
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
// Binds all variadic arguments to 1-based parameter indices without recursion
template <typename... Args>
void bind_all(sqlite3_stmt* stmt, const Args&... args) {
    constexpr size_t N = sizeof...(Args);
    if constexpr (N > 0) {
        [&]<size_t... Is>(std::index_sequence<Is...>) {
            (bind_one(stmt, static_cast<int>(Is + 1), args...[Is]), ...);
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
