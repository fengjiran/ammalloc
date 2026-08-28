//
// Created by richard on 2/19/26.
//
#include "ammalloc/central_cache.h"
#include "ammalloc/page_allocator.h"
#include "ammalloc/page_cache.h"
#include "ammalloc/thread_cache.h"

#include <cstring>
#include <gtest/gtest.h>
#include <random>
#include <thread>
#include <vector>

namespace {
using namespace ammalloc;

class CentralCacheTest : public ::testing::Test {
protected:
    CentralCache& central_cache_ = CentralCache::GetInstance();
    PageCache& page_cache_ = PageCache::GetInstance();

    void SetUp() override {
        central_cache_.Reset();
        page_cache_.Reset();
    }

    void TearDown() override {
        central_cache_.Reset();
        page_cache_.Reset();
    }
};

// 测试点 1: 基本的 FetchRange 操作
TEST_F(CentralCacheTest, BasicFetchRange) {
    FreeList list;
    size_t obj_size = 16;
    size_t batch_num = 10;

    size_t fetched = central_cache_.FetchRange(list, batch_num, obj_size);

    EXPECT_GT(fetched, 0);
    EXPECT_EQ(list.size(), fetched);

    // 清理
    void* head = nullptr;
    while (!list.empty()) {
        void* obj = list.pop();
        auto* block = static_cast<FreeBlock*>(obj);
        block->next = static_cast<FreeBlock*>(head);
        head = obj;
    }
    central_cache_.ReleaseListToSpans(head, obj_size);
}

// 测试点 2: FetchRange 多次调用
TEST_F(CentralCacheTest, MultipleFetchRange) {
    FreeList list;
    size_t obj_size = 32;

    for (int i = 0; i < 5; ++i) {
        size_t fetched = central_cache_.FetchRange(list, 20, obj_size);
        EXPECT_GT(fetched, 0);
    }

    EXPECT_GE(list.size(), 50);

    // 清理
    void* head = nullptr;
    while (!list.empty()) {
        void* obj = list.pop();
        auto* block = static_cast<FreeBlock*>(obj);
        block->next = static_cast<FreeBlock*>(head);
        head = obj;
    }
    central_cache_.ReleaseListToSpans(head, obj_size);
}

// 测试点 3: ReleaseListToSpans 基本操作
TEST_F(CentralCacheTest, BasicReleaseListToSpans) {
    FreeList list;
    size_t obj_size = 64;
    size_t batch_num = 10;

    // 先获取一些对象
    size_t fetched = central_cache_.FetchRange(list, batch_num, obj_size);
    ASSERT_GT(fetched, 0);

    // 构建释放链表
    void* head = nullptr;
    while (!list.empty()) {
        void* obj = list.pop();
        auto* block = static_cast<FreeBlock*>(obj);
        block->next = static_cast<FreeBlock*>(head);
        head = obj;
    }

    // 释放回 CentralCache
    central_cache_.ReleaseListToSpans(head, obj_size);

    // 验证：再次获取应该能成功
    size_t fetched2 = central_cache_.FetchRange(list, batch_num, obj_size);
    EXPECT_GT(fetched2, 0);

    head = nullptr;
    while (!list.empty()) {
        void* obj = list.pop();
        auto* block = static_cast<FreeBlock*>(obj);
        block->next = static_cast<FreeBlock*>(head);
        head = obj;
    }
    central_cache_.ReleaseListToSpans(head, obj_size);
}

// 测试点 4: 不同 Size Class 的分配
TEST_F(CentralCacheTest, DifferentSizeClasses) {
    std::vector<size_t> sizes = {16, 32, 64, 128, 160, 256, 512, 1024, 2048, 4096};

    for (size_t size: sizes) {
        FreeList list;
        size_t fetched = central_cache_.FetchRange(list, 5, size);
        EXPECT_GT(fetched, 0) << "Failed for size " << size;

        void* head = nullptr;
        while (!list.empty()) {
            void* obj = list.pop();
            auto* block = static_cast<FreeBlock*>(obj);
            block->next = static_cast<FreeBlock*>(head);
            head = obj;
        }
        central_cache_.ReleaseListToSpans(head, size);
    }
}

// 测试点 5: 大批量分配
TEST_F(CentralCacheTest, LargeBatchAllocation) {
    FreeList list;
    size_t obj_size = 128;
    size_t batch_num = 100;

    size_t fetched = central_cache_.FetchRange(list, batch_num, obj_size);
    EXPECT_GT(fetched, 0);

    // 验证所有对象都是有效的
    void* head = nullptr;
    size_t count = 0;
    while (!list.empty()) {
        void* obj = list.pop();
        EXPECT_NE(obj, nullptr);
        auto* block = static_cast<FreeBlock*>(obj);
        block->next = static_cast<FreeBlock*>(head);
        head = obj;
        ++count;
    }
    central_cache_.ReleaseListToSpans(head, obj_size);
    EXPECT_EQ(count, fetched);
}

// 测试点 6: Reset 功能
TEST_F(CentralCacheTest, Reset) {
    FreeList list;
    size_t obj_size = 256;

    // 分配一些对象
    central_cache_.FetchRange(list, 10, obj_size);

    // 清空 list
    void* head = nullptr;
    while (!list.empty()) {
        void* obj = list.pop();
        auto* block = static_cast<FreeBlock*>(obj);
        block->next = static_cast<FreeBlock*>(head);
        head = obj;
    }
    central_cache_.ReleaseListToSpans(head, obj_size);

    // Reset CentralCache
    central_cache_.Reset();

    // 再次分配应该仍然正常工作
    size_t fetched = central_cache_.FetchRange(list, 10, obj_size);
    EXPECT_GT(fetched, 0);

    // 清理
    head = nullptr;
    while (!list.empty()) {
        void* obj = list.pop();
        auto* block = static_cast<FreeBlock*>(obj);
        block->next = static_cast<FreeBlock*>(head);
        head = obj;
    }
    central_cache_.ReleaseListToSpans(head, obj_size);
}

// 测试点 7: 对象归还后再次分配
TEST_F(CentralCacheTest, ReallocateAfterRelease) {
    FreeList list;
    size_t obj_size = 512;

    // 第一次分配
    size_t fetched1 = central_cache_.FetchRange(list, 20, obj_size);
    ASSERT_GT(fetched1, 0);

    // 构建释放链表
    void* head = nullptr;
    while (!list.empty()) {
        void* obj = list.pop();
        auto* block = static_cast<FreeBlock*>(obj);
        block->next = static_cast<FreeBlock*>(head);
        head = obj;
    }

    // 释放
    central_cache_.ReleaseListToSpans(head, obj_size);

    // 第二次分配
    size_t fetched2 = central_cache_.FetchRange(list, 20, obj_size);
    ASSERT_GT(fetched2, 0);

    // 清理
    head = nullptr;
    while (!list.empty()) {
        void* obj = list.pop();
        auto* block = static_cast<FreeBlock*>(obj);
        block->next = static_cast<FreeBlock*>(head);
        head = obj;
    }
    central_cache_.ReleaseListToSpans(head, obj_size);
}

// 测试点 8: 压力测试
TEST_F(CentralCacheTest, StressTest) {
    std::vector<std::pair<void*, size_t>> allocated;
    std::mt19937 g(42);
    std::uniform_int_distribution<> size_dis(8, 1024);
    std::uniform_int_distribution<> batch_dis(1, 50);

    // 随机分配
    for (int i = 0; i < 100; ++i) {
        size_t obj_size = size_dis(g);
        obj_size = SizeClass::RoundUp(obj_size);

        FreeList list;
        size_t batch_num = batch_dis(g);
        size_t fetched = central_cache_.FetchRange(list, batch_num, obj_size);

        while (!list.empty()) {
            void* obj = list.pop();
            allocated.emplace_back(obj, obj_size);
        }
    }

    // 随机释放
    std::shuffle(allocated.begin(), allocated.end(), g);

    // 按大小分组释放
    std::map<size_t, void*> release_lists;
    std::map<size_t, void*> release_tails;
    std::map<size_t, size_t> release_counts;

    for (auto& [obj, size]: allocated) {
        auto* block = static_cast<FreeBlock*>(obj);
        if (release_lists.find(size) == release_lists.end()) {
            release_lists[size] = obj;
            release_tails[size] = obj;
            block->next = nullptr;
        } else {
            block->next = static_cast<FreeBlock*>(release_lists[size]);
            release_lists[size] = obj;
        }
        ++release_counts[size];
    }

    for (auto& [size, head]: release_lists) {
        central_cache_.ReleaseListToSpans(head, size);
    }
}

// 测试点 9: 多线程分配
TEST_F(CentralCacheTest, MultiThreadedAllocation) {
    constexpr int num_threads = 4;
    constexpr int allocations_per_thread = 100;

    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&success_count]() {
            for (int i = 0; i < allocations_per_thread; ++i) {
                FreeList list;
                size_t obj_size = 64;
                size_t fetched = CentralCache::GetInstance().FetchRange(list, 10, obj_size);
                if (fetched > 0) {
                    success_count.fetch_add(fetched);
                }
                // 清理
                void* head = nullptr;
                while (!list.empty()) {
                    void* obj = list.pop();
                    auto* block = static_cast<FreeBlock*>(obj);
                    block->next = static_cast<FreeBlock*>(head);
                    head = obj;
                }
                CentralCache::GetInstance().ReleaseListToSpans(head, obj_size);
            }
        });
    }

    for (auto& t: threads) {
        t.join();
    }

    EXPECT_EQ(success_count.load(), num_threads * allocations_per_thread * 10);
}

