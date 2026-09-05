#include "ammalloc/free_list.h"

#include <gtest/gtest.h>

namespace {

using namespace ammalloc;

// Test point 1: Push(nullptr) must be ignored, list state unchanged
TEST(FreeListTest, PushNullIsIgnored) {
    FreeList list;

    list.Push(nullptr);

    EXPECT_TRUE(list.empty());
    EXPECT_EQ(list.size(), 0);
}

// Test point 2: Pop() on empty list returns nullptr, state unchanged
TEST(FreeListTest, PopOnEmptyReturnsNull) {
    FreeList list;

    EXPECT_EQ(list.Pop(), nullptr);
    EXPECT_TRUE(list.empty());
    EXPECT_EQ(list.size(), 0);
}

// Test point 3: PushRange rejects null-pointer chains
TEST(FreeListTest, PushRangeRejectsNull) {
    std::array<FreeBlock, 3> blocks{};
    FreeList list;

    list.PushRange(FreeChain{nullptr, &blocks[2], 3});
    EXPECT_TRUE(list.empty());
    EXPECT_EQ(list.size(), 0);

    list.PushRange(FreeChain{blocks.data(), nullptr, 3});
    EXPECT_TRUE(list.empty());
    EXPECT_EQ(list.size(), 0);
}

// Test point 4: PushRange rejects count == 0
TEST(FreeListTest, PushRangeRejectsZeroCount) {
    std::array<FreeBlock, 3> blocks{};
    FreeList list;

    list.PushRange(FreeChain{blocks.data(), &blocks[2], 0});

    EXPECT_TRUE(list.empty());
    EXPECT_EQ(list.size(), 0);
}

// Test point 5: overages counter read-modify-write round trip
TEST(FreeListTest, SetAndGetOverages) {
    FreeList list;

    EXPECT_EQ(list.overages(), 0);

    list.set_overages(3);
    EXPECT_EQ(list.overages(), 3);

    list.set_overages(0);
    EXPECT_EQ(list.overages(), 0);
}

// Test point 6: PopRange preserves original chain order and terminates correctly
TEST(FreeListTest, PopRangeReturnsOrderedChain) {
    std::array<FreeBlock, 3> blocks{};
    blocks[0].next = &blocks[1];
    blocks[1].next = &blocks[2];
    blocks[2].next = nullptr;

    FreeList list;
    list.PushRange(FreeChain{blocks.data(), &blocks[2], 3});

    const FreeChain chain = list.PopRange(3);

    EXPECT_EQ(chain.head, blocks.data());
    EXPECT_EQ(chain.tail, &blocks[2]);
    EXPECT_EQ(chain.count, 3);
    EXPECT_TRUE(list.empty());
    EXPECT_EQ(list.size(), 0);
    // The detached chain must be terminated.
    EXPECT_EQ(blocks[2].next, nullptr);
}

// Test point 7: PopRange pops everything when chain shorter than n
TEST(FreeListTest, PopRangePartialOnShortList) {
    std::array<FreeBlock, 2> blocks{};
    blocks[0].next = &blocks[1];
    blocks[1].next = nullptr;

    FreeList list;
    list.PushRange(FreeChain{blocks.data(), &blocks[1], 2});

    const FreeChain chain = list.PopRange(5);

    EXPECT_EQ(chain.count, 2);
    EXPECT_EQ(chain.head, blocks.data());
    EXPECT_EQ(chain.tail, &blocks[1]);
    EXPECT_TRUE(list.empty());
}

// Test point 8: PopRange on empty list returns empty chain
TEST(FreeListTest, PopRangeOnEmptyList) {
    FreeList list;

    const FreeChain chain = list.PopRange(3);

    EXPECT_EQ(chain.count, 0);
    EXPECT_EQ(chain.head, nullptr);
    EXPECT_EQ(chain.tail, nullptr);
    EXPECT_TRUE(list.empty());
}

// Test point 9: PopRange keeps remaining objects on the chain
TEST(FreeListTest, PopRangeKeepsRemainder) {
    std::array<FreeBlock, 5> blocks{};
    for (size_t i = 0; i < 4; ++i) {
        blocks[i].next = &blocks[i + 1];
    }
    blocks[4].next = nullptr;

    FreeList list;
    list.PushRange(FreeChain{blocks.data(), &blocks[4], 5});

    const FreeChain first = list.PopRange(2);
    EXPECT_EQ(first.count, 2);
    EXPECT_EQ(list.size(), 3);

    const FreeChain rest = list.PopRange(3);
    EXPECT_EQ(rest.count, 3);
    EXPECT_EQ(rest.head, &blocks[2]);
    EXPECT_TRUE(list.empty());
    // The remainder chain must not reach the already-removed prefix.
    EXPECT_EQ(blocks[4].next, nullptr);
}

// Test point 10: PopRange output can be fed back into PushRange (round trip)
TEST(FreeListTest, PopRangePushRangeRoundTrip) {
    std::array<FreeBlock, 4> blocks{};
    for (size_t i = 0; i < 3; ++i) {
        blocks[i].next = &blocks[i + 1];
    }
    blocks[3].next = nullptr;

    FreeList list;
    list.PushRange(FreeChain{blocks.data(), &blocks[3], 4});

    const FreeChain chain = list.PopRange(4);
    list.PushRange(chain);

    EXPECT_EQ(list.size(), 4);
    EXPECT_EQ(list.Pop(), blocks.data());
    EXPECT_EQ(list.Pop(), &blocks[1]);
    EXPECT_EQ(list.Pop(), &blocks[2]);
    EXPECT_EQ(list.Pop(), &blocks[3]);
    EXPECT_TRUE(list.empty());
}

// PushRange must abort in debug builds when count mismatches the chain.
TEST(FreeListTest, PushRangeCountMismatchDeath) {
#ifndef NDEBUG
    std::array<FreeBlock, 3> blocks{};
    blocks[0].next = &blocks[1];
    blocks[1].next = &blocks[2];
    blocks[2].next = nullptr;

    FreeList list;
    EXPECT_DEATH(list.PushRange(FreeChain{blocks.data(), &blocks[2], 5}), "Check failed");
#endif
}

// PushRange must abort in debug builds when end is not on the chain.
TEST(FreeListTest, PushRangeEndUnreachableDeath) {
#ifndef NDEBUG
    std::array<FreeBlock, 3> blocks{};
    blocks[0].next = &blocks[1];
    blocks[1].next = &blocks[2];
    blocks[2].next = nullptr;

    FreeList list;
    EXPECT_DEATH(list.PushRange(FreeChain{blocks.data(), &blocks[2] + 1, 3}), "Check failed");
#endif
}

// PopRangeTail evicts the least recently pushed objects, keeping the newest
// ones local (overflow-trim locality contract).
TEST(FreeListTest, PopRangeTailKeepsNewest) {
    std::array<FreeBlock, 5> blocks{};
    for (size_t i = 0; i < 4; ++i) {
        blocks[i].next = &blocks[i + 1];
    }
    blocks[4].next = nullptr;

    FreeList list;
    // LIFO: blocks[0] is the newest, blocks[4] the oldest.
    list.PushRange(FreeChain{blocks.data(), &blocks[4], 5});

    const FreeChain chain = list.PopRangeTail(2);

    // The two oldest objects are evicted, the three newest stay.
    EXPECT_EQ(chain.count, 2);
    EXPECT_EQ(chain.head, &blocks[3]);
    EXPECT_EQ(chain.tail, &blocks[4]);
    EXPECT_EQ(list.size(), 3);
    // Both chains are terminated at the cut point.
    EXPECT_EQ(blocks[2].next, nullptr);
    EXPECT_EQ(blocks[4].next, nullptr);

    // The remaining chain keeps LIFO order.
    EXPECT_EQ(list.Pop(), blocks.data());
    EXPECT_EQ(list.Pop(), &blocks[1]);
    EXPECT_EQ(list.Pop(), &blocks[2]);
    EXPECT_TRUE(list.empty());
}

// PopRangeTail evicts everything when n covers the whole list.
TEST(FreeListTest, PopRangeTailAllWhenLongEnough) {
    std::array<FreeBlock, 3> blocks{};
    blocks[0].next = &blocks[1];
    blocks[1].next = &blocks[2];
    blocks[2].next = nullptr;

    FreeList list;
    list.PushRange(FreeChain{blocks.data(), &blocks[2], 3});

    const FreeChain chain = list.PopRangeTail(5);

    EXPECT_EQ(chain.count, 3);
    EXPECT_EQ(chain.head, blocks.data());
    EXPECT_EQ(chain.tail, &blocks[2]);
    EXPECT_TRUE(list.empty());
    EXPECT_EQ(list.size(), 0);
}

// PopRangeTail on an empty list returns an empty chain.
TEST(FreeListTest, PopRangeTailOnEmptyList) {
    FreeList list;

    const FreeChain chain = list.PopRangeTail(3);

    EXPECT_EQ(chain.count, 0);
    EXPECT_EQ(chain.head, nullptr);
    EXPECT_EQ(chain.tail, nullptr);
    EXPECT_TRUE(list.empty());
}

// PopRangeTail with n == 0 is a no-op.
TEST(FreeListTest, PopRangeTailZeroCount) {
    std::array<FreeBlock, 2> blocks{};
    blocks[0].next = &blocks[1];
    blocks[1].next = nullptr;

    FreeList list;
    list.PushRange(FreeChain{blocks.data(), &blocks[1], 2});

    const FreeChain chain = list.PopRangeTail(0);

    EXPECT_EQ(chain.count, 0);
    EXPECT_EQ(list.size(), 2);
    EXPECT_EQ(list.Pop(), blocks.data());
}

// PopRangeTail output round-trips through PushRange.
TEST(FreeListTest, PopRangeTailPushRangeRoundTrip) {
    std::array<FreeBlock, 4> blocks{};
    for (size_t i = 0; i < 3; ++i) {
        blocks[i].next = &blocks[i + 1];
    }
    blocks[3].next = nullptr;

    FreeList list;
    list.PushRange(FreeChain{blocks.data(), &blocks[3], 4});

    const FreeChain chain = list.PopRangeTail(1);
    list.PushRange(chain);

    EXPECT_EQ(list.size(), 4);
    // The evicted oldest object (blocks[3]) is re-prepended, becoming newest.
    EXPECT_EQ(list.Pop(), &blocks[3]);
    EXPECT_EQ(list.Pop(), blocks.data());
    EXPECT_EQ(list.Pop(), &blocks[1]);
    EXPECT_EQ(list.Pop(), &blocks[2]);
    EXPECT_TRUE(list.empty());
}

}// namespace