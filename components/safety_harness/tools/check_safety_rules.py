#!/usr/bin/env python3

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(sys.argv[1]).resolve()
PROJECT_DIRS = [ROOT / "main", ROOT / "components"]
IGNORED_PREFIXES = (
    ROOT / "components" / "espressif__",
    ROOT / "managed_components",
)
IGNORED_FILES = {
    ROOT / "components" / "safety_harness" / "safety_harness.c",
    ROOT / "components" / "safety_harness" / "include" / "safety_harness.h",
}

BANNED_PATTERNS = {
    "raw malloc": re.compile(r"(?<![A-Za-z0-9_])malloc\s*\("),
    "raw calloc": re.compile(r"(?<![A-Za-z0-9_])calloc\s*\("),
    "raw realloc": re.compile(r"(?<![A-Za-z0-9_])realloc\s*\("),
    "raw free": re.compile(r"(?<![A-Za-z0-9_])free\s*\("),
    "unsafe strcpy": re.compile(r"(?<![A-Za-z0-9_])strcpy\s*\("),
    "unsafe strcat": re.compile(r"(?<![A-Za-z0-9_])strcat\s*\("),
    "unsafe sprintf": re.compile(r"(?<![A-Za-z0-9_])sprintf\s*\("),
    "unsafe vsprintf": re.compile(r"(?<![A-Za-z0-9_])vsprintf\s*\("),
    "unsafe gets": re.compile(r"(?<![A-Za-z0-9_])gets\s*\("),
    "unsafe alloca": re.compile(r"(?<![A-Za-z0-9_])alloca\s*\("),
}


def iter_project_sources() -> list[Path]:
    files: list[Path] = []
    for project_dir in PROJECT_DIRS:
        if not project_dir.exists():
            continue
        for path in project_dir.rglob("*"):
            if not path.is_file():
                continue
            if path.suffix.lower() not in {".c", ".cc", ".cpp", ".h", ".hpp"}:
                continue
            if path in IGNORED_FILES:
                continue
            if any(str(path).startswith(str(prefix)) for prefix in IGNORED_PREFIXES):
                continue
            files.append(path)
    return files


def scan_file(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8", errors="ignore")
    hits: list[str] = []
    for label, pattern in BANNED_PATTERNS.items():
        if pattern.search(text):
            hits.append(label)
    return hits


def main() -> int:
    violations: list[str] = []
    for path in iter_project_sources():
        hits = scan_file(path)
        if hits:
            violations.append(f"{path.relative_to(ROOT)}: {', '.join(hits)}")

    if violations:
        print("safety harness violations found:")
        for line in violations:
            print(f"- {line}")
        return 1

    print("safety harness scan passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
