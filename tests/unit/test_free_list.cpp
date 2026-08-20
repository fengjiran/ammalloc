#include "ammalloc/free_list.h"

#include <gtest/gtest.h>

namespace {

using namespace ammalloc;

// 测试点 1: push(nullptr) 必须被忽略，链表状态保持不变
TEST(FreeListTest, PushNullIsIgnored) {
    FreeList list;

    list.push(nullptr);

    EXPECT_TRUE(list.empty());
    EXPECT_EQ(list.size(), 0);
}

// 测试点 2: 空表 pop() 返回 nullptr，状态保持不变
TEST(FreeListTest, PopOnEmptyReturnsNull) {
    FreeList list;

    EXPECT_EQ(list.pop(), nullptr);
    EXPECT_TRUE(list.empty());
    EXPECT_EQ(list.size(), 0);
}

// 测试点 3: push_range 收到空指针链时拒绝入链
TEST(FreeListTest, PushRangeRejectsNull) {
    std::array<FreeBlock, 3> blocks{};
    FreeList list;

    list.push_range(FreeChain{nullptr, &blocks[2], 3});
    EXPECT_TRUE(list.empty());
    EXPECT_EQ(list.size(), 0);

    list.push_range(FreeChain{blocks.data(), nullptr, 3});
    EXPECT_TRUE(list.empty());
    EXPECT_EQ(list.size(), 0);
}

// 测试点 4: push_range 收到 count == 0 时拒绝入链
TEST(FreeListTest, PushRangeRejectsZeroCount) {
    std::array<FreeBlock, 3> blocks{};
    FreeList list;

    list.push_range(FreeChain{blocks.data(), &blocks[2], 0});

    EXPECT_TRUE(list.empty());
    EXPECT_EQ(list.size(), 0);
}

// 测试点 5: overages 计数器的读改写往返
TEST(FreeListTest, SetAndGetOverages) {
    FreeList list;

    EXPECT_EQ(list.overages(), 0);

    list.set_overages(3);
    EXPECT_EQ(list.overages(), 3);

    list.set_overages(0);
    EXPECT_EQ(list.overages(), 0);
}

// 测试点 6: pop_range 保持原链顺序并正确终止
TEST(FreeListTest, PopRangeReturnsOrderedChain) {
    std::array<FreeBlock, 3> blocks{};
    blocks[0].next = &blocks[1];
    blocks[1].next = &blocks[2];
    blocks[2].next = nullptr;

    FreeList list;
    list.push_range(FreeChain{blocks.data(), &blocks[2], 3});

    const FreeChain chain = list.pop_range(3);

    EXPECT_EQ(chain.head, blocks.data());
    EXPECT_EQ(chain.tail, &blocks[2]);
    EXPECT_EQ(chain.count, 3);
    EXPECT_TRUE(list.empty());
    EXPECT_EQ(list.size(), 0);
    // The detached chain must be terminated.
    EXPECT_EQ(blocks[2].next, nullptr);
}

// 测试点 7: pop_range 在链不足 n 时弹出全部
TEST(FreeListTest, PopRangePartialOnShortList) {
    std::array<FreeBlock, 2> blocks{};
    blocks[0].next = &blocks[1];
    blocks[1].next = nullptr;

    FreeList list;
    list.push_range(FreeChain{blocks.data(), &blocks[1], 2});

    const FreeChain chain = list.pop_range(5);

    EXPECT_EQ(chain.count, 2);
    EXPECT_EQ(chain.head, blocks.data());
    EXPECT_EQ(chain.tail, &blocks[1]);
    EXPECT_TRUE(list.empty());
}

// 测试点 8: pop_range 空表返回空链
TEST(FreeListTest, PopRangeOnEmptyList) {
    FreeList list;

    const FreeChain chain = list.pop_range(3);

    EXPECT_EQ(chain.count, 0);
    EXPECT_EQ(chain.head, nullptr);
    EXPECT_EQ(chain.tail, nullptr);
    EXPECT_TRUE(list.empty());
}

// 测试点 9: pop_range 保留链上剩余对象
TEST(FreeListTest, PopRangeKeepsRemainder) {
    std::array<FreeBlock, 5> blocks{};
    for (size_t i = 0; i < 4; ++i) {
        blocks[i].next = &blocks[i + 1];
    }
    blocks[4].next = nullptr;

    FreeList list;
    list.push_range(FreeChain{blocks.data(), &blocks[4], 5});

    const FreeChain first = list.pop_range(2);
    EXPECT_EQ(first.count, 2);
    EXPECT_EQ(list.size(), 3);

    const FreeChain rest = list.pop_range(3);
    EXPECT_EQ(rest.count, 3);
    EXPECT_EQ(rest.head, &blocks[2]);
    EXPECT_TRUE(list.empty());
    // The remainder chain must not reach the already-removed prefix.
    EXPECT_EQ(blocks[4].next, nullptr);
}

// 测试点 10: pop_range 输出可直接回灌 push_range（对偶往返）
TEST(FreeListTest, PopRangePushRangeRoundTrip) {
    std::array<FreeBlock, 4> blocks{};
    for (size_t i = 0; i < 3; ++i) {
        blocks[i].next = &blocks[i + 1];
    }
    blocks[3].next = nullptr;

    FreeList list;
    list.push_range(FreeChain{blocks.data(), &blocks[3], 4});

    const FreeChain chain = list.pop_range(4);
    list.push_range(chain);

    EXPECT_EQ(list.size(), 4);
    EXPECT_EQ(list.pop(), blocks.data());
    EXPECT_EQ(list.pop(), &blocks[1]);
    EXPECT_EQ(list.pop(), &blocks[2]);
    EXPECT_EQ(list.pop(), &blocks[3]);
    EXPECT_TRUE(list.empty());
}

// push_range must abort in debug builds when count mismatches the chain.
TEST(FreeListTest, PushRangeCountMismatchDeath) {
#ifndef NDEBUG
    std::array<FreeBlock, 3> blocks{};
    blocks[0].next = &blocks[1];
    blocks[1].next = &blocks[2];
    blocks[2].next = nullptr;

    FreeList list;
    EXPECT_DEATH(list.push_range(FreeChain{blocks.data(), &blocks[2], 5}), "Check failed");
#endif
}

// push_range must abort in debug builds when end is not on the chain.
TEST(FreeListTest, PushRangeEndUnreachableDeath) {
#ifndef NDEBUG
    std::array<FreeBlock, 3> blocks{};
    blocks[0].next = &blocks[1];
    blocks[1].next = &blocks[2];
    blocks[2].next = nullptr;

    FreeList list;
    EXPECT_DEATH(list.push_range(FreeChain{blocks.data(), &blocks[2] + 1, 3}), "Check failed");
#endif
}

}// namespace