// 测试点 10: FreeList 基本操作
TEST_F(CentralCacheTest, FreeListOperations) {
    FreeList list;

    EXPECT_TRUE(list.empty());
    EXPECT_EQ(list.size(), 0);

    // 使用实际分配的内存来测试
    size_t obj_size = 64;
    FreeList source;
    central_cache_.FetchRange(source, 5, obj_size);

    // Push
    void* a = source.pop();
    void* b = source.pop();
    void* c = source.pop();

    list.push(a);
    list.push(b);
    list.push(c);

    EXPECT_FALSE(list.empty());
    EXPECT_EQ(list.size(), 3);

    // Pop (LIFO)
    EXPECT_EQ(list.pop(), c);
    EXPECT_EQ(list.pop(), b);
    EXPECT_EQ(list.pop(), a);

    EXPECT_TRUE(list.empty());
    EXPECT_EQ(list.size(), 0);
}

// 测试点 11: FreeList push_range 和 pop_range
TEST_F(CentralCacheTest, FreeListPushRange) {
    FreeList list;
    size_t obj_size = 64;

    // 从 CentralCache 获取对象
    FreeList source;
    central_cache_.FetchRange(source, 5, obj_size);

    void* a = source.pop();
    void* b = source.pop();
    void* c = source.pop();

    // 构建链表: a -> b -> c
    auto* head = static_cast<FreeBlock*>(a);
    auto* node2 = static_cast<FreeBlock*>(b);
    auto* tail = static_cast<FreeBlock*>(c);

    head->next = node2;
    node2->next = tail;
    tail->next = nullptr;

    list.push_range(FreeChain{head, tail, 3});

    EXPECT_EQ(list.size(), 3);
    EXPECT_EQ(list.pop(), head);
    EXPECT_EQ(list.pop(), node2);
    EXPECT_EQ(list.pop(), tail);
    EXPECT_TRUE(list.empty());
}

