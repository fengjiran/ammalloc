# C++ Comment Guidelines

This document defines two things:

1. **Comment Style Guide** — how comments should be written and formatted
2. **Documentation Content Standard** — what information comments should contain

Comments must improve understanding, not repeat the code.

Scope and precedence:

- This document expands comment conventions used in this repository.
- If this document conflicts with `AGENTS.md` or verified repository constraints, follow `AGENTS.md` and repository facts.
- Examples that use STL containers are illustrative; subsystem-specific constraints still apply (for example, `ammalloc` code must follow `ammalloc/AGENTS.md` §4.1 and avoid heap-allocating STL containers).

---

## 1. Comment Style Guide

### 1.1 General Style Rules

- Prefer fewer, higher-value comments over many low-value comments.
- Keep comments accurate; fix or remove them when code changes.
- Use English unless the project explicitly requires another language.
- Write full sentences for public API docs and important design comments.
- Short inline comments may be sentence fragments if clear.
- Avoid jokes, sarcasm, and personal notes.
- Avoid ambiguous words like "just", "obviously", or "simply".
- If better naming or refactoring would remove the need for a comment, prefer improving the code first.

---

### 1.2 Comment Forms

#### Public API comments

Use Doxygen documentation comments for non-trivial public APIs in header
files. Comments must use the `///` marker and structured Doxygen tags so
the documentation can be extracted by Doxygen without ambiguity.

Comment form:

- Use `///` for all documentation comment lines.
- Do not mix `/** ... */` block comments with `///` in the same API.
- Place the comment block immediately before the declaration it documents.

Mandatory tags:

- `@brief` — one-line summary. Always required for public API.
- `@param` — one per function parameter, required when the function has
  parameters. Omit for zero-argument functions.
- `@return` — required when the function returns a non-void value. Omit
  for `void` functions and constructors.
- `@throws` — required when the function may throw. Omit when the function
  is `noexcept` or reports errors via return value/status only.
- `@tparam` — one per template parameter, required when the template
  parameter has a non-trivial contract. Omit for trivial template
  parameters whose meaning is obvious from the name.

Optional tags (use when relevant):

- `@pre`, `@post`, `@note`, `@warning`, `@see`, `@deprecated`

Do not force every API to fill every optional tag. Document what is
necessary for correct use and maintenance.

Example:

```cpp
/// @brief Parses a configuration file and returns the loaded options.
///
/// @param path Path to the configuration file.
/// @return Parsed configuration object.
/// @throws std::runtime_error if the file cannot be read or parsed.
/// @pre `path` is a valid filesystem path.
Config parse_config(const std::string& path);
```

Non-example (plain prose without tags, not acceptable for public API):

```cpp
// Returns the parsed config. Throws on read failure.
Config parse_config(const std::string& path);
```

#### Internal implementation comments

Use `//` for implementation comments in `.cc` / `.cpp` files.

Example:

```cpp
// Keep the previous buffer alive until all async callbacks complete.
pending_buffers_.push_back(std::move(buffer));
```

#### Block comments

Avoid `/* ... */` for ordinary implementation comments.

Use block comments only for:

- API documentation
- copyright/license headers
- rare multi-paragraph explanations

------

### 1.3 Comment Placement

#### File-level comments

Add a file-level comment to non-trivial public headers and source files whose role is not obvious from the file name alone.

Do not force heavy file headers on trivial files.

Placement relative to the include guard:

1. Copyright/license header (if any) goes **before** the include guard, using `//`.
2. The file-level Doxygen comment (`/// @file ...`) goes **after** the
   include guard (`#ifndef`/`#define`) and **before** the first `#include`.
3. Entity-level documentation follows includes.

Example:

```cpp
// Copyright 2026 The AetherMind Authors
// SPDX-License-Identifier: Apache-2.0

#ifndef AETHERMIND_MODULE_FILE_H
#define AETHERMIND_MODULE_FILE_H

/// @file file.h
/// @brief One-line summary of the module.
///
/// Optional longer description.
#include "..."

namespace aethermind { ... }
#endif
```

#### Class and struct comments

Document classes or structs when their role, lifecycle, invariants, or synchronization model are non-trivial.

#### Member comments

Comment data members only when the meaning is not obvious from the name and type, or when invariants/locking rules matter.

#### Function body comments

Inside function bodies, comment only the tricky parts.

Do not narrate each line.

#### Templates and generic code

Document template assumptions only when they are non-trivial.

#### Macros

Document non-trivial macros, especially public or widely used ones.

------

### 1.4 Formatting Rules

- Keep comments close to the code they describe.
- Use one space after `//`.
- Prefer sentence case.
- Keep multi-line comments aligned consistently.
- Keep comment formatting compatible with project tooling.

Example:

```cpp
// Keep the old state alive until the async flush completes.
// The callback may still read from `buffer`.
```

------

### 1.5 TODO / FIXME / HACK / NOTE Format

Use structured markers sparingly and intentionally.

#### TODO

Use for planned future work that is safe to defer.

Format:

```cpp
// TODO(username): Support batched eviction once metrics are available.
```

#### FIXME

