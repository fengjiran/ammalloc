#include "ammalloc/page_allocator.h"
#include "ammalloc/span.h"

#include <algorithm>
#include <cstdint>
#include <gtest/gtest.h>
#include <iterator>
#include <limits>
#include <type_traits>
#include <vector>

namespace {

using namespace ammalloc;

// Stack-local Span backed by a real SystemAlloc mapping. The destructor
// returns the pages; death-test forks abort before it runs, leaking one page
// per forked child, which is intentional.
struct TestSpan {
    void* base = nullptr;
    size_t page_num = 0;
    Span span;

    explicit TestSpan(size_t obj_size, size_t pages = 1)
        : base(PageAllocator::SystemAlloc(pages)),
          page_num(pages),
          span(detail::PtrToPageId(base), static_cast<uint32_t>(pages)) {
        span.Init(obj_size);
    }

    ~TestSpan() {
        PageAllocator::SystemFree(base, page_num);
    }

    TestSpan(const TestSpan&) = delete;
    TestSpan& operator=(const TestSpan&) = delete;
};

TEST(SpanTest, ObjectSlotOf_ValidSlot) {
    TestSpan ts(16);

    void* obj0 = ts.span.AllocObject();
    void* obj1 = ts.span.AllocObject();
    ASSERT_NE(obj0, nullptr);
    ASSERT_NE(obj1, nullptr);

    EXPECT_EQ(ts.span.ObjectSlotOf(obj0), size_t{0});
    EXPECT_EQ(ts.span.ObjectSlotOf(obj1), size_t{1});
}

TEST(SpanTest, ObjectSlotOf_OutOfRangeReturnsMax) {
    TestSpan ts(16);
    auto* data_base = static_cast<char*>(ts.span.GetDataBasePtr());
    char* below = data_base - 1;
    char* past_end = data_base + ts.span.capacity * ts.span.aligned_obj_size;

    EXPECT_EQ(ts.span.ObjectSlotOf(below), std::numeric_limits<size_t>::max());
    EXPECT_EQ(ts.span.ObjectSlotOf(past_end), std::numeric_limits<size_t>::max());
}

TEST(SpanTest, ObjectSlotOf_MisalignedReturnsMax) {
    TestSpan ts(16);

    void* misaligned = static_cast<char*>(ts.span.GetDataBasePtr()) + 1;
    EXPECT_EQ(ts.span.ObjectSlotOf(misaligned), std::numeric_limits<size_t>::max());
}

// Uninitialized Spans (capacity == 0) model large-object spans; every address
// must be rejected because no object geometry exists yet.
TEST(SpanTest, ObjectSlotOf_UninitializedSpanReturnsMax) {
    Span raw;

    EXPECT_EQ(raw.ObjectSlotOf(nullptr), std::numeric_limits<size_t>::max());
    EXPECT_EQ(raw.ObjectSlotOf(reinterpret_cast<void*>(0x1000)), std::numeric_limits<size_t>::max());
}

TEST(SpanTest, AllocObject_ExhaustsCapacity) {
    TestSpan ts(16);

    std::vector<void*> objs;
    objs.reserve(ts.span.capacity);
    while (void* obj = ts.span.AllocObject()) {
        objs.push_back(obj);
    }

    EXPECT_EQ(objs.size(), ts.span.capacity);
    EXPECT_EQ(ts.span.use_count, ts.span.capacity);
    EXPECT_EQ(ts.span.AllocObject(), nullptr);
}

// 1 page + 16B slots yields capacity 254 > 64, forcing the global object index
// to span 4 bitmap words; every returned address must stay inside the data
// region with no duplicates or overlap.
TEST(SpanTest, AllocObject_AddressesWithinDataRange) {
    TestSpan ts(16);
    const auto data_base = reinterpret_cast<uintptr_t>(ts.span.GetDataBasePtr());
    const size_t data_bytes = ts.span.capacity * ts.span.aligned_obj_size;

    std::vector<uintptr_t> slots;
    slots.reserve(ts.span.capacity);
    while (void* obj = ts.span.AllocObject()) {
        slots.push_back(reinterpret_cast<uintptr_t>(obj));
    }
    ASSERT_EQ(slots.size(), ts.span.capacity);

    std::ranges::sort(slots);
    EXPECT_TRUE(std::ranges::adjacent_find(slots) == slots.end());
    EXPECT_TRUE(std::ranges::all_of(slots, [&](const uintptr_t addr) {
        return addr >= data_base && addr < data_base + data_bytes;
    }));
    EXPECT_TRUE(std::ranges::all_of(slots, [&](const uintptr_t addr) {
        return (addr - data_base) % ts.span.aligned_obj_size == size_t{0};
    }));
}

TEST(SpanTest, FreeObject_NonPowerOfTwoSize) {
    TestSpan ts(160);// Non-power-of-two class size: FreeObject resolves slots by division.

    std::vector<void*> objs;
    for (int i = 0; i < 10; ++i) {
        void* obj = ts.span.AllocObject();
        ASSERT_NE(obj, nullptr);
        objs.push_back(obj);
    }
    std::ranges::for_each(objs, [&](void* obj) {
        ts.span.FreeObject(obj);
    });
    EXPECT_EQ(ts.span.use_count, 0);

    for (int i = 0; i < 10; ++i) {
        EXPECT_NE(ts.span.AllocObject(), nullptr);
    }
}

TEST(SpanTest, FreeObject_ResetsScanCursor) {
    TestSpan ts(16);

    // Exhaust the Span: the cursor advances past the last bitmap word.
    std::vector<void*> objs;
    objs.reserve(ts.span.capacity);
    while (void* obj = ts.span.AllocObject()) {
        objs.push_back(obj);
    }
    ASSERT_EQ(objs.size(), ts.span.capacity);
    EXPECT_EQ(ts.span.scan_cursor, ts.span.GetBitmapNum());

    // Free a high-slot object (slot 200 lives in the 4th word) and expect the
    // cursor to retreat to that word so the next allocation reuses slot 200.
    const size_t slot = 200;
    void* obj = static_cast<char*>(ts.span.GetDataBasePtr()) + slot * ts.span.aligned_obj_size;
    ts.span.FreeObject(obj);
    EXPECT_EQ(ts.span.scan_cursor, slot >> SystemConfig::BITMAP_SHIFT);

    void* again = ts.span.AllocObject();
    ASSERT_NE(again, nullptr);
    EXPECT_EQ(again, obj);
}

TEST(SpanTest, FreeObjectThenReallocSameSlot) {
    TestSpan ts(16);

    void* obj = ts.span.AllocObject();
    ASSERT_NE(obj, nullptr);
    ts.span.FreeObject(obj);
    EXPECT_EQ(ts.span.use_count, 0);

    void* again = ts.span.AllocObject();
    ASSERT_NE(again, nullptr);
    EXPECT_EQ(again, obj);
    EXPECT_EQ(ts.span.use_count, 1);
}

TEST(SpanTest, Init_ZeroObjectSizeDeath) {
    void* ptr = PageAllocator::SystemAlloc(1);
    ASSERT_NE(ptr, nullptr);
    Span span(detail::PtrToPageId(ptr), 1);

    EXPECT_DEATH(span.Init(0), "Check failed");

    PageAllocator::SystemFree(ptr, 1);
}

// Init enforces its creation-time invariant with AM_CHECK, which is active in
// every build, so these death tests need no NDEBUG/HARDENED guard.
TEST(SpanTest, Init_NonClassSizeDeath) {
    void* ptr = PageAllocator::SystemAlloc(1);
    ASSERT_NE(ptr, nullptr);
    Span span(detail::PtrToPageId(ptr), 1);

    EXPECT_DEATH(span.Init(24), "Check failed");

    PageAllocator::SystemFree(ptr, 1);
}

TEST(SpanTest, Init_OversizeDeath) {
    void* ptr = PageAllocator::SystemAlloc(1);
    ASSERT_NE(ptr, nullptr);
    Span span(detail::PtrToPageId(ptr), 1);

    EXPECT_DEATH(span.Init(SizeConfig::MAX_TC_SIZE + 1), "Check failed");

    PageAllocator::SystemFree(ptr, 1);
}

TEST(SpanTest, Init_MultiPageLayout) {
    TestSpan ts(160, 2);
    auto* page_base = static_cast<char*>(ts.span.GetPageBaseAddr());
    const size_t total_bytes = ts.page_num << SystemConfig::PAGE_SHIFT;
    auto* data_base = static_cast<char*>(ts.span.GetDataBasePtr());

    EXPECT_EQ(ts.span.obj_offset % SystemConfig::ALIGNMENT, 0u);
    EXPECT_GE(data_base, page_base + ts.span.GetBitmapNum() * sizeof(uint64_t));
    EXPECT_LT(data_base, page_base + total_bytes);
    EXPECT_LE(data_base + ts.span.capacity * ts.span.aligned_obj_size, page_base + total_bytes);
}

// An object size larger than the backing pages leaves no room for data after
// the bitmap: capacity collapses to zero and allocation always fails.
TEST(SpanTest, Init_CapacityZero) {
    TestSpan ts(8192);

    EXPECT_EQ(ts.span.capacity, 0u);
    EXPECT_EQ(ts.span.AllocObject(), nullptr);
}

TEST(SpanTest, FlagBits_SetAndClear) {
    Span span;

    EXPECT_FALSE(span.IsUsed());
    EXPECT_FALSE(span.IsCommitted());

    span.SetUsed(true);
    span.SetCommitted(true);
    EXPECT_TRUE(span.IsUsed());
    EXPECT_TRUE(span.IsCommitted());

    span.SetUsed(false);
    span.SetCommitted(false);
    EXPECT_FALSE(span.IsUsed());
    EXPECT_FALSE(span.IsCommitted());
}

// 1 page + 16B slots: capacity 254 = 3 full words plus a 62-bit tail word;
// bits beyond capacity must stay zero.
TEST(SpanTest, BitmapInit_SetsCapacityFreeBits) {
    TestSpan ts(16);

    ASSERT_EQ(ts.span.GetBitmapNum(), 4u);
    auto* bitmap = ts.span.GetBitmap();
    EXPECT_EQ(bitmap[0], ~0ULL);
    EXPECT_EQ(bitmap[1], ~0ULL);
    EXPECT_EQ(bitmap[2], ~0ULL);
    EXPECT_EQ(bitmap[3], (1ULL << 62) - 1);
}

TEST(SpanTest, SpanList_PushFrontPopFrontOrder) {
    SpanList list;
    Span a, b, c;

    list.push_front(&a);
    list.push_front(&b);
    list.push_front(&c);

    EXPECT_EQ(list.pop_front(), &c);
    EXPECT_EQ(list.pop_front(), &b);
    EXPECT_EQ(list.pop_front(), &a);
    EXPECT_TRUE(list.empty());
}

TEST(SpanTest, SpanList_PushBackAndEraseMiddle) {
    SpanList list;
    Span a, b, c;

    list.push_back(&a);
    list.push_back(&b);
    list.push_back(&c);

    Span* after = list.erase(&b);
    EXPECT_EQ(after, &c);
    EXPECT_EQ(&*list.begin(), &a);
    EXPECT_EQ(a.next, &c);
    EXPECT_EQ(c.prev, &a);

    EXPECT_EQ(list.pop_front(), &a);
    EXPECT_EQ(list.pop_front(), &c);
    EXPECT_TRUE(list.empty());
}

TEST(SpanTest, SpanList_EmptyPopFrontReturnsNull) {
    SpanList list;

    EXPECT_TRUE(list.empty());
    EXPECT_EQ(list.pop_front(), nullptr);
}

// Compile-time const-correctness contract: every mutator stays callable on
// mutable lists only, and const accessors never hand out mutable node pointers.
template<typename T, typename = void>
struct CanMutateSpanList : std::false_type {};
template<typename T>
struct CanMutateSpanList<
        T, std::void_t<decltype(std::declval<T&>().push_front(std::declval<Span*>())),
                       decltype(std::declval<T&>().push_back(std::declval<Span*>())),
                       decltype(std::declval<T&>().erase(std::declval<Span*>())),
                       decltype(std::declval<T&>().pop_front())>> : std::true_type {};

static_assert(CanMutateSpanList<SpanList>::value,
              "mutable SpanList must expose every mutator");
static_assert(!CanMutateSpanList<const SpanList>::value,
              "const SpanList must reject every mutator");
static_assert(std::is_same_v<decltype(std::declval<SpanList&>().begin()),
                             SpanList::Iterator>,
              "mutable begin() must return Iterator");
static_assert(std::is_same_v<decltype(std::declval<const SpanList&>().begin()),
                             SpanList::ConstIterator>,
              "const begin() must return ConstIterator");
static_assert(std::is_same_v<decltype(std::declval<SpanList&>().end()),
                             SpanList::Iterator>,
              "mutable end() must return Iterator");
static_assert(std::is_same_v<decltype(std::declval<const SpanList&>().end()),
                             SpanList::ConstIterator>,
              "const end() must return ConstIterator");
static_assert(std::is_same_v<SpanList::Iterator::reference, Span&>,
              "Iterator dereferences to Span&");
static_assert(std::is_same_v<SpanList::ConstIterator::reference, const Span&>,
              "ConstIterator dereferences to const Span&");
static_assert(!std::is_convertible_v<SpanList::ConstIterator, SpanList::Iterator>,
              "ConstIterator must never degrade to mutable Iterator");
static_assert(std::forward_iterator<SpanList::Iterator>,
              "Iterator must satisfy std::forward_iterator");
static_assert(std::forward_iterator<SpanList::ConstIterator>,
              "ConstIterator must satisfy std::forward_iterator");

TEST(SpanTest, SpanList_RangeForOrder) {
    SpanList list;
    Span a, b;
    list.push_front(&a);
    list.push_front(&b);

    std::vector<Span*> seen;
    for (Span& span: list) {
        seen.push_back(&span);
    }

    EXPECT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0], &b);
    EXPECT_EQ(seen[1], &a);
}

