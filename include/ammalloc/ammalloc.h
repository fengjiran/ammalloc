#ifndef AMMALLOC_AMMALLOC_H
#define AMMALLOC_AMMALLOC_H

/// @file
/// @brief Public allocation and deallocation entry points for ammalloc.
/// @see docs/designs/ammalloc_design.md, docs/api/public-api.md

#include <cstddef>

namespace ammalloc {

/// @brief Allocates storage for at least `original_size` bytes.
/// @param original_size Requested size in bytes. A zero-size request is mapped
///        to the allocator's minimum size class.
/// @return Pointer to allocated storage, or null when allocation fails.
void* am_malloc(size_t original_size);

/// @brief Returns storage previously allocated by `am_malloc` to the allocator.
/// @param ptr Pointer returned by `am_malloc`; null is accepted as a no-op.
/// @note Pointers not recognized by the allocator's page map are ignored.
void am_free(void* ptr);

}// namespace ammalloc

#endif// AMMALLOC_AMMALLOC_H
