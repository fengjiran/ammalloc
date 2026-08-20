// Copyright 2026 The AetherMind Authors
// SPDX-License-Identifier: Apache-2.0
//
// Pure-function tests for the ThreadCache quota policy. The policy computes
// the next quota/overage state only; state itself lives in FreeList and is
// exercised by ThreadCacheTest.

#include "ammalloc/thread_cache.h"

#include <gtest/gtest.h>

namespace ammalloc {
namespace {

// 测试点 1: 配额低于一个 batch 时指数增长
TEST(QuotaPolicyTest, RefillGrowsExponentiallyBelowBatch) {
    const size_t batch = 8;
    EXPECT_EQ(quota_policy::NextAfterRefill(0, batch), 1);
    EXPECT_EQ(quota_policy::NextAfterRefill(1, batch), 2);
    EXPECT_EQ(quota_policy::NextAfterRefill(2, batch), 4);
    EXPECT_EQ(quota_policy::NextAfterRefill(4, batch), 8);
    // 5 -> min(8, 5 + 5) = 8
    EXPECT_EQ(quota_policy::NextAfterRefill(5, batch), 8);
    EXPECT_EQ(quota_policy::NextAfterRefill(7, batch), 8);
}

// 测试点 2: 达到 batch 后线性增长，8 倍 batch 处封顶
TEST(QuotaPolicyTest, RefillGrowsLinearlyUpToEightBatches) {
    const size_t batch = 16;// inc = max(1, 16 / 8) = 2
    EXPECT_EQ(quota_policy::NextAfterRefill(16, batch), 18);
    EXPECT_EQ(quota_policy::NextAfterRefill(62, batch), 64);
    EXPECT_EQ(quota_policy::NextAfterRefill(126, batch), 128);// clamp at 8*batch
    EXPECT_EQ(quota_policy::NextAfterRefill(128, batch), 128);// ceiling: unchanged
    EXPECT_EQ(quota_policy::NextAfterRefill(200, batch), 200);// above ceiling
}

// 测试点 3: batch = 1 特例（batch / 8 == 0，inc 取 1）
TEST(QuotaPolicyTest, RefillWithTinyBatch) {
    const size_t batch = 1;
    EXPECT_EQ(quota_policy::NextAfterRefill(0, batch), 1);
    EXPECT_EQ(quota_policy::NextAfterRefill(1, batch), 2);
    EXPECT_EQ(quota_policy::NextAfterRefill(2, batch), 3);
    EXPECT_EQ(quota_policy::NextAfterRefill(7, batch), 8);
    EXPECT_EQ(quota_policy::NextAfterRefill(8, batch), 8);
}

// 测试点 4: 配额在 floor 处时 overflow 不保留衰减状态
TEST(QuotaPolicyTest, OverflowAtFloorResetsOverage) {
    const auto s = quota_policy::NextAfterOverflow(8, 8, 2);
    EXPECT_EQ(s.max_size, 8);
    EXPECT_EQ(s.overages, 0);
}

// 测试点 5: overflow 计数递增，达到 kMaxOverages 时衰减一个 batch
TEST(QuotaPolicyTest, OverflowDecaysAfterMaxOverages) {
    const size_t batch = 8;
    const auto s1 = quota_policy::NextAfterOverflow(32, batch, 0);
    EXPECT_EQ(s1.max_size, 32);
    EXPECT_EQ(s1.overages, 1);

    const auto s2 = quota_policy::NextAfterOverflow(32, batch, s1.overages);
    EXPECT_EQ(s2.max_size, 32);
    EXPECT_EQ(s2.overages, 2);

    const auto s3 = quota_policy::NextAfterOverflow(32, batch, s2.overages);
    EXPECT_EQ(s3.max_size, 24);// 32 - 8
    EXPECT_EQ(s3.overages, 0);
}

// 测试点 6: 衰减下限为 batch
TEST(QuotaPolicyTest, OverflowDecayFloorsAtBatch) {
    const size_t batch = 16;
    const auto s = quota_policy::NextAfterOverflow(24, batch, 2);
    EXPECT_EQ(s.max_size, 16);// max(24 - 16, 16)
    EXPECT_EQ(s.overages, 0);
}

}// namespace
}// namespace ammalloc