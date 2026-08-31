#ifndef AMMALLOC_AMMALLOC_H
#define AMMALLOC_AMMALLOC_H

/// @file ammalloc.h
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
/// @pre `ptr` is null or the original live pointer returned by `am_malloc`.
/// @note Pointers not recognized by the allocator's page map are ignored.
///       Interior pointers and double frees are unsupported even when a
///       hardened build rejects some of them.
void am_free(void* ptr);

/// @brief Soft-trims the calling thread's ThreadCache without creating one.
///
/// This is an owner-thread safepoint API: it preserves a bounded warm working
/// set and returns evicted objects through CentralCache for reuse.
void am_thread_cache_trim() noexcept;

/// @brief Purges the calling thread's ThreadCache and drains TransferCache.
///
/// Local objects bypass TransferCache and return to Span bitmaps. Existing
/// TransferCache snapshots are then drained under their bucket locks. Free
/// spans become eligible for the normal PageCache scavenger; cached bytes are
/// not a direct RSS measurement.
/// @note A purge contracts per-class quotas to actual occupancy (empty classes
///       return to the one-object slow-start floor), so post-purge demand
///       re-warms through slow-start. Prefer `am_thread_cache_trim()` at
///       latency-sensitive safepoints.
void am_thread_cache_purge() noexcept;

/// @brief Publishes a cooperative soft trim for live ThreadCache owners.
///
/// The request is observed only at owner-thread slow paths or explicit calls to
/// `am_thread_cache_trim`; it never lets a controller touch another TLS cache.
void am_request_thread_cache_trim() noexcept;

/// @brief Publishes a cooperative hard purge for live ThreadCache owners.
///
/// The request is observed only at owner-thread slow paths or explicit calls to
/// `am_thread_cache_purge`; it never lets a controller touch another TLS cache.
void am_request_thread_cache_purge() noexcept;

}// namespace ammalloc

#endif // AMMALLOC_AMMALLOC_H
