# C++ Coding Style Guidelines

Scope and precedence:

- This document defines practical coding style guidance for AetherMind.
- If this document conflicts with `AGENTS.md` or verified repository constraints, follow `AGENTS.md` and repository facts.
- Product-scope constraints (for example, Phase 1 boundaries) are defined in `docs/products/aethermind_prd.md`.
- Subsystem-specific constraints override generic guidance (for example, `ammalloc/AGENTS.md`).

## Goals

Code must prioritize:

- correctness
- readability
- maintainability
- explicit ownership and lifetime
- predictable performance

Prefer clear and boring code over clever code.

Optimize only when there is a concrete reason.

------

## General Principles

- Prefer simple, local, easy-to-verify code.
- Prefer small, focused functions over long functions.
- Prefer explicit behavior over hidden magic.
- Prefer standard library facilities over custom utilities unless there is a strong reason.
- Prefer code that is easy to review over code that is merely short.
- Preserve existing project conventions unless there is a strong local reason to improve them.

------

## Language Standard

- This repository uses C++20 (`CMAKE_CXX_STANDARD 20`).
- Use newer language features only when they improve clarity, safety, or correctness.
- Do not introduce modern features purely for style.
- `std::expected` is optional and depends on toolchain support; use custom `Expected<T, E>` when unavailable (see `AGENTS.md`).

------

## Naming

### General Rules

- Use names that describe intent, not implementation trivia.
- Avoid abbreviations unless they are standard and widely understood.
- Avoid single-letter names except for:
  - trivial loop indices
  - short-lived local variables in very small scopes
  - conventional mathematical notation

### Preferred Naming Style

Use the project’s established naming convention consistently.

If no project convention exists, use:

- `snake_case` for variables and functions
- `PascalCase` for types
- `kPascalCase` for enum members
- `AM_UPPER_SNAKE_CASE` for project macros/constants
- trailing underscore for private data members if the project already uses it

Example:

```cpp
class RequestParser {
public:
    bool parse_request(std::string_view input);

private:
    std::string buffer_;
    std::size_t next_offset_ = 0;
};
```

------

## File Organization

- Headers should declare interfaces.
- Source files should contain implementation details.
- Do not put heavy implementation in headers unless required for templates, `inline`, or performance-sensitive reasons.
- Keep headers self-contained where practical.
- Minimize transitive includes.
- Prefer forward declarations when they reduce coupling and do not hurt clarity.

------

## Includes

- Keep includes minimal and relevant.
- Prefer the corresponding header first in `.cpp` files, then project headers, then third-party headers, then standard library headers.
- Remove unused includes.
- Do not rely on indirect includes.

Example order:

```cpp
#include "parser/request_parser.h"

#include "common/status.h"
#include "net/socket.h"

#include <string>
#include <string_view>
#include <vector>
```

------

## Functions

### Function Size

- Prefer short functions with a single clear responsibility.
- Split large functions when doing so improves readability and testing.
- Do not split code into tiny wrappers that obscure the actual logic.

### Parameters

- Prefer passing cheap scalar types by value.
- Prefer `const T&` for large read-only objects.
- Prefer `T&&` only when move semantics are actually intended.
- Prefer `std::string_view` for read-only string-like inputs when lifetime is safe.
- Prefer explicit parameter objects when a function takes many related arguments.

### Return Values

- Prefer returning values over output parameters.
- Use output parameters only when there is a strong reason.
- Prefer strong, self-explanatory return types.

Good:

```cpp
ParseResult parse_config(std::string_view text);
```

Avoid:

```cpp
bool parse_config(const std::string& text, Config* out_config, Error* out_error);
```

unless that pattern is already standard in the codebase.

------

## Types

- Prefer concrete, simple types when possible.
- Prefer `auto` when:
  - the type is obvious from the initializer
  - the type is verbose and repeating it hurts readability
  - it prevents accidental narrowing or duplication of complex type names
- Do not use `auto` when it hides important type information.

Good:

```cpp
auto it = items.find(key);
```

Bad:

```cpp
auto value = get_critical_policy_state();
```

if the actual type matters for understanding.

