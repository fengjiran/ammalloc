#!/usr/bin/env python3
"""Checks that allocator-core sources stay free of recursion and throw sources.

The checked files run on allocation, deallocation, metadata OOM, or mutex
failure paths. They may not use spdlog because logger initialization and
formatting can allocate through the allocator being implemented. They may not
construct std::lock_guard or std::unique_lock either: both can throw
std::system_error, which would reach std::terminate at the noexcept boundaries
these paths are declared with. detail::NoThrow* guards are the required
substitute.
"""

import argparse
import re
import sys
from pathlib import Path


CORE_FILES = (
    "include/ammalloc/page_allocator.h",
    "src/page_allocator.cpp",
    "include/ammalloc/page_cache.h",
    "src/page_cache.cpp",
    "include/ammalloc/central_cache.h",
    "src/central_cache.cpp",
    "src/ammalloc.cpp",
)
FORBIDDEN_SPDLOG = re.compile(r"\bspdlog::")
FORBIDDEN_POOL_NEW = re.compile(r"\b(?:span_pool_|radix_node_pool_)\.New\s*\(")
FORBIDDEN_STD_LOCK = re.compile(r"\bstd::(?:lock_guard|unique_lock)\b")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    args = parser.parse_args()

    problems = []
    for relative_path in CORE_FILES:
        path = args.root / relative_path
        text = path.read_text(encoding="utf-8")
        for line_no, line in enumerate(text.splitlines(), 1):
            if FORBIDDEN_SPDLOG.search(line):
                problems.append(f"{relative_path}:{line_no}: allocator core must not use spdlog")
            if FORBIDDEN_POOL_NEW.search(line):
                problems.append(
                    f"{relative_path}:{line_no}: metadata pools must use no-throw TryNew"
                )
            if FORBIDDEN_STD_LOCK.search(line):
                problems.append(
                    f"{relative_path}:{line_no}: "
                    "allocator core must use detail::NoThrow lock guards, std locks can throw"
                )

    for problem in problems:
        print(f"FAIL: {problem}")
    print(f"{'FAIL' if problems else 'OK'}: {len(problems)} problem(s) found")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
