#ifndef AMMALLOC_CONFIG_H
#define AMMALLOC_CONFIG_H

/// @file
/// @brief Compile-time allocator constants and environment-derived runtime options.

#include "ammalloc/attributes.h"

#include <cstddef>
#include <new>

namespace ammalloc {

/// @brief System page, address-space, cache-line, and alignment constants.
struct SystemConfig {
    constexpr static size_t PAGE_SIZE = 4096;
    constexpr static size_t PAGE_SHIFT = 12;
    constexpr static size_t HUGE_PAGE_SIZE = 2 * 1024 * 1024;
    constexpr static size_t CACHE_LINE_SIZE = 64;
    constexpr static size_t BITMAP_BITS = 64;
    constexpr static size_t ALIGNMENT = 16;

#ifdef AM_USE_57BIT_VA
    static constexpr size_t VA_BITS = 57;
#else
    static constexpr size_t VA_BITS = 48;
#endif

    constexpr static size_t PAGE_ID_BITS = VA_BITS - PAGE_SHIFT;
};

/// @brief Size-class geometry and ThreadCache limits.
struct SizeConfig {
    constexpr static size_t MAX_TC_SIZE = 32 * 1024;
    constexpr static int kStepsPerGroup = 4;
    constexpr static int kStepShift = 2;
    constexpr static size_t kSmallSizeThreshold = 1024;
};

/// @brief PageCache, radix-tree, retry, and huge-page-cache limits.
struct PageConfig {
    /// Largest span retained in PageCache buckets; larger spans return to the OS.
    constexpr static size_t MAX_PAGE_NUM = 128;
    constexpr static size_t RADIX_NODE_BITS = 9;
    constexpr static size_t RADIX_NODE_SIZE = 1 << RADIX_NODE_BITS;
    constexpr static size_t RADIX_ROOT_BITS = SystemConfig::PAGE_ID_BITS - 3 * RADIX_NODE_BITS;
    constexpr static size_t RADIX_ROOT_SIZE = 1 << RADIX_ROOT_BITS;
    constexpr static size_t RADIX_MASK = RADIX_NODE_SIZE - 1;
    constexpr static size_t MAX_ALLOC_RETRIES = 3;
    static constexpr size_t HUGE_PAGE_CACHE_SIZE = 16;
};

/// @brief Process-wide runtime settings initialized from environment variables.
///
/// Construction uses static storage and placement new so configuration lookup
/// cannot recurse into the allocator. Values are immutable after initialization.
class RuntimeConfig {
public:
    /// @brief Returns the process-wide runtime configuration.
    /// @return Reference to the lazily initialized singleton.
    static RuntimeConfig& GetInstance() {
        // Static storage avoids recursive allocation during initialization.
        alignas(alignof(RuntimeConfig)) static char storage[sizeof(RuntimeConfig)];
        static auto* instance = new (storage) RuntimeConfig();
        return *instance;
    }

    /// @brief Returns the configured ThreadCache size limit.
    /// @return Maximum ThreadCache allocation size in bytes.
    AM_NODISCARD size_t MaxTCSize() const {
        return max_tc_size_;
    }

    /// @brief Reports whether eager mapping population is enabled.
    /// @return True when `AM_USE_MAP_POPULATE` parsed as enabled.
    AM_NODISCARD bool UseMapPopulate() const {
        return use_map_populate_;
    }

    /// @brief Reports whether the background page scavenger is enabled.
    /// @return True unless `AM_ENABLE_SCAVENGER` disables it.
    AM_NODISCARD bool EnableScavenger() const {
        return enable_scavenger_;
    }

private:
    RuntimeConfig() {
        InitFromEnv();
    }

    void InitFromEnv();

    size_t max_tc_size_ = SizeConfig::MAX_TC_SIZE;
    bool use_map_populate_ = false;
    bool enable_scavenger_ = true;
};

}// namespace ammalloc

#endif// AMMALLOC_CONFIG_H