------

## Const Correctness

- Use `const` consistently for values that should not change.
- Mark member functions `const` when they do not mutate observable state.
- Prefer immutability for local variables unless mutation improves clarity.

Example:

```cpp
const auto end = values.end();
for (auto it = values.begin(); it != end; ++it) {
    ...
}
```

------

## References, Pointers, and Ownership

### Ownership Rules

- Prefer explicit ownership.
- Prefer RAII.
- Avoid raw owning pointers.
- Use raw pointers or references only for non-owning access unless the project has a different established convention.

### Preferred Tools

- `ObjectPtr<T>` for intrusive reference-counted object ownership in project object model
- `String` (project type) instead of `std::string` where project APIs require it
- `std::unique_ptr` for exclusive ownership in non-intrusive ownership paths
- `std::shared_ptr` only when shared lifetime is genuinely required and `ObjectPtr<T>` is not applicable
- raw pointer for nullable non-owning access
- reference for non-null non-owning access

### Avoid

- unclear ownership transfer
- implicit lifetime assumptions
- hidden aliasing
- unnecessary heap allocation

Good:

```cpp
std::unique_ptr<Node> make_node();
void process(const Request& request);
void attach(Session* session);  // non-owning, nullable
```

------

## Resource Management

- Manage resources through objects with clear lifetime.
- Prefer constructors/destructors, scoped guards, and containers.
- Avoid manual cleanup paths scattered across functions.
- Keep acquisition and release logic close together.

------

## Error Handling

- Follow the project’s established error model consistently.
- Prefer project macros for consistency: `AM_THROW`, `AM_CHECK`, `AM_DCHECK`, `AM_UNREACHABLE`.
- Use status/result-style returns only where the subsystem API already uses that contract.
- Error paths must be readable and explicit.
- Do not swallow errors silently.

Good:

```cpp
auto config = load_config(path);
if (!config.ok()) {
    return config.error();
}
```

Project-style example:

```cpp
AM_CHECK(idx < size(), "index {} out of range {}", idx, size());
if (bad_state) {
    AM_THROW(ErrorKind::InvalidArgument) << "invalid runtime state";
}
```

------

## Control Flow

- Prefer straightforward control flow.
- Avoid deeply nested conditionals when early returns improve readability.
- Prefer guard clauses for invalid states and preconditions.
- Avoid complex boolean expressions when intermediate named variables would help.

Good:

```cpp
if (!session) {
    return Status::InvalidArgument("session is null");
}

if (!session->is_ready()) {
    return Status::FailedPrecondition("session not ready");
}
```

------

## Classes

- Each class should have one clear responsibility.
- Keep class invariants simple and explicit.
- Prefer composition over inheritance unless polymorphism is clearly needed.
- Keep public interfaces small and coherent.
- Avoid large “god objects”.

### Data Members

- Group related members together.
- Keep invariants obvious from ordering and naming.
- Initialize members where possible.

------

## Constructors and Initialization

- Prefer member initializer lists.
- Prefer in-class member initializers for simple defaults.
- Make single-argument constructors `explicit` unless implicit conversion is clearly intended.
- Avoid heavy work in constructors unless the type semantics require it.

Example:

```cpp
class Cache {
public:
    explicit Cache(std::size_t capacity);

private:
    std::size_t capacity_ = 0;
};
```

------

## Enums

- Prefer `enum class` over unscoped enums.
- Use enums when a fixed closed set of values exists.
- Avoid using integers where an enum expresses intent better.

Example:

```cpp
enum class LogLevel {
    Error,
    Warning,
    Info,
    Debug
};
```

------

## Templates and Generic Code

- Use templates only when genericity is actually needed.
- Keep template interfaces constrained and understandable.
- Document assumptions about template parameters.
- Avoid overly clever metaprogramming unless there is a strong justification.

Prefer readable generic code over advanced type trickery.

------

## STL Usage

- Prefer standard containers and algorithms.
- Prefer algorithms when they improve clarity.
- Prefer range-based `for` loops when appropriate.
- Avoid forcing algorithms into places where a plain loop is clearer.
- Choose containers based on access pattern and ownership needs, not habit.