TEST(SpanTest, SpanList_ConstRangeForTraversal) {
    SpanList list;
    Span a, b;
    list.push_front(&a);
    list.push_front(&b);

    const SpanList& const_list = list;
    std::vector<const Span*> seen;
    for (const Span& span: const_list) {
        seen.push_back(&span);
    }

    EXPECT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0], &b);
    EXPECT_EQ(seen[1], &a);
    EXPECT_FALSE(const_list.empty());
    EXPECT_EQ(&*const_list.begin(), &b);
}

TEST(SpanTest, SpanList_EraseByIterator) {
    SpanList list;
    Span a, b, c;
    list.push_back(&a);
    list.push_back(&b);
    list.push_back(&c);

    // Advance to the middle node (&b), then erase it through the iterator.
    auto it = list.begin();
    ++it;
    auto next = list.erase(it);

    EXPECT_EQ(&*next, &c);
    EXPECT_EQ(&*list.begin(), &a);
    EXPECT_EQ(a.next, &c);
    EXPECT_EQ(c.prev, &a);
    EXPECT_EQ(b.next, nullptr);
    EXPECT_EQ(b.prev, nullptr);
}

// Standard `it = erase(it)` pattern while iterating: every other node is
// removed without skipping or revisiting any node.
TEST(SpanTest, SpanList_EraseWhileIterating) {
    SpanList list;
    Span a, b, c, d;
    list.push_back(&a);
    list.push_back(&b);
    list.push_back(&c);
    list.push_back(&d);

    auto it = list.begin();
    size_t idx = 0;
    while (it != list.end()) {
        if (idx++ % 2 == 1) {
            it = list.erase(it);
        } else {
            ++it;
        }
    }

    std::vector<Span*> remaining;
    for (Span& span: list) {
        remaining.push_back(&span);
    }
    EXPECT_EQ(remaining.size(), 2u);
    EXPECT_EQ(remaining[0], &a);
    EXPECT_EQ(remaining[1], &c);
}