// 测试点 12: FreeList max_size
TEST_F(CentralCacheTest, FreeListMaxSize) {
    FreeList list;

    EXPECT_EQ(list.max_size(), 1);

    list.set_max_size(100);
    EXPECT_EQ(list.max_size(), 100);

    list.set_max_size(1000);
    EXPECT_EQ(list.max_size(), 1000);
}

// 测试点 13: 小对象分配（8 字节请求映射到最小 16 字节类）
TEST_F(CentralCacheTest, SmallObjectAllocation) {
    FreeList list;
    size_t obj_size = SizeClass::RoundUp(8);// 8 映射到 16 字节类，Span 按类别尺寸雕刻

    size_t fetched = central_cache_.FetchRange(list, 50, obj_size);
    EXPECT_GT(fetched, 0);

    // 清理
    void* head = nullptr;
    while (!list.empty()) {
        void* obj = list.pop();
        auto* block = static_cast<FreeBlock*>(obj);
        block->next = static_cast<FreeBlock*>(head);
        head = obj;
    }
    central_cache_.ReleaseListToSpans(head, obj_size);
}

// 测试点 14: 边界大小分配
TEST_F(CentralCacheTest, BoundarySizeAllocation) {
    // 测试 ThreadCache 最大支持的大小
    size_t max_size = SizeConfig::MAX_TC_SIZE;

    FreeList list;
    size_t fetched = central_cache_.FetchRange(list, 10, max_size);
    EXPECT_GT(fetched, 0);

    // 清理
    void* head = nullptr;
    while (!list.empty()) {
        void* obj = list.pop();
        auto* block = static_cast<FreeBlock*>(obj);
        block->next = static_cast<FreeBlock*>(head);
        head = obj;
    }
    central_cache_.ReleaseListToSpans(head, max_size);
}