Examples:

- `std::vector` as default sequence container
- `std::array` for fixed-size data
- `std::string_view` for borrowed string input
- `std::optional` for “maybe present”
- `std::variant` for closed alternatives
- `std::span` for borrowed contiguous ranges when available in the project standard

Note:

- The STL guidance above is general and does not override subsystem constraints.
- In `ammalloc`, avoid heap-allocating STL containers and regular heap `new`/`delete` to prevent allocator recursion; follow `ammalloc/AGENTS.md` and `AGENTS.md`.

------

## Performance

- Prefer clarity first, then optimize bottlenecks.
- Avoid unnecessary allocations and copies.
- Be careful in hot paths with:
  - hidden temporaries
  - repeated string construction
  - repeated lookups
  - unnecessary virtual dispatch
- Document performance-sensitive choices when they are non-obvious.
- Do not claim performance wins without evidence.

------

## Concurrency

- Concurrency design must be explicit.
- Clearly document which mutex protects which state.
- Keep critical sections small but understandable.
- Avoid mixing atomic and mutex-based synchronization casually.
- Prefer simple synchronization schemes over clever lock-free logic unless there is a real need.
- Thread-affinity assumptions must be explicit in names, comments, or API design.

------

## Macros

- Avoid macros unless they are required for:
  - platform compatibility
  - conditional compilation
  - carefully scoped compile-time utilities
- Do not use macros where constants, inline functions, templates, or enums are better.
- Macro names must be obvious and isolated.

------

## Comments

Follow the project comment guidelines.

In particular:

- use comments to explain intent, invariants, ownership, lifetime, concurrency, and non-obvious tradeoffs
- do not comment obvious code
- do not leave commented-out code
- keep comments accurate

------

## Formatting

- Follow the project formatter if one exists.
- If the project uses `clang-format`, do not hand-format against it.
- Use consistent indentation and spacing.
- Align pointers with the type: `Type* ptr`.
- Prefer line breaks that improve readability over dense one-line expressions.
- Keep declarations and control flow visually simple.

------

## Logging

- Log actionable information.
- Do not log noise in hot paths unless explicitly required.
- Include enough context for debugging, but avoid leaking sensitive data.
- Use appropriate log levels consistently.

------

## Tests

- Add or update tests for behavior changes.
- Prefer focused unit tests over broad brittle tests.
- Cover edge cases, invariants, and failure modes.
- Do not change production code only to satisfy a weak test if the test should be fixed instead.
- Keep tests readable and deterministic.

------

## Build System and Project Integration

- Respect the existing build structure.
- Do not rename targets, move files, or change build organization unless necessary.
- Keep compile-time impact in mind when changing widely included headers.
- Prefer focused target-level validation over rebuilding everything.

------

## Change Scope Rules

- Make the smallest correct change that fully solves the task.
- Do not perform drive-by refactors unless they are required for correctness or requested explicitly.
- Keep diffs reviewable.
- Separate mechanical cleanup from logic changes when practical.

------

## Review Checklist

When writing or reviewing code, check:

- Is the code correct?
- Is ownership clear?
- Are lifetime assumptions safe?
- Is the API easy to understand?
- Is control flow straightforward?
- Are edge cases handled?
- Are concurrency assumptions explicit?
- Is performance acceptable for the expected scale?
- Is the change scope appropriately small?
- Would a future maintainer understand this quickly?

------

## Preferred Patterns

Prefer:

- RAII
- explicit ownership
- small functions
- early returns
- `enum class`
- standard containers and algorithms
- self-documenting names
- focused tests
- minimal diffs

------

## Avoid

Avoid:

- hidden ownership
- broad refactors without request
- clever template tricks without strong need
- raw owning pointers
- large functions with mixed responsibilities
- inconsistent error handling
- commented-out code
- unexplained performance hacks
- deep nesting when guard clauses are clearer
- ambiguous naming

------

## Summary

Write C++ code that is:

- correct
- explicit
- easy to review
- easy to maintain
- clear about ownership, lifetime, and concurrency
- conservative in scope
- modern where it improves safety and clarity