Use for known incorrect behavior that should be fixed.

Format:

```cpp
// FIXME(username): This fails for empty input because the parser assumes one token.
```

#### HACK

Use only for intentional ugly workarounds.

Format:

```cpp
// HACK(username): Keep the extra copy to avoid a use-after-free in the legacy callback path.
```

#### NOTE

Use for important non-actionable context.

Format:

```cpp
// NOTE: This field is serialized and must remain backward-compatible.
```

------
### 1.6 Tool Integration

Tools used by this repository for comment formatting and validation:

- **clang-format**: Configure `CommentPragmas` for special markers.
- **clang-tidy**: Enable `bugprone-suspicious-missing-comma` and related checks.
- **doxygen**: Public API documentation must be Doxygen-extractable.
  Recommended configuration: `EXTRACT_ALL=NO`, `EXTRACT_PRIVATE=NO`,
  `JAVADOC_AUTOBRIEF=NO` (use explicit `@brief` tags), `QT_AUTOBRIEF=NO`.
  Explicit `@brief` tags are mandatory; do not rely on auto-brief extraction.

---

## 2. Documentation Content Standard

### 2.1 Core Principle

Comments should explain:

- intent
- assumptions
- invariants
- ownership and lifetime
- thread-safety expectations
- non-obvious tradeoffs
- performance constraints
- reasons for unusual code

Do **not** use comments to mechanically translate obvious code into English.

Prefer explaining **why** over explaining **what**.

Prefer explaining **constraints** and **invariants** over explaining syntax.

------

### 2.2 What to Document

#### Intent

Document the purpose of non-obvious code.

Good:

```cpp
// Reserve slot 0 as an invalid handle sentinel.
handles_.push_back({});
```

Bad:

```cpp
// Push back an empty element.
handles_.push_back({});
```

#### Invariants

Document important invariants that must remain true.

Examples:

```cpp
// `free_list_` never contains duplicate indices.
// `head_` is null iff the queue is empty.
```

#### Ownership and lifetime

Document ownership transfer, borrowing, and lifetime assumptions.

Examples:

```cpp
// Ownership is transferred to the scheduler.
scheduler_->enqueue(std::move(task));
// Borrowed pointer. The caller must ensure `ctx` outlives this parser.
Parser(Context* ctx);
// `view` points into `storage_`; do not mutate `storage_` while it is in use.
std::string_view current_view() const;
```

#### Thread-safety and synchronization

Document locking requirements and concurrency assumptions.

Examples:

```cpp
// Must be called with `mutex_` held.
void unlink_node(Node* node);
// Protected by `sessions_mu_`.
std::unordered_map<SessionId, Session> sessions_;
// Release store publishes initialized state to worker threads.
initialized_.store(true, std::memory_order_release);
```

Document lock ordering, memory order choices, and progress guarantees when non-obvious.

#### Error handling

Document non-obvious failure modes and reporting behavior.

Examples:

```cpp
/// Returns false on validation failure; does not throw.
bool validate(const Request& req);
// Ignore EINTR and retry because the operation is logically idempotent.
```

#### Performance-sensitive choices

Document surprising code when performance, scale, or layout justifies it.

Examples:

```cpp
// Linear scan is intentional here: the list is capped at <= 8 entries.
// Avoid `std::function` here to keep the hot path allocation-free.
// Keep this layout packed to improve cache locality during traversal.
```

#### Compatibility and persistence constraints

Document behavior that must stay stable because of ABI, file format, protocol, or platform constraints.

Examples:

```cpp
// Keep this field order stable: it is part of the on-disk binary format.
// Work around MSVC miscompilation in version 19.3x.
```

------

### 2.3 Public API Documentation Standard

Use Doxygen documentation comments (`///`) for:

- public classes
- public structs
- public enums when semantics are non-obvious
- non-trivial public functions and methods
- protected extension points with non-obvious contracts

Mandatory Doxygen tags (see §1.2):

- `@brief` — always required.
- `@param` — required for each parameter when the function has parameters.
- `@return` — required when the function returns a non-void value.
- `@throws` — required when the function may throw.
- `@tparam` — required for non-trivial template parameters.

Optional tags: `@pre`, `@post`, `@note`, `@warning`, `@see`, `@deprecated`.

A public API comment should additionally cover the relevant subset of these
questions in the tag body or a follow-up paragraph:

- what does it do (covered by `@brief`)
- what are the preconditions (`@pre`)
- what are the postconditions (`@post`)
- who owns the returned object or referenced memory (`@return` body or `@note`)
- is it thread-safe (`@note`)
- can it block (`@note`)
- can it throw; if not, how are errors reported (`@throws` or `@note`)
- are there complexity or performance guarantees (`@note`)

Do **not** force every API to fill every optional tag. Document what is
necessary for correct use and maintenance.

Example:

```cpp
/// @brief Inserts a value if the key is not already present.
///
/// @param key Key to insert. Ownership is transferred.
/// @param value Value associated with the key.
/// @return True if insertion occurred, false if the key already existed.
/// @note This method is not thread-safe. Average complexity is O(1).
bool insert(std::string key, Value value);
```

