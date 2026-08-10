# Test Writing Guidelines

This document defines practical guidance for writing unit tests in ammalloc.

Scope and precedence:

- This document expands test conventions used in this repository.
- If this document conflicts with `AGENTS.md` or verified repository constraints, follow `AGENTS.md` and repository facts.
- Module-specific constraints override generic guidance (for example, `AGENTS.md` §4).

---

## 1. Goals

Tests must prioritize:

- **correctness** — verify the contract, not the implementation trivia
- **readability** — a failing test should explain what broke at a glance
- **maintainability** — tests should not break on harmless refactors
- **isolation** — each test should set up and verify one behavior
- **speed** — unit tests must run fast; slow scenarios belong in benchmarks or integration tests

Prefer clear, focused tests over exhaustive enumeration.

---

## 2. Test Framework and Build

- Framework: GoogleTest (`GTest::gtest_main`).
- Test target: `ammalloc_unit_tests`.
- Test sources are collected automatically via `GLOB_RECURSE` from `tests/unit/**/*.cpp` (see [tests/unit/CMakeLists.txt](file:///home/richard/project/ammalloc/tests/unit/CMakeLists.txt)). New test files are picked up without editing build config.
- Benchmark target: `ammalloc_benchmarks` (Google Benchmark, see `tests/benchmark/`).
- Run commands are documented in `AGENTS.md` §9.

### 2.1 File Placement

Place tests under `tests/unit/`, one `test_<module>.cpp` per module header:

| Source header                        | Test file                             |
| ------------------------------------ | ------------------------------------- |
| `include/ammalloc/span.h`            | `tests/unit/test_span.cpp`            |
| `include/ammalloc/size_class.h`      | `tests/unit/test_size_class.cpp`      |
| `include/ammalloc/central_cache.h`   | `tests/unit/test_central_cache.cpp`   |
| `include/ammalloc/page_cache.h`      | `tests/unit/test_page_cache.cpp`      |
| `include/ammalloc/page_allocator.h`  | `tests/unit/test_page_allocator.cpp`  |
| `include/ammalloc/thread_cache.h`    | `tests/unit/test_thread_cache.cpp`    |

Name test files `test_<unit>.cpp` (for example `test_central_cache.cpp`).

### 2.2 File Size

Keep test files focused. If a file exceeds ~800 lines or mixes many unrelated test suites, split it by logical group (for example, keep `test_size_class.cpp` and `test_central_cache.cpp` separate).

---

## 3. File Header and Includes

### 3.1 Header Format

A new file may omit the `// Created by ...` line. Existing files keep their header.

### 3.2 Include Order

Group includes with blank lines, project headers first or gtest first (both styles exist in the tree). Within each group, keep alphabetical order.

Preferred style for new files:

```cpp
#include "ammalloc/central_cache.h"       // project headers
#include "test_central_cache_helpers.h"

#include <gtest/gtest.h>                    // third-party

#include <optional>                         // standard library
#include <vector>
```

Rationale: project headers come first so the compiler checks self-containment of the unit under test.

### 3.3 Namespace and Using

Wrap test code in an anonymous namespace to avoid ODR clashes across translation units.

```cpp
namespace ammalloc {
namespace {

using namespace ammalloc;   // or only specific names

// ... tests ...

}// namespace
}// namespace ammalloc
```

For files that only need `using namespace ammalloc;` at file scope (older style), keeping that is acceptable for consistency with neighbors.

---

## 4. Test Naming

### 4.1 Suite and Test Names

- Use **PascalCase** for both suite name and test name.
- The suite name should describe the unit under test (a class, module, or concept).
- The test name should describe the behavior being verified, not the implementation step.

Good:

```cpp
TEST(SizeClassTest, RoundUp) { ... }
TEST(SpanTest, DoubleFreeCorruption) { ... }
TEST(ConfigTest, ParseSize) { ... }
```

Bad (avoid):

```cpp
TEST(page_cache, test1) { ... }             // snake_case, non-descriptive
TEST(Span, Find2) { ... }                    // numeric suffix
TEST(SizeClassTest, TestRoundUp) { ... }     // redundant "Test" prefix
```

### 4.2 Death Tests

Suffix the test name with `Death` and assert the failure message emitted by `AMMALLOC_CHECK`.

`AMMALLOC_CHECK` failures go through `detail::HandleCheckFailed`, which prints `Check failed: ...` to stderr and calls `std::abort()`. Match the leading `Check failed` substring.

```cpp
// AMMALLOC_CHECK evaluates the condition and aborts the process on failure.
TEST(AssertTest, CheckFailureDeath) {
    EXPECT_DEATH(AMMALLOC_CHECK(1 == 2, "intentional failure"), "Check failed");
}
```

Guard debug-only death tests with `#ifndef NDEBUG` so release builds do not execute them.

```cpp
TEST(AssertTest, DebugOnlyCheckDeath) {
#ifndef NDEBUG
    EXPECT_DEATH(AMMALLOC_DCHECK(1 == 2), "Check failed");
#endif
}
```

### 4.3 Disabled and Skipped Tests

- Do not leave `DISABLED_` tests in the tree. Convert them to real tests (death tests if they verify abort behavior) or delete them.
- Do not leave commented-out `// GTEST_SKIP();` lines. Either the test runs or it is removed.
- `GTEST_SKIP()` is allowed only when the reason is environmental and unavoidable (missing hardware feature, symlink permission, overflow on a specific platform). Always include a reason string:

```cpp
GTEST_SKIP() << "requires an unsupported platform feature";
```

---

## 5. Test Forms: TEST, TEST_F, TEST_P

### 5.1 TEST — Default

Use `TEST` when no per-test setup is needed. This is the common case.

```cpp
TEST(SizeClassTest, SmallObjectMapping) { ... }
```

### 5.2 TEST_F — Shared Setup/Teardown

Use `TEST_F` when multiple tests share expensive setup or need deterministic reset of a global resource (cache, registry, allocator state).

Name the fixture `<Unit>Test` and keep `SetUp`/`TearDown` minimal.

```cpp
class CentralCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        CentralCache::GetInstance().Reset();
        PageCache::GetInstance().Reset();
    }
    void TearDown() override {
        CentralCache::GetInstance().Reset();
        PageCache::GetInstance().Reset();
    }
    CentralCache& central_cache_ = CentralCache::GetInstance();
    PageCache&   page_cache_     = PageCache::GetInstance();
};

TEST_F(CentralCacheTest, BasicFetchRange) { ... }
```

### 5.3 TEST_P — Parameterized

Use `TEST_P` when the same scenario must be verified across many inputs and the inputs differ only in data, not in logic.

```cpp
class BoundarySizeTest : public ::testing::TestWithParam<size_t> {};

TEST_P(BoundarySizeTest, RoundTrip) {
    const size_t size = GetParam();
    EXPECT_GE(SizeClass::Size(SizeClass::Index(size)), size);
}

INSTANTIATE_TEST_SUITE_P(
        SizeClassBoundaries,
        BoundarySizeTest,
        ::testing::Values(1, 8, 129, SizeConfig::MAX_TC_SIZE));
```

Prefer `TEST_P` over copy-pasted `TEST` blocks when 3+ cases share a body.

---

## 6. Assertions

### 6.1 EXPECT vs ASSERT

- Use `EXPECT_*` by default. A failed `EXPECT` records the failure and continues, which surfaces multiple problems in one run.
- Use `ASSERT_*` only when continuing would dereference a null/invalid value or crash. Typical cases: after null-checks that gate all later assertions.

```cpp
Span* span = PageCache::GetInstance().AllocSpan(1);
ASSERT_NE(span, nullptr);                  // gates line below
EXPECT_EQ(span->page_num, 1);
```

### 6.2 Failure Messages

Add `<<` context to assertions whose raw values are unhelpful on failure (statuses, paths, opaque IDs).

```cpp
ASSERT_NE(span, nullptr) << "page allocation failed";
EXPECT_EQ(span->page_num, 1) << "span page count";
```

### 6.3 Floating Point

- Use `EXPECT_FLOAT_EQ` / `EXPECT_DOUBLE_EQ` for bitwise-tolerant comparisons.
- For reductions where associativity matters, use a relative-epsilon helper and print both values on failure.

```cpp
void ExpectClose(float actual, float expected, float rel_eps = 1.0e-3F) {
    const float max_abs = std::max({std::abs(actual), std::abs(expected),
                                    std::numeric_limits<float>::min()});
    if (const float abs_err = std::abs(actual - expected); abs_err > rel_eps * max_abs) {
        ADD_FAILURE() << "Expected: " << expected << " actual: " << actual
                      << " abs_err: " << abs_err << " rel_err: " << (abs_err / max_abs);
    }
}
```

### 6.4 Compile-Time Checks

Prefer `static_assert` inside a `TEST` body for type traits and `noexcept` contracts. This makes the check participate in the test report while still failing at compile time.

```cpp
TEST(CentralCacheTest, ResetIsNoexcept) {
    static_assert(noexcept(std::declval<CentralCache&>().Reset()));
}
```

---

## 7. Test Helpers

### 7.1 Local Helpers

Place file-local helpers in the anonymous namespace. Keep them `noexcept` when they only shuffle bytes.

```cpp
std::size_t RoundUpToClass(std::size_t size) noexcept {
    return detail::AlignUp(size, 8);
}
```

### 7.2 Shared Helpers

When 3+ test files need the same builder, extract it into a header next to the tests (not into the public `include/` tree). Mark the function `inline` and document why it exists.

```cpp
// tests/unit/test_central_cache_helpers.h
#ifndef AMMALLOC_TEST_CENTRAL_CACHE_HELPERS_H
#define AMMALLOC_TEST_CENTRAL_CACHE_HELPERS_H

#include "ammalloc/central_cache.h"
#include "ammalloc/page_cache.h"

// Resets the global singleton caches before a test.
// Shared by cache test files to avoid duplicating reset logic.
inline void ResetAllocators() {
    CentralCache::GetInstance().Reset();
    PageCache::GetInstance().Reset();
}

#endif
```

Naming:

- Builders that mirror a constructor may use the short name (for example, `NewSpan`).
- Builders that wrap logic should describe the result (for example, `ResetAllocators`, `RoundUpToClass`).

---

## 8. Test Structure and Comments

### 8.1 One Behavior per Test

Each `TEST` should verify one behavior. If a test has 5 unrelated `EXPECT` blocks split by `// 测试xxx` comments, split it into 5 tests. Small tests pinpoint failures.

### 8.2 Arrange-Act-Assert

Prefer the AAA layout. Blank lines separate the phases. A short comment may introduce a non-obvious arrangement.

```cpp
TEST(SizeClassTest, RoundTripContract) {
    // Arrange
    const size_t size = 129;

    // Act
    const size_t idx = SizeClass::Index(size);
    const size_t rounded = SizeClass::Size(idx);

    // Assert
    EXPECT_GE(rounded, size);
    EXPECT_EQ(SizeClass::Index(rounded), idx);
}
```

### 8.3 Comments

- Comment the **intent** of a non-obvious arrangement or the **invariant** being exercised.
- Do not comment obvious assertions (`// check size is 3` above `EXPECT_EQ(v.size(), 3)` adds nothing).
- For death tests, explain what triggers the abort (see §4.2).

---

## 9. Platform and Feature Dependencies

### 9.1 Compile-Time Guards

For code that requires a specific ISA or compiler feature, guard the implementation with `#ifdef` so the file still compiles elsewhere.

```cpp
#if defined(__AVX2__)
#include <immintrin.h>
// ... AVX2 test bodies ...
#endif
```

### 9.2 Runtime Skip

When the feature is detected at runtime, skip with a reason. Only skip when the test genuinely cannot run; do not skip to mask a bug.

```cpp
if (!HasAvx2Support()) {
    GTEST_SKIP() << "CPU lacks AVX2";
}
```

---

## 10. Determinism and Isolation

- Tests must not depend on execution order. Do not rely on global state mutated by an earlier test.
- When a test touches a global singleton (cache, registry), reset it in `SetUp`/`TearDown` (see `CentralCacheTest` above).
- Use fresh fixtures or local variables per test. Avoid sharing mutable state across tests in the same file.
- For random data, seed the RNG explicitly (`std::mt19937 rng(42);`) so failures are reproducible.

---

## 11. What to Test

### 11.1 Public API

Test the public contract of each module: inputs, outputs, documented error conditions, and invariants stated in headers.

### 11.2 Edge Cases

Always cover at minimum:

- empty input (zero-size allocation, empty container)
- boundary sizes (size-class boundary, max representable value)
- invalid input that must be rejected (oversized request, null pointer for non-empty size)
- idempotent operations applied twice

### 11.3 Error Paths

Verify that invalid inputs are rejected or abort via `AMMALLOC_CHECK` (death test). Do not let invalid input silently succeed.

### 11.4 What Not to Test

- Private implementation details that have no observable behavior.
- Standard library facilities (do not test `std::vector`).
- Trivial getters/setters with no logic.

---

## 12. Checklist for New Tests

Before submitting a new test file or suite, verify:

- [ ] File is named `test_<module>.cpp` and placed under `tests/unit/`.
- [ ] Suite and test names are PascalCase and descriptive.
- [ ] No `DISABLED_` prefix, no commented-out `GTEST_SKIP()`.
- [ ] Each test verifies one behavior; AAA layout is clear.
- [ ] `ASSERT_*` used only where continuing would crash.
- [ ] Death tests match `"Check failed"` and are guarded by `#ifndef NDEBUG` when debug-only.
- [ ] Helpers are in the anonymous namespace or a shared `test_*.h`.
- [ ] Global state is reset in `SetUp`/`TearDown`.
- [ ] Random data uses an explicit seed.
- [ ] The narrowest relevant `--gtest_filter` runs green locally.