TEST(SpanTest, SpanList_UsableWithRangesAlgorithms) {
    SpanList list;
    Span a, b, c;
    list.push_back(&a);
    list.push_back(&b);
    list.push_back(&c);

    EXPECT_EQ(std::ranges::count_if(list, [](const Span&) { return true; }), 3u);

    auto found = std::ranges::find_if(list, [&](const Span& s) { return &s == &b; });
    ASSERT_NE(found, list.end());
    EXPECT_EQ(&*found, &b);

    Span missing;
    EXPECT_EQ(std::ranges::find_if(list, [&](const Span& s) { return &s == &missing; }),
              list.end());
}

TEST(SpanTest, FreeObjectUnderflowDeath) {
#if defined(AM_HARDENED) || !defined(NDEBUG)
    TestSpan ts(16);
    char* below = static_cast<char*>(ts.span.GetDataBasePtr()) - 16;
    EXPECT_DEATH(ts.span.FreeObject(below), "Check failed");
#endif
}

TEST(SpanTest, FreeObjectOverflowDeath) {
#if defined(AM_HARDENED) || !defined(NDEBUG)
    TestSpan ts(16);
    char* data_end = static_cast<char*>(ts.span.GetDataBasePtr()) +
                     ts.span.capacity * ts.span.aligned_obj_size;
    EXPECT_DEATH(ts.span.FreeObject(data_end), "Check failed");
#endif
}

TEST(SpanTest, FreeObjectMisalignedDeath) {
#if defined(AM_HARDENED) || !defined(NDEBUG)
    TestSpan ts(16);
    char* misaligned = static_cast<char*>(ts.span.GetDataBasePtr()) + 1;
    EXPECT_DEATH(ts.span.FreeObject(misaligned), "Check failed");
#endif
}

TEST(SpanTest, FreeObjectDoubleFreeDeath) {
#if defined(AM_HARDENED) || !defined(NDEBUG)
    TestSpan ts(16);
    void* obj = ts.span.AllocObject();
    ASSERT_NE(obj, nullptr);
    ts.span.FreeObject(obj);                              // normal free
    EXPECT_DEATH(ts.span.FreeObject(obj), "Check failed");// double free
#endif
}

}// namespace