// FetchRange returns a partial batch when supply is short: one Span with a
// single free slot left and PageCache unable to refill (OOM) serve 15 of 16
// requested objects. This is the partial-refill fact the ThreadCache quota
// policy is built on (a partial refill must not grow the quota).
TEST_F(CentralCacheTest, FetchRangeReturnsPartialOnShortSupply) {
    // MAX_TC_SIZE class: batch == 2, span capacity 16, and the Span is exactly
    // the 128-page refill unit (no spare Span left in PageCache). Its mapping
    // (< 1 MiB) goes through AllocNormalPage, so the mock blocks new Span
    // acquisition.
    const size_t size = SizeConfig::MAX_TC_SIZE;
    ASSERT_EQ(SizeClass::CalculateBatchSize(size), 2u);

    // Take one object: the only Span has `capacity - 1` free slots left.
    FreeList one;
    ASSERT_EQ(central_cache_.FetchRange(one, 1, size), 1u);
    void* first = one.pop();
    auto* span = PageMap::GetSpan(first);
    ASSERT_NE(span, nullptr);
    const size_t capacity = span->capacity;
    one.push(first);

    // Request `capacity`: only `capacity - 1` are available before the Span is
    // full and the mocked PageCache cannot supply a new one.
    g_mock_normal_alloc_fail.store(true, std::memory_order_relaxed);
    FreeList list;
    const size_t fetched = central_cache_.FetchRange(list, capacity, size);
    g_mock_normal_alloc_fail.store(false, std::memory_order_relaxed);

    EXPECT_EQ(fetched, capacity - 1);
    EXPECT_EQ(list.size(), capacity - 1);

    // 清理
    void* head = nullptr;
    while (!one.empty()) {
        void* obj = one.pop();
        auto* block = static_cast<FreeBlock*>(obj);
        block->next = static_cast<FreeBlock*>(head);
        head = obj;
    }
    while (!list.empty()) {
        void* obj = list.pop();
        auto* block = static_cast<FreeBlock*>(obj);
        block->next = static_cast<FreeBlock*>(head);
        head = obj;
    }
    central_cache_.ReleaseListToSpans(head, size);
}

}// namespace
