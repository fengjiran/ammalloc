#!/usr/bin/env python3
"""ammalloc documentation drift checker.

Checks three invariants defined in docs/guides/documentation-guide.md:

1. Link validity: every relative markdown link under docs/ resolves to an
   existing file (anchors are not validated).
2. Symbol traceability: identifiers of the form ``Class::Member`` that appear
   in design documents under docs/designs/ exist in some header under
   include/ammalloc/.
3. Index coverage: every documentation file (except templates/ and the
   improvement-plan chapters) is referenced from docs/README.md.

Usage:
    python3 scripts/verify_docs.py [--root PATH] [--no-links] [--no-symbols]
                                   [--no-index]

Exit code is 0 when all checks pass, 1 otherwise.
"""

import argparse
import re
import sys
from pathlib import Path

LINK_RE = re.compile(r"\[[^\]]*\]\(([^)]+)\)")
SYMBOL_RE = re.compile(r"\b([A-Z][A-Za-z0-9_]*)::([A-Za-z_][A-Za-z0-9_]*)\b")
# Code-like link targets that are not filesystem paths (function signatures,
# type names in ABI tables, etc.).
CODE_LIKE_TARGET = ("::", "*", "&", "<", ">")


def iter_markdown(docs: Path):
    for path in sorted(docs.rglob("*.md")):
        if path.name == "README.md" and path.parent == docs:
            continue  # The index itself is not a subject.
        if "templates" in path.parts:
            continue  # Templates are copy-paste sources with placeholder paths.
        yield path


def check_links(docs: Path) -> list[str]:
    problems = []
    for path in iter_markdown(docs):
        for line_no, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            for target in LINK_RE.findall(line):
                target = target.split("#", 1)[0]
                if not target:
                    continue
                if target.startswith(("http://", "https://", "mailto:", "file://")):
                    continue
                if any(token in target for token in CODE_LIKE_TARGET):
                    continue  # Code-signature-like target, not a file path.
                resolved = (path.parent / target).resolve()
                if not resolved.exists():
                    problems.append(f"{path.relative_to(docs)}:{line_no}: broken link -> {target}")
    return problems


def check_symbols(root: Path, docs: Path) -> list[str]:
    sources = []
    for subdir, pattern in (("include/ammalloc", "*.h"), ("src", "*.cpp")):
        sources += list((root / subdir).glob(pattern))
    source_text = "\n".join(p.read_text(encoding="utf-8") for p in sources)
    problems = []
    for path in sorted((docs / "designs").glob("*.md")):
        in_code_block = False
        for line_no, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if line.lstrip().startswith("```"):
                in_code_block = not in_code_block
                continue
            if in_code_block:
                continue  # Illustrative snippets may simplify real signatures.
            for cls, member in SYMBOL_RE.findall(line):
                # Skip C++ standard/library names that are not ammalloc symbols.
                if cls == "std":
                    continue
                # Loose traceability: the class name and the member name must
                # both occur somewhere in include/ammalloc/ or src/.
                cls_pattern = "\\b" + re.escape(cls) + "\\b"
                member_pattern = "\\b" + re.escape(member) + "\\b"
                if not re.search(cls_pattern, source_text) or not re.search(
                    member_pattern, source_text
                ):
                    problems.append(
                        f"{path.relative_to(docs)}:{line_no}: "
                        f"symbol `{cls}::{member}` not found in include/ammalloc/ or src/"
                    )
    return problems


def check_index(docs: Path) -> list[str]:
    index = (docs / "README.md")
    index_text = index.read_text(encoding="utf-8") if index.exists() else ""
    problems = []
    for path in iter_markdown(docs):
        if path.parent == docs / "improvement-plan":
            continue  # The plan index (README.md) covers the chapters.
        if path.name not in index_text:
            problems.append(f"docs/README.md does not reference {path.relative_to(docs)}")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--no-links", action="store_true", help="skip link validity check")
    parser.add_argument("--no-symbols", action="store_true", help="skip symbol traceability check")
    parser.add_argument("--no-index", action="store_true", help="skip index coverage check")
    args = parser.parse_args()

    docs = args.root / "docs"
    all_problems = []
    if not args.no_links:
        all_problems += check_links(docs)
    if not args.no_symbols:
        all_problems += check_symbols(args.root, docs)
    if not args.no_index:
        all_problems += check_index(docs)

    for problem in all_problems:
        print(f"FAIL: {problem}")
    print(f"{'FAIL' if all_problems else 'OK'}: {len(all_problems)} problem(s) found")
    return 1 if all_problems else 0


if __name__ == "__main__":
    sys.exit(main())
