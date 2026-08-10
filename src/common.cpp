#include "ammalloc/common.h"

#include <cctype>
#include <cstdlib>
#include <limits>
#include <string_view>

namespace ammalloc {
namespace detail {

size_t ParseSize(const char* str) {
    if (!str || *str == '\0') {
        return 0;
    }
    char* end_ptr = nullptr;
    auto value = strtoul(str, &end_ptr, 10);

    if (str == end_ptr) {
        return 0;
    }

    while (*end_ptr && std::isspace(static_cast<unsigned char>(*end_ptr))) {
        ++end_ptr;
    }

    if (*end_ptr == '\0') {
        return value;
    }

    size_t multiplier = 1;
    switch (std::tolower(static_cast<unsigned char>(*end_ptr))) {
        case 'b':
            multiplier = 1;
            break;
        case 'k':
            multiplier = 1ULL << 10;
            break;
        case 'm':
            multiplier = 1ULL << 20;
            break;
        case 'g':
            multiplier = 1ULL << 30;
            break;
        case 't':
            multiplier = 1ULL << 40;
            break;
        default:
            return value;
    }

    // Saturate rather than wrapping an environment-provided byte count.
    if (value > std::numeric_limits<size_t>::max() / multiplier) {
        return std::numeric_limits<size_t>::max();
    }

    return multiplier * value;
}

bool ParseBool(const char* str) {
    if (!str) {
        return false;
    }

    std::string_view sv(str);
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) {
        sv.remove_prefix(1);
    }

    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) {
        sv.remove_suffix(1);
    }

    if (sv.empty()) {
        return false;
    }

    if (sv == "1") {
        return true;
    }

    auto is_equal = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) {
            return false;
        }

        return std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
            return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
        });
    };

    if (is_equal(sv, "true") ||
        is_equal(sv, "on") ||
        is_equal(sv, "yes")) {
        return true;
    }

    return false;
}

}// namespace detail
}// namespace ammalloc
