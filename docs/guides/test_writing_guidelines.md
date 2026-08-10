# Test Writing Guidelines

This document defines practical guidance for writing unit tests in AetherMind.

Scope and precedence:

- This document expands test conventions used in this repository.
- If this document conflicts with `AGENTS.md` or verified repository constraints, follow `AGENTS.md` and repository facts.
- Subsystem-specific constraints override generic guidance (for example, `ammalloc/AGENTS.md`).

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
- Test target: `aethermind_unit_tests`.
- Test sources are collected automatically via `GLOB_RECURSE` from `tests/unit/**/*.cpp` (see [tests/unit/CMakeLists.txt](file:///home/richard/project/AetherMind/tests/unit/CMakeLists.txt)). New test files are picked up without editing build config.
- Benchmark target: `aethermind_benchmark` (Google Benchmark, see `tests/benchmark/`).
- Run commands are documented in `AGENTS.md` §4.

### 2.1 File Placement

Place tests under `tests/unit/` mirroring the source layout:

| Source path                         | Test path                              |
| ----------------------------------- | -------------------------------------- |
| `include/aethermind/graph/`         | `tests/unit/graph/`                    |
| `src/graph/`                        | `tests/unit/graph/`                    |
| `include/aethermind/model/`         | `tests/unit/model/`                    |
| `src/model/`                        | `tests/unit/model/`                    |
| `src/operators/`                    | `tests/unit/operators/`                |
| `include/aethermind/execution/`     | `tests/unit/execution/`                |
| `src/execution/`                    | `tests/unit/execution/`                |
| `src/backend/cpu/kernels/`          | `tests/unit/backend/cpu/kernels/`      |
| `ammalloc/src/`                     | `tests/unit/memory/`                   |
| `src/container/`                    | `tests/unit/tensor/`                   |

Name test files `test_<unit>.cpp` (for example `test_rmsnorm_op.cpp`).

### 2.2 File Size

Keep test files focused. If a file exceeds ~800 lines or mixes many unrelated test suites, split it by logical group (see [test_string_*.cpp](file:///home/richard/project/AetherMind/tests/unit/tensor/test_string_accessors.cpp) as an example of splitting by responsibility).

---

## 3. File Header and Includes

### 3.1 Header Format

A new file may omit the `// Created by ...` line. Existing files keep their header.

### 3.2 Include Order

Group includes with blank lines, project headers first or gtest first (both styles exist in the tree). Within each group, keep alphabetical order.

Preferred style for new files:

```cpp
#include "aethermind/graph/graph.h"          // project headers
#include "test_graph_helpers.h"

#include <gtest/gtest.h>                    // third-party

#include <optional>                         // standard library
#include <vector>
```

Rationale: project headers come first so the compiler checks self-containment of the unit under test.

### 3.3 Namespace and Using

Wrap test code in an anonymous namespace to avoid ODR clashes across translation units.

```cpp
namespace aethermind {
namespace {

using namespace aethermind;   // or only specific names

// ... tests ...

}// namespace
}// namespace aethermind
```

For files that only need `using namespace aethermind;` at file scope (older style), keeping that is acceptable for consistency with neighbors.

---

## 4. Test Naming

### 4.1 Suite and Test Names

- Use **PascalCase** for both suite name and test name.
- The suite name should describe the unit under test (a class, module, or concept).
- The test name should describe the behavior being verified, not the implementation step.

Good:

```cpp
TEST(RmsNormOp, ValidatesStaticInputContract) { ... }
TEST(StringFrontBack, BasicFunctionality) { ... }
TEST(TypeSystem, UnionType) { ... }
```

Bad (avoid):

```cpp
TEST(rms_norm, test1) { ... }                 // snake_case, non-descriptive
TEST(StringFind, Find2) { ... }               // numeric suffix
TEST(TypeSystem, TestUnionType) { ... }       // redundant "Test" prefix
```

### 4.2 Death Tests

Suffix the test name with `Death` and assert the failure message emitted by `AM_CHECK`.

`AM_CHECK` failures go through `HandleCheckFailed`, which prints `Check failed: ...` to stderr and calls `std::abort()`. Match the leading `Check failed` substring.

```cpp
// front()/back() on an empty string triggers AM_CHECK(!empty()) which aborts.
TEST(StringFrontBack, EmptyStringDeath) {
    String empty;
    EXPECT_DEATH(static_cast<void>(empty.front()), "Check failed");
    EXPECT_DEATH(static_cast<void>(empty.back()), "Check failed");
}
```

Guard debug-only death tests with `#ifndef NDEBUG` so release builds do not execute them.

```cpp
TEST(CharLayoutPolicyTest, CheckInvariantsRejectsInvalidCategory) {
    Policy::Storage storage;
    Policy::InitEmpty(storage);
    WriteProbeByte(storage, config::kIsLittleEndian ? 0x40U : 0x01U);

#ifndef NDEBUG
    EXPECT_DEATH(Policy::CheckInvariants(storage), "Check failed");
#endif
}
```

### 4.3 Disabled and Skipped Tests

- Do not leave `DISABLED_` tests in the tree. Convert them to real tests (death tests if they verify abort behavior) or delete them.
- Do not leave commented-out `// GTEST_SKIP();` lines. Either the test runs or it is removed.
- `GTEST_SKIP()` is allowed only when the reason is environmental and unavoidable (missing hardware feature, symlink permission, overflow on a specific platform). Always include a reason string:

```cpp
GTEST_SKIP() << "numel calculation overflowed";
```

---

## 5. Test Forms: TEST, TEST_F, TEST_P

### 5.1 TEST — Default

Use `TEST` when no per-test setup is needed. This is the common case.

```cpp
TEST(TensorTypeTest, BasicProperties) { ... }
```

### 5.2 TEST_F — Shared Setup/Teardown

Use `TEST_F` when multiple tests share expensive setup or need deterministic reset of a global resource (cache, registry, allocator state).

Name the fixture `<Unit>Test` and keep `SetUp`/`TearDown` minimal.

```cpp
class ThreadCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        central_cache_.Reset();
        page_cache_.Reset();
    }
    void TearDown() override {
        central_cache_.Reset();
        page_cache_.Reset();
    }
    CentralCache& central_cache_ = CentralCache::GetInstance();
    PageCache&   page_cache_     = PageCache::GetInstance();
};

TEST_F(ThreadCacheTest, BasicAllocate) { ... }
```

### 5.3 TEST_P — Parameterized

Use `TEST_P` when the same scenario must be verified across many inputs and the inputs differ only in data, not in logic.

```cpp
class HfConfigMissingRequiredFieldTest : public ::testing::TestWithParam<const char*> {};

TEST_P(HfConfigMissingRequiredFieldTest, DefersMissingRequiredFieldToValidator) {
    const auto* const missing_field = GetParam();
    // ... write config without `missing_field`, expect validator to reject ...
}

INSTANTIATE_TEST_SUITE_P(
        RequiredFields,
        HfConfigMissingRequiredFieldTest,
        ::testing::Values("model_type", "hidden_size", /* ... */),
        [](const ::testing::TestParamInfo<...>& info) {
            return std::string(info.param);   // deterministic test name
        });
```

Prefer `TEST_P` over copy-pasted `TEST` blocks when 3+ cases share a body.

---

## 6. Assertions

### 6.1 EXPECT vs ASSERT

- Use `EXPECT_*` by default. A failed `EXPECT` records the failure and continues, which surfaces multiple problems in one run.
- Use `ASSERT_*` only when continuing would dereference a null/invalid value or crash. Typical cases: after `Status`/`Expected` checks that gate all later assertions.

```cpp
auto reader = OpenTempDir(temp_dir);
ASSERT_TRUE(reader.ok()) << reader.status().ToString();   // gates line below
const auto config = reader->ParseConfig();
ASSERT_TRUE(config.ok()) << config.status().ToString();

EXPECT_EQ(config->model_type, "llama");                   // non-fatal
```

### 6.2 Failure Messages

Add `<<` context to assertions whose raw values are unhelpful on failure (statuses, paths, opaque IDs).

```cpp
ASSERT_TRUE(layout.ok()) << layout.status().ToString();
EXPECT_EQ(buffer[kSize], 'x') << "kSize=" << kSize;
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
TEST(BasicStringCoreSwap, IsNoexceptWhenPolicySwapIsNoexcept) {
    static_assert(noexcept(std::declval<DefaultCore&>().swap(std::declval<DefaultCore&>())));
    static_assert(!noexcept(std::declval<ThrowingSwapCore&>().swap(std::declval<ThrowingSwapCore&>())));
}
```

---

## 7. Test Helpers

### 7.1 Local Helpers

Place file-local helpers in the anonymous namespace. Keep them `noexcept` when they only shuffle bytes.

```cpp
std::uint8_t EncodedSmallProbe(Policy::SizeType size) noexcept {
    const auto meta = Policy::EncodeSmallSizeToProbe(size);
    if constexpr (config::kIsLittleEndian) {
        return static_cast<std::uint8_t>(meta);
    } else {
        return static_cast<std::uint8_t>(meta << Policy::kCategoryBits);
    }
}
```

### 7.2 Shared Helpers

When 3+ test files need the same builder, extract it into a header next to the tests (not into the public `include/` tree). Mark the function `inline` and document why it exists.

```cpp
// tests/unit/graph/test_graph_helpers.h
#ifndef AETHERMIND_GRAPH_TEST_GRAPH_HELPERS_H
#define AETHERMIND_GRAPH_TEST_GRAPH_HELPERS_H

#include "aethermind/shape_inference/tensor_spec.h"

#include <cstdint>
#include <vector>

namespace aethermind {

// Builds a fully-static TensorSpec from a dtype and concrete dims.
// Shared by graph test files to avoid duplicating the helper.
inline TensorSpec Spec(DataType dtype, std::vector<int64_t> shape) {
    return TensorSpec{.dtype = dtype, .shape = SymbolicShape(IntArrayView(shape))};
}

}// namespace aethermind

#endif
```

Naming:

- Builders that mirror a constructor may use the short name (e.g. `Spec`).
- Builders that wrap logic should describe the result (e.g. `StaticShape`, `ActivationSpec`, `TokenSpec`).

---

## 8. Test Structure and Comments

### 8.1 One Behavior per Test

Each `TEST` should verify one behavior. If a test has 5 unrelated `EXPECT` blocks split by `// 测试xxx` comments, split it into 5 tests. Small tests pinpoint failures.

### 8.2 Arrange-Act-Assert

Prefer the AAA layout. Blank lines separate the phases. A short comment may introduce a non-obvious arrangement.

```cpp
TEST(RmsNormOp, PreservesInputShapeAsOutputShape) {
    // Arrange
    const RmsNormOp op{RmsNormOp::Params{}};
    const TensorSpec inputs[2] = {
            TensorSpec{.dtype = DataType::Float32(), .shape = StaticShape({4, 8})},
            TensorSpec{.dtype = DataType::Float32(), .shape = StaticShape({8})},
    };

    // Act
    const auto outputs = op.InferOutputSpecs(inputs);

    // Assert
    ASSERT_TRUE(outputs.ok()) << outputs.status().ToString();
    EXPECT_EQ(outputs->at(0).shape, inputs[0].shape);
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

### 9.3 Model-Dependent Integration Tests

Tests that load real model weights from `.models/` should:

- Reference the path via the `AETHERMIND_TEST_MODELS_DIR` macro (defined in [tests/unit/CMakeLists.txt](file:///home/richard/project/AetherMind/tests/unit/CMakeLists.txt)).
- Fail fast with `ASSERT_TRUE(ok)` if the fixture is missing, so the failure is clear rather than a mysterious null deref.

```cpp
fs::path TestModelDir() {
    return fs::path(AETHERMIND_TEST_MODELS_DIR) / "tiny-random-LlamaForCausalLM";
}
```

---

## 10. Determinism and Isolation

- Tests must not depend on execution order. Do not rely on global state mutated by an earlier test.
- When a test touches a global singleton (cache, registry), reset it in `SetUp`/`TearDown` (see `ThreadCacheTest` above).
- Use fresh fixtures or local variables per test. Avoid sharing mutable state across tests in the same file.
- For random data, seed the RNG explicitly (`std::mt19937 rng(42);`) so failures are reproducible.

---

## 11. What to Test

### 11.1 Public API

Test the public contract of each module: inputs, outputs, documented error conditions, and invariants stated in headers.

### 11.2 Edge Cases

Always cover at minimum:

- empty input (empty string, zero-size tensor, empty vector)
- boundary sizes (SSO capacity boundary, max representable value)
- invalid input that must be rejected (bad dtype, null pointer for non-empty size)
- idempotent operations applied twice

### 11.3 Error Paths

Verify that invalid inputs produce the documented `Status` code or abort via `AM_CHECK` (death test). Do not let invalid input silently succeed.

### 11.4 What Not to Test

- Private implementation details that have no observable behavior.
- Standard library facilities (do not test `std::vector`).
- Trivial getters/setters with no logic.

---

## 12. Checklist for New Tests

Before submitting a new test file or suite, verify:

- [ ] File is placed under the directory mirroring the source module.
- [ ] Suite and test names are PascalCase and descriptive.
- [ ] No `DISABLED_` prefix, no commented-out `GTEST_SKIP()`.
- [ ] Each test verifies one behavior; AAA layout is clear.
- [ ] `ASSERT_*` used only where continuing would crash.
- [ ] Death tests match `"Check failed"` and are guarded by `#ifndef NDEBUG` when debug-only.
- [ ] Helpers are in the anonymous namespace or a shared `test_*.h`.
- [ ] Global state is reset in `SetUp`/`TearDown`.
- [ ] Random data uses an explicit seed.
- [ ] The narrowest relevant `--gtest_filter` runs green locally.
