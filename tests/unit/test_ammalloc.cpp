#include "ammalloc/ammalloc.h"
#include "ammalloc/config.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

namespace {

using namespace ammalloc;

// Allocate `count` objects of `size` at once, verify every returned address
// is ALIGNMENT-aligned, then release them all. Holding many objects at the
// same time is essential: allocating and immediately freeing would keep
// returning the first slot, which is aligned even when odd slots
// (data_start + odd * size) are not.
void ExpectAlignedAllocations(size_t size, size_t count) {
    std::vector<void*> held;
    held.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        void* p = am_malloc(size);
        ASSERT_NE(p, nullptr) << "am_malloc(" << size << ") failed at slot " << i;
        held.push_back(p);
    }
    for (void* p : held) {
        EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % SystemConfig::ALIGNMENT, 0)
                << "misaligned address for request size " << size;
    }
    for (void* p : held) {
        am_free(p);
    }
}

}  // namespace

TEST(AmMallocAlignmentTest, SmallSizesReturnAlignedAddresses) {
    // Sweep 0..512: the linear region [1, 128] (one class per ALIGNMENT
    // bytes) plus the first geometric groups. 64 simultaneous objects per
    // size exercise many slots of the owning span, including slot 0 which is
    // the span data start, and cross the ThreadCache -> CentralCache batch
    // refill boundary.
    for (size_t req = 0; req <= 512; ++req) {
        ExpectAlignedAllocations(req, 64);
    }
}

TEST(AmMallocAlignmentTest, BoundarySizesReturnAlignedAddresses) {
    // MAX_TC_SIZE stays on the thread-cache path; MAX_TC_SIZE + 1 takes the
    // direct page-allocation path (whole pages, no slot carving), which must
    // honor the same alignment contract.
    ExpectAlignedAllocations(SizeConfig::MAX_TC_SIZE, 64);
    ExpectAlignedAllocations(SizeConfig::MAX_TC_SIZE + 1, 64);
}