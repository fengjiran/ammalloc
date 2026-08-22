// Pure-function tests for the ThreadCache quota policy. The policy computes
// the next quota/overage state only; state itself lives in FreeList and is
// exercised by ThreadCacheTest.

#include "ammalloc/thread_cache.h"

#include <gtest/gtest.h>

namespace ammalloc {
namespace {

// Test 1: exponential growth while the quota is below one batch.
TEST(QuotaPolicyTest, RefillGrowsExponentiallyBelowBatch) {
    constexpr size_t batch = 8;
    EXPECT_EQ(quota_policy::NextAfterRefill(0, batch), 1);
    EXPECT_EQ(quota_policy::NextAfterRefill(1, batch), 2);
    EXPECT_EQ(quota_policy::NextAfterRefill(2, batch), 4);
    EXPECT_EQ(quota_policy::NextAfterRefill(4, batch), 8);
    // 5 -> min(8, 5 + 5) = 8
    EXPECT_EQ(quota_policy::NextAfterRefill(5, batch), 8);
    EXPECT_EQ(quota_policy::NextAfterRefill(7, batch), 8);
}

// Test 2: linear growth once past one batch, clamped at kMaxQuotaBatches * batch.
TEST(QuotaPolicyTest, RefillGrowsLinearlyUpToEightBatches) {
    constexpr size_t batch = 16;// inc = max(1, 16 / kMaxQuotaBatches) = 2
    EXPECT_EQ(quota_policy::NextAfterRefill(16, batch), 18);
    EXPECT_EQ(quota_policy::NextAfterRefill(62, batch), 64);
    // clamp at kMaxQuotaBatches * batch
    EXPECT_EQ(quota_policy::NextAfterRefill(126, batch), quota_policy::kMaxQuotaBatches * batch);
    // ceiling: unchanged
    EXPECT_EQ(quota_policy::NextAfterRefill(quota_policy::kMaxQuotaBatches * batch, batch),
              quota_policy::kMaxQuotaBatches * batch);
    // above ceiling
    EXPECT_EQ(quota_policy::NextAfterRefill(200, batch), 200);
}

// Test 3: batch = 1 special case (batch / kMaxQuotaBatches == 0, inc clamps to 1).
TEST(QuotaPolicyTest, RefillWithTinyBatch) {
    constexpr size_t batch = 1;
    EXPECT_EQ(quota_policy::NextAfterRefill(0, batch), 1);
    EXPECT_EQ(quota_policy::NextAfterRefill(1, batch), 2);
    EXPECT_EQ(quota_policy::NextAfterRefill(2, batch), 3);
    // clamp at kMaxQuotaBatches * batch = 8
    EXPECT_EQ(quota_policy::NextAfterRefill(7, batch), quota_policy::kMaxQuotaBatches);
    // ceiling: unchanged
    EXPECT_EQ(quota_policy::NextAfterRefill(quota_policy::kMaxQuotaBatches, batch),
              quota_policy::kMaxQuotaBatches);
}

// Test 4: overflow at the floor keeps no decay state.
TEST(QuotaPolicyTest, OverflowAtFloorResetsOverage) {
    const auto s = quota_policy::NextAfterOverflow(8, 8, 2);
    EXPECT_EQ(s.max_size, 8);
    EXPECT_EQ(s.overages, 0);
}

// Test 5: the overage counter advances; decay kicks in at kMaxOverages.
TEST(QuotaPolicyTest, OverflowDecaysAfterMaxOverages) {
    constexpr size_t batch = 8;
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

// Test 6: decay floors at one batch.
TEST(QuotaPolicyTest, OverflowDecayFloorsAtBatch) {
    constexpr size_t batch = 16;
    const auto s = quota_policy::NextAfterOverflow(24, batch, 2);
    EXPECT_EQ(s.max_size, 16);// max(24 - 16, 16)
    EXPECT_EQ(s.overages, 0);
}

// Test 7: current below the floor is kept unchanged and overages reset.
TEST(QuotaPolicyTest, OverflowBelowFloorKeepsCurrent) {
    const auto s1 = quota_policy::NextAfterOverflow(4, 8, 3);
    EXPECT_EQ(s1.max_size, 4);
    EXPECT_EQ(s1.overages, 0);

    const auto s2 = quota_policy::NextAfterOverflow(1, 16, 2);
    EXPECT_EQ(s2.max_size, 1);
    EXPECT_EQ(s2.overages, 0);
}

}// namespace
}// namespace ammalloc