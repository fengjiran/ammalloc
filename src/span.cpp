#include "ammalloc/span.h"
#include "ammalloc/size_class.h"

namespace ammalloc {

void Span::Init(size_t aligned_object_size) {
    AM_DCHECK(aligned_object_size > 0 && aligned_object_size <= std::numeric_limits<uint32_t>::max());
    aligned_obj_size = static_cast<uint32_t>(aligned_object_size);
    size_class_idx = SizeClass::Index(aligned_obj_size);
    void* start_ptr = detail::PageIDToPtr(start_page_idx);
    const size_t total_bytes = page_num << SystemConfig::PAGE_SHIFT;

    // Estimate bitmap + data layout:
    // Total = BitmapBytes(1 bit per object) + DataBytes(obj_size per object)
    size_t max_objs = (total_bytes * 8) / (aligned_obj_size * 8 + 1);
    size_t bitmap_num = (max_objs + 64 - 1) >> 6;
    auto* bitmap = new (start_ptr) uint64_t[bitmap_num];

    // Calculate data start with alignment padding.
    uintptr_t data_start = reinterpret_cast<uintptr_t>(bitmap) + bitmap_num * 8;
    data_start = (data_start + SystemConfig::ALIGNMENT - 1) & ~(SystemConfig::ALIGNMENT - 1);
    obj_offset = static_cast<uint32_t>(data_start - reinterpret_cast<uintptr_t>(start_ptr));

    // Capacity may be less than max_objs due to alignment overhead.
    uintptr_t data_end = reinterpret_cast<uintptr_t>(start_ptr) + total_bytes;
    capacity = (data_start >= data_end) ? 0 : (data_end - data_start) / aligned_obj_size;

    // Initialize bitmap: set first 'capacity' bits to 1 (free).
    size_t full_bitmap_num = capacity / 64;
    size_t tail_bits = capacity & 63;
    for (size_t i = 0; i < full_bitmap_num; ++i) {
        bitmap[i] = ~0ULL;
    }

    if (full_bitmap_num < bitmap_num) {
        bitmap[full_bitmap_num] = tail_bits == 0 ? 0 : ((1ULL << tail_bits) - 1);
        for (size_t i = full_bitmap_num + 1; i < bitmap_num; ++i) {
            bitmap[i] = 0;
        }
    }

    use_count = 0;
    scan_cursor = 0;
}

void* Span::AllocObject() {
    // clang-format off
    if (use_count >= capacity) AM_UNLIKELY {
        return nullptr;
    }

    auto bitmap_num = GetBitmapNum();
    auto* bitmap = GetBitmap();
    for (size_t i = scan_cursor; i < bitmap_num; ++i) {
        uint64_t val = bitmap[i];
        if (val == 0) AM_UNLIKELY {
            continue;
        }

        int bit_pos = std::countr_zero(val);
        val &= ~(1ull << bit_pos);
        bitmap[i] = val;
        ++use_count;
        scan_cursor = val == 0 ? static_cast<uint32_t>(i + 1) : static_cast<uint32_t>(i);

        size_t global_obj_idx = i * 64 + bit_pos;
        return static_cast<char*>(GetDataBasePtr()) + global_obj_idx * aligned_obj_size;
    }
    return nullptr;
    // clang-format on
}

void Span::FreeObject(void* ptr) {
    const char* base_ptr = static_cast<char*>(GetDataBasePtr());
    AM_DCHECK(static_cast<char*>(ptr) >= base_ptr, "Pointer underflow detected!");
    size_t offset = static_cast<char*>(ptr) - base_ptr;
    AM_DCHECK(offset % aligned_obj_size == 0);
    size_t global_obj_idx = 0;
    // clang-format off
    if (std::has_single_bit(static_cast<size_t>(aligned_obj_size))) AM_LIKELY {
        global_obj_idx = offset >> std::countr_zero(static_cast<size_t>(aligned_obj_size));
    } else {
        global_obj_idx = offset / aligned_obj_size;
    }
    // clang-format on

    auto bitmap_idx = static_cast<uint32_t>(global_obj_idx >> 6);
    AM_DCHECK(bitmap_idx < GetBitmapNum());
    int bit_pos = static_cast<int>(global_obj_idx & 63);
    uint64_t mask = 1ull << bit_pos;
    auto* bitmap = GetBitmap();
    AM_DCHECK((bitmap[bitmap_idx] & mask) == 0, "double free detected.");
    bitmap[bitmap_idx] |= mask;
    --use_count;
    scan_cursor = std::min(scan_cursor, bitmap_idx);
}

}// namespace ammalloc
