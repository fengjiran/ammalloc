# ammalloc

`ammalloc` is a high-performance concurrent userspace memory allocator for Linux.
It uses a layered ThreadCache → CentralCache → PageCache → PageAllocator
architecture to reduce allocation latency and lock contention through a
thread-local lock-free fast path, batched object transfers, and page-level Span
management.

> [!IMPORTANT]
> The project currently exposes the explicit `ammalloc::am_malloc` and
> `ammalloc::am_free` APIs. It does not replace or interpose the system
> `malloc`/`free` functions. Only pass pointers returned by `am_malloc` to
> `am_free`.

## Features

- Small objects up to 32 KiB are served primarily from TLS `ThreadCache` LIFO
  free lists, keeping the common path lock-free.
- `CentralCache` uses size-class buckets with a `TransferCache` and `SpanList`
  to move objects between threads in batches.
- `PageCache` supports Span splitting, owner-shard-local coalescing of adjacent
  free Spans, and lock-free PageMap lookups.
- `PageAllocator` is built on `mmap`, `munmap`, and `madvise`, with a 2 MiB
  mapping cache and transparent huge-page hints.
- A background scavenger periodically applies `MADV_DONTNEED` to long-idle
  Spans to reduce resident physical memory.
- Core metadata uses fixed-size object pools and intrusive structures to avoid
  recursive allocator entry.
- Both 48-bit and 57-bit virtual address configurations are supported.

## Architecture

```text
Application
    │  am_malloc / am_free
    ▼
ThreadCache (per-thread, lock-free fast path, LIFO FreeLists)
    │  batched refill / trim
    ▼
CentralCache (one bucket per size class)
    ├─ TransferCache (SpinLock)
    └─ SpanList      (Mutex + Bitmap)
    │  Span allocation / release
    ▼
PageCache (shard locks, Span split/coalesce, lock-free PageMap reads)
    │  page-level memory
    ▼
PageAllocator (mmap / munmap / madvise)
    │
    ▼
Linux Kernel
```

Requests up to 32 KiB use ThreadCache and CentralCache. Larger requests are
rounded up to whole pages and sent directly to PageCache. Allocated addresses
are recorded in a four-level radix-tree PageMap, allowing `am_free` to locate
the owning Span in O(1) time.

## Requirements

- Linux, because the implementation depends on POSIX/Linux memory-mapping APIs
- CMake 3.28 or later
- A compiler and standard library with C++20 and `std::format` support
- Git and network access for CMake `FetchContent` dependencies during the first
  configuration

Build dependencies include pthread,
[spdlog](https://github.com/gabime/spdlog),
[GoogleTest](https://github.com/google/googletest), and
[Google Benchmark](https://github.com/google/benchmark). Test and benchmark
dependencies are omitted when their respective build options are disabled.

## Quick Start

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target ammalloc -j
```

The build produces the shared library target `ammalloc`. To embed the allocator
in another CMake project, add it as a subdirectory:

```cmake
add_subdirectory(path/to/ammalloc)
target_link_libraries(your_target PRIVATE ammalloc)
```

Include the public header and use the explicit allocation API:

```cpp
#include <ammalloc/ammalloc.h>

#include <cstddef>

int main() {
    constexpr std::size_t size = 1024;
    void* ptr = ammalloc::am_malloc(size);
    if (ptr == nullptr) {
        return 1;
    }

    // Use the storage at ptr, which is at least size bytes long.

    ammalloc::am_free(ptr);
    return 0;
}
```

## API Semantics

```cpp
void* ammalloc::am_malloc(std::size_t size);
void ammalloc::am_free(void* ptr);
```

- `am_malloc(0)` allocates from the minimum size class instead of immediately
  returning null.
- `am_malloc` returns `nullptr` when allocation fails.
- `am_free(nullptr)` is a no-op.
- `am_free` only accepts an original, live pointer returned by `am_malloc`.
  Mixing it with the system `free`, freeing an interior pointer, or double-freeing
  a pointer is unsupported.
- The current implementation does not replace global `new`/`delete` or
  interpose libc allocation functions.

## Build Options

| Option | Default | Description |
| --- | --- | --- |
| `BUILD_TESTS` | `ON` | Build unit tests and enable test-only helpers in the library |
| `BUILD_BENCHMARKS` | `ON` | Build performance benchmarks |
| `USE_57BIT_VA` | `OFF` | Define `AM_USE_57BIT_VA` so PageMap covers a 57-bit virtual address space |

For example, to build only the library with 57-bit virtual address support:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTS=OFF \
  -DBUILD_BENCHMARKS=OFF \
  -DUSE_57BIT_VA=ON
cmake --build build --target ammalloc -j
```

## Runtime Configuration

Runtime configuration is read from the environment when it is first initialized
in the process and remains immutable afterward. Set these variables before the
first call to `am_malloc`.

| Environment variable | Default | Description |
| --- | --- | --- |
| `AM_USE_MAP_POPULATE` | `false` | Add `MAP_POPULATE` to normal `mmap` requests when supported by the platform |
| `AM_ENABLE_SCAVENGER` | `true` | Lazily start the background page scavenger on the first allocation slow path |
| `AM_TC_SIZE` | `32KB` | Parsed and capped at 32 KiB; currently exposed through `RuntimeConfig` but does not change the fixed 32 KiB routing threshold |

Boolean true values are `1`, `true`, `on`, and `yes`, ignoring case and
surrounding whitespace. Byte counts accept the binary suffixes `B`, `K`, `M`,
`G`, and `T`; examples include `16K` and `16KB`.

```bash
AM_USE_MAP_POPULATE=1 AM_ENABLE_SCAVENGER=0 ./your_program
```

## Tests and Benchmarks

The project runs the gtest executable directly and does not register tests with
ctest.

```bash
# Build and run all unit tests.
cmake --build build --target ammalloc_unit_tests -j
./build/tests/unit/ammalloc_unit_tests

# Run a focused unit-test suite.
./build/tests/unit/ammalloc_unit_tests --gtest_filter=ThreadCacheTest.*

# Build and run all benchmarks.
cmake --build build --target ammalloc_benchmarks -j
./build/tests/benchmark/ammalloc_benchmarks

# Run a focused benchmark group.
./build/tests/benchmark/ammalloc_benchmarks --benchmark_filter='BM_PageMap_.*'
```

Performance results depend heavily on the CPU, compiler, build type, kernel,
and machine load. Compare before-and-after results on the same machine with a
Release build and a consistent runtime environment.

## Repository Layout

```text
include/ammalloc/   Public APIs and core data structures
src/                Allocator implementation
tests/unit/         GoogleTest unit tests
tests/benchmark/    Google Benchmark suites
docs/designs/       Architecture and subsystem design documents
docs/guides/        Coding, commenting, testing, and review guidelines
```

See [`docs/designs/ammalloc_design.md`](docs/designs/ammalloc_design.md) for a
deeper implementation overview. Before contributing, read
[`AGENTS.md`](AGENTS.md) and the engineering guidelines under `docs/guides/`.
Any change to a core path should be evaluated for correctness, concurrency
safety, memory usage, and performance regressions.
