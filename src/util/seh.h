#pragma once

// MinGW supports Windows SEH, but not MSVC's SEH-to-C++ translator.
// Other compilers rely on the index supervisor for native fault recovery.
#if defined(_WIN32) && defined(_MSC_VER)
#define CODETOPO_HAS_SEH_TRANSLATOR 1

#include <exception>
#include <windows.h>
#include <eh.h>
#include <malloc.h>

namespace codetopo {

struct SehException : std::exception {
    DWORD code;
    SehException(DWORD c) : code(c) {}
    const char* what() const noexcept override { return "SEH exception"; }
};

inline void seh_translator(unsigned int code, EXCEPTION_POINTERS*) {
    // Restore the guard page or the next overflow triggers uncatchable fastfail.
    if (code == EXCEPTION_STACK_OVERFLOW) {
        _resetstkoflw();
    }
    throw SehException(static_cast<DWORD>(code));
}

} // namespace codetopo
#else
#define CODETOPO_HAS_SEH_TRANSLATOR 0
#endif
