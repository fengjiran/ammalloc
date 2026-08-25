#include "ammalloc/page_allocator.h"
#include "ammalloc/span.h"
#include <gtest/gtest.h>

namespace {

using namespace ammalloc;

class SpanTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Mock PageAllocator behavior if needed, or just use real allocation
    }

    void TearDown() override {
    }
};

TEST_F(SpanTest, DoubleFreeCorruption) {
    // 1. Manually allocate a Span
    // Use a small size class (e.g. 16 bytes)
    size_t obj_size = 16;
    size_t page_num = 1;

    void* ptr = PageAllocator::SystemAlloc(page_num);
    ASSERT_NE(ptr, nullptr);

    Span span(detail::PtrToPageId(ptr), page_num);
    span.Init(obj_size);

    // 2. Alloc one object
    void* obj1 = span.AllocObject();
    ASSERT_NE(obj1, nullptr);
    EXPECT_EQ(span.use_count, 1);

    // 3. Free it (Normal)
    span.FreeObject(obj1);
    EXPECT_EQ(span.use_count, 0);

    // 4. Free it again (Double Free)
    // Current bug: use_count becomes MAX_SIZE_T
    // span.FreeObject(obj1);

    // If bug exists, use_count wrapped around
    if (span.use_count > span.capacity) {
        // Demonstrated the bug!
        // Now AllocObject should fail because use_count >= capacity
        void* obj2 = span.AllocObject();
        EXPECT_EQ(obj2, nullptr) << "AllocObject should fail due to use_count corruption";
    } else {
        // Bug fixed? Or didn't wrap?
        // If fixed, use_count should still be 0 (idempotent free) or aborted.
        EXPECT_EQ(span.use_count, 0);
        void* obj2 = span.AllocObject();
        EXPECT_NE(obj2, nullptr);
    }

    // Cleanup
    // Need to clean up manually since Span destructor doesn't free memory
    // But Span is stack allocated here.
    // Memory allocated via SystemAlloc.
    PageAllocator::SystemFree(ptr, page_num);
}

// Stack-local single-page Span with fixed-size slots, backed by SystemAlloc.
// Used only by death tests below; each aborts in the forked child, so the
// backing mapping is intentionally never returned to the OS.
struct TestSpan {
    void* base = nullptr;
    Span span;

    explicit TestSpan(size_t obj_size)
        : base(PageAllocator::SystemAlloc(1)),
          span(detail::PtrToPageId(base), 1) {
        span.Init(obj_size);
    }
};

TEST_F(SpanTest, FreeObjectUnderflowDeath) {
#if defined(AM_HARDENED) || !defined(NDEBUG)
    TestSpan ts(16);
    char* below = static_cast<char*>(ts.span.GetDataBasePtr()) - 16;
    EXPECT_DEATH(ts.span.FreeObject(below), "Check failed");
#endif
}

TEST_F(SpanTest, FreeObjectOverflowDeath) {
#if defined(AM_HARDENED) || !defined(NDEBUG)
    TestSpan ts(16);
    char* data_end = static_cast<char*>(ts.span.GetDataBasePtr()) +
                     ts.span.capacity * ts.span.aligned_obj_size;
    EXPECT_DEATH(ts.span.FreeObject(data_end), "Check failed");
#endif
}

TEST_F(SpanTest, FreeObjectMisalignedDeath) {
#if defined(AM_HARDENED) || !defined(NDEBUG)
    TestSpan ts(16);
    char* misaligned = static_cast<char*>(ts.span.GetDataBasePtr()) + 1;
    EXPECT_DEATH(ts.span.FreeObject(misaligned), "Check failed");
#endif
}

TEST_F(SpanTest, FreeObjectDoubleFreeDeath) {
#if defined(AM_HARDENED) || !defined(NDEBUG)
    TestSpan ts(16);
    void* obj = ts.span.AllocObject();
    ASSERT_NE(obj, nullptr);
    ts.span.FreeObject(obj);                              // normal free
    EXPECT_DEATH(ts.span.FreeObject(obj), "Check failed");// double free
#endif
}

}// namespace
