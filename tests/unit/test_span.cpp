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

}// namespace