------

### 2.4 File-Level Documentation Standard

A file-level comment may describe:

- module purpose
- ownership model
- concurrency model
- major invariants
- format/protocol constraints
- important dependencies

Add file-level comments only when they improve understanding.

Mandatory tags:

- `@file` — always required, so Doxygen associates the block with the file.
  With `JAVADOC_AUTOBRIEF=NO` / `QT_AUTOBRIEF=NO` (see §1.6), auto-detection
  does not apply; without `@file` the block is treated as the first entity's
  documentation instead.
- `@brief` — one-line summary, always required.

Optional tags: `@note`, `@warning`, `@see`.

------

### 2.5 Class and Struct Documentation Standard

A class comment should describe the relevant subset of:

- responsibility
- ownership model
- thread-safety model
- major invariants
- usage restrictions

Example:

```cpp
/// @brief Thread-safe bounded queue for producer-consumer workloads.
///
/// Multiple producers and consumers are supported.
/// `push` blocks when the queue is full.
/// `pop` blocks when the queue is empty.
/// Destruction requires that no threads are blocked inside the queue.
template <typename T>
class BoundedQueue {
    ...
};
```

------

### 2.6 Member Documentation Standard

Comment data members only when the meaning is not obvious from the name and type, or when invariants/locking rules matter.

Good:

```cpp
// Protected by `mutex_`.
std::vector<Job> pending_;
// Immutable after construction.
const std::size_t capacity_;
```

Bad:

```cpp
// Pending jobs.
std::vector<Job> pending_;
```

------

### 2.7 Function Body Documentation Standard

Inside function bodies, comment only the tricky parts:

- non-obvious control flow
- invariants
- lock expectations
- memory ordering
- ownership/lifetime constraints
- algorithmic choices
- unusual error-handling behavior

Preferred pattern:

```cpp
// Fast path: avoid taking the lock when the cache is already initialized.
if (ready_.load(std::memory_order_acquire)) {
    return value_;
}
```

Avoid comments that merely narrate each line.

------

### 2.8 Template and Generic Code Documentation Standard

Document template assumptions explicitly when they are non-trivial.

Examples:

```cpp
/// @brief Filters elements of `input` for which `fn` returns true.
///
/// @tparam T Element type. Must be nothrow-move-constructible for the
///           strong exception guarantee.
/// @tparam Fn Filter predicate. Must be invocable as `bool(const T&)`.
/// @param input Source vector to filter.
/// @param fn Predicate invoked per element.
/// @return Vector containing elements for which `fn` returned true.
template <class T, class Fn>
std::vector<T> filter(const std::vector<T>& input, Fn&& fn);
```

Avoid turning template comments into generic textbook explanations.

------

### 2.9 Macro Documentation Standard

When relevant, explain:

- purpose
- parameters
- evaluation hazards
- side effects
- required usage constraints
- portability implications
- ABI/API impact

Example:

```cpp
// Evaluates `expr` exactly once and aborts in debug builds if it is false.
#define AM_ASSERT(expr) ...
```

If a macro is trivial and local, do not over-document it.

------

### 2.10 TODO / FIXME / HACK / NOTE Content Standard

Rules:

- every TODO/FIXME/HACK should be actionable
- prefer linking an issue number when available
- remove markers once resolved
- do not use vague markers such as `TODO: improve this`

A HACK must state:

- why it exists
- what risk it avoids
- when it can be removed

------

### 2.11 What Not to Document

Do not comment code that is already obvious.

Bad:

```cpp
// Increment index.
++index;
// Return success.
return true;
```

Do not leave commented-out code in the repository.

Bad:

```cpp
// old implementation
// process_legacy(data);
```

Use version control instead.

Do not add comments that merely restate names.

Bad:

```cpp
int timeout_ms;  // timeout in ms
```

Unless the unit or semantics are genuinely important and non-obvious.

------

### 2.12 Review Standard for Comments

When adding or reviewing comments, check:

- does the comment explain something non-obvious
- is it still true
- does it describe intent, invariants, or constraints
- would the code be clearer if renamed or refactored instead
- is the comment at the right level of abstraction
- is it consistent with nearby comments and project terminology

If a comment only compensates for bad naming or poor structure, prefer fixing the code first.

------

### 2.13 Examples

#### Good examples

```cpp
// Skip index 0 because it is reserved as an invalid handle sentinel.
// Protected by `mutex_`.
std::deque<Task> pending_;
/// Returns a borrowed view into internal storage.
/// The returned view becomes invalid after the next call to `append`.
std::string_view view() const;
// Linear search is intentional: the array never grows beyond 8 elements.
```

#### Bad examples

```cpp
// Add item to vector.
items.push_back(item);
// This is a mutex.
std::mutex mutex_;
// TODO: optimize
// old code
// foo();
```

------

### 2.14 Summary

Use comments to document:

- why the code exists
- what assumptions it relies on
- what must remain true
- how ownership, lifetime, and concurrency work
- why a non-obvious design or performance tradeoff is justified

Do not comment obvious code.
Do not keep stale comments.
Do not use comments as a substitute for clear naming and good structure.
