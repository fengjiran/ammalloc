/// @file span.cpp
/// @brief Bitmap-managed small-object allocation within a contiguous page span.

#include "ammalloc/span.h"
#include "ammalloc/size_class.h"

namespace ammalloc {

void Span::Init(size_t aligned_object_size) noexcept {
    // Creation-time invariant, active in every build: a Span may only carve an
    // exact size-class grid, so free-time slot math and bucket indexing always
    // agree. Abort here rather than carving a misaligned grid.
    AM_CHECK(aligned_object_size > 0 &&
             aligned_object_size <= SizeConfig::MAX_TC_SIZE);
    const size_t idx = SizeClass::Index(aligned_object_size);
    AM_CHECK(idx < SizeClass::kNumSizeClasses &&
             SizeClass::Size(idx) == aligned_object_size);

    aligned_obj_size = static_cast<uint32_t>(aligned_object_size);
    size_class_idx = static_cast<uint16_t>(idx);
    void* start_ptr = detail::PageIDToPtr(start_page_idx);

    const SpanLayout layout = ComputeSpanLayout(aligned_object_size, page_num);
    const size_t needed_bitmap_words =
            (layout.capacity >> SystemConfig::BITMAP_SHIFT) +
            ((layout.capacity & SystemConfig::BITMAP_MASK) != 0);
    AM_CHECK(layout.capacity > 0);
    AM_CHECK(layout.bitmap_num >= needed_bitmap_words);

    const size_t total_bytes = static_cast<size_t>(page_num) << SystemConfig::PAGE_SHIFT;
    AM_CHECK(layout.obj_offset <= total_bytes &&
             layout.capacity <= (total_bytes - layout.obj_offset) / aligned_object_size);

    obj_offset = layout.obj_offset;
    capacity = layout.capacity;
    auto* bitmap = new (start_ptr) uint64_t[layout.bitmap_num];

    // Initialize bitmap: set first 'capacity' bits to 1 (free).
    size_t full_bitmap_num = capacity >> SystemConfig::BITMAP_SHIFT;
    size_t tail_bits = capacity & SystemConfig::BITMAP_MASK;
    for (size_t i = 0; i < full_bitmap_num; ++i) {
        bitmap[i] = ~0ULL;
    }

    if (full_bitmap_num < layout.bitmap_num) {
        bitmap[full_bitmap_num] = tail_bits == 0 ? 0 : ((1ULL << tail_bits) - 1);
        for (size_t i = full_bitmap_num + 1; i < layout.bitmap_num; ++i) {
            bitmap[i] = 0;
        }
    }

    use_count = 0;
    scan_cursor = 0;
}

void* Span::AllocObject() noexcept {
    // clang-format off
    if (IsFull()) AM_UNLIKELY {
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

        size_t global_obj_idx = i * SystemConfig::BITMAP_BITS + bit_pos;
        return static_cast<char*>(GetDataBasePtr()) + global_obj_idx * aligned_obj_size;
    }
    return nullptr;
    // clang-format on
}

void Span::FreeObject(void* ptr) noexcept {
    // Use uintptr_t arithmetic so an arbitrary caller pointer does not trigger
    // undefined pointer subtraction before the range check can run.
    const auto base = reinterpret_cast<uintptr_t>(GetDataBasePtr());
    const auto uptr = reinterpret_cast<uintptr_t>(ptr);
    const size_t offset = uptr - base;
    AM_HCHECK(uptr >= base, "Pointer underflow detected!");
    AM_HCHECK(offset % aligned_obj_size == 0, "Pointer misaligned!");

    size_t global_obj_idx = 0;
    // clang-format off
    if (std::has_single_bit(static_cast<size_t>(aligned_obj_size))) AM_LIKELY {
        global_obj_idx = offset >> std::countr_zero(static_cast<size_t>(aligned_obj_size));
    } else {
        global_obj_idx = offset / aligned_obj_size;
    }
    // clang-format on

    AM_HCHECK(global_obj_idx < capacity, "Pointer overflow detected!");
    auto bitmap_idx = static_cast<uint32_t>(global_obj_idx >> SystemConfig::BITMAP_SHIFT);
    AM_HCHECK(bitmap_idx < GetBitmapNum());
    int bit_pos = static_cast<int>(global_obj_idx & SystemConfig::BITMAP_MASK);
    uint64_t mask = 1ull << bit_pos;
    auto* bitmap = GetBitmap();
    AM_HCHECK((bitmap[bitmap_idx] & mask) == 0, "double free detected.");
    AM_HCHECK(use_count > 0, "use_count underflow detected.");
    bitmap[bitmap_idx] |= mask;
    --use_count;
    scan_cursor = std::min(scan_cursor, bitmap_idx);
}

size_t Span::ObjectSlotOf(void* ptr) const noexcept {
    const auto base = reinterpret_cast<uintptr_t>(GetDataBasePtr());
    const auto uptr = reinterpret_cast<uintptr_t>(ptr);
    const size_t span_bytes = static_cast<size_t>(capacity) * aligned_obj_size;
    if (uptr < base || uptr - base >= span_bytes) {
        return std::numeric_limits<size_t>::max();
    }

    const size_t offset = uptr - base;
    if (offset % aligned_obj_size != 0) {
        return std::numeric_limits<size_t>::max();
    }
    return offset / aligned_obj_size;
}

namespace {

/// @brief Whether every size class carves at least one object per its span.
consteval bool EveryClassFitsAtLeastOneObject() {
    for (size_t i = 0; i < SizeClass::kNumSizeClasses; ++i) {
        const size_t obj_size = SizeClass::Size(i);
        if (ComputeSpanLayout(obj_size, SizeClass::GetMovePageNum(obj_size)).capacity == 0) {
            return false;
        }
    }
    return true;
}

/// @brief Whether the bitmap words Init reserves cover every object slot.
consteval bool BitmapCoversEverySlot() {
    for (size_t i = 0; i < SizeClass::kNumSizeClasses; ++i) {
        const size_t obj_size = SizeClass::Size(i);
        const SpanLayout layout = ComputeSpanLayout(obj_size, SizeClass::GetMovePageNum(obj_size));
        const size_t needed_words = (layout.capacity + SystemConfig::BITMAP_MASK) >>
                                    SystemConfig::BITMAP_SHIFT;
        if (needed_words > layout.bitmap_num) {
            return false;
        }
    }
    return true;
}

}// namespace

// Both invariants depend only on the page/batch tables. A zero-capacity span
// would make CentralCache request fresh spans until PageCache OOM, and an
// undersized bitmap would make AllocObject() read words outside the reserved
// bitmap area; failing here beats degrading silently.
static_assert(EveryClassFitsAtLeastOneObject(),
              "every size class must fit at least one object per span");
static_assert(BitmapCoversEverySlot(), "the span bitmap must cover every object slot");

}// namespace ammalloc
