/// @file config.cpp
/// @brief Runtime configuration initialization from environment variables.

#include "ammalloc/config.h"
#include "ammalloc/common.h"

#include <cctype>
#include <cstdlib>

namespace ammalloc {

void RuntimeConfig::InitFromEnv() {
    if (const char* env = std::getenv("AM_TC_SIZE")) {
        if (const auto val = detail::ParseSize(env); val > 0) {
            max_tc_size_ = val < SizeConfig::MAX_TC_SIZE ? val : SizeConfig::MAX_TC_SIZE;
        }
    }

    if (const char* env = std::getenv("AM_USE_MAP_POPULATE")) {
        use_map_populate_ = detail::ParseBool(env);
    }

    if (const char* env = std::getenv("AM_ENABLE_SCAVENGER")) {
        enable_scavenger_ = detail::ParseBool(env);
    }
}

}// namespace ammalloc
