#include "ammalloc/noalloc_diagnostics.h"

#include <cstdlib>
#include <unistd.h>

namespace ammalloc::detail {
namespace {

void WriteLiteral(const char* text) noexcept {
    size_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    // Diagnostics are best effort; retrying a partial write can itself block
    // indefinitely after allocator infrastructure has already failed.
    static_cast<void>(::write(STDERR_FILENO, text, length));
}

}// namespace

AM_NORETURN void FatalNoAlloc(const char* message) noexcept {
    WriteLiteral("ammalloc fatal: ");
    WriteLiteral(message);
    WriteLiteral("\n");
    std::abort();
}

// LockOrFatal and UnlockOrFatal are inline templates declared in
// noalloc_diagnostics.h so they can be instantiated for both std::mutex and
// the instrumented wrapper without duplicating the try/catch dispatch here.

}// namespace ammalloc::detail
