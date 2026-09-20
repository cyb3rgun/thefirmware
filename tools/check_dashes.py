#!/usr/bin/env python3
"""Fail if a tracked text file contains a dash that is not a plain hyphen.

Decision D-003: English code and docs, no em dashes. The check covers the
whole dash family, because an en dash between words is the same mistake as
an em dash and is harder to spot.

Usage:
    python tools/check_dashes.py            check every tracked text file
    python tools/check_dashes.py FILE ...   check the named files
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

# The characters are built from code points so that this file itself
# stays free of the characters it bans.
BANNED = {
    chr(0x2010): "hyphen U+2010",
    chr(0x2011): "non breaking hyphen U+2011",
    chr(0x2012): "figure dash U+2012",
    chr(0x2013): "en dash U+2013",
    chr(0x2014): "em dash U+2014",
    chr(0x2015): "horizontal bar U+2015",
    chr(0x2212): "minus sign U+2212",
}

TEXT_SUFFIXES = {
    ".c", ".h", ".cpp", ".hpp", ".py", ".md", ".txt", ".yml", ".yaml",
    ".json", ".cmake", ".sh", ".ps1", ".cfg", ".ini", ".toml", ".csv",
}
TEXT_NAMES = {"CMakeLists.txt", "Kconfig", "Kconfig.projbuild", ".gitignore"}


def tracked_files() -> list[Path]:
    out = subprocess.run(
        ["git", "ls-files"], capture_output=True, text=True, check=True
    ).stdout.splitlines()
    return [Path(p) for p in out if p]


def is_text(path: Path) -> bool:
    return path.suffix in TEXT_SUFFIXES or path.name in TEXT_NAMES


def check(path: Path) -> list[str]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (UnicodeDecodeError, OSError):
        return []
    hits = []
    for lineno, line in enumerate(lines, 1):
        for col, ch in enumerate(line, 1):
            if ch in BANNED:
                hits.append(f"{path.as_posix()}:{lineno}:{col}: {BANNED[ch]}")
    return hits


def main(argv: list[str]) -> int:
    if argv:
        paths = [Path(a) for a in argv]
    else:
        paths = [p for p in tracked_files() if is_text(p)]

    hits: list[str] = []
    for path in paths:
        if path.is_file():
            hits.extend(check(path))

    if hits:
        print(f"check_dashes: {len(hits)} banned dash(es) found")
        for hit in hits:
            print(f"  {hit}")
        print("Replace them with a plain hyphen or reword the sentence.")
        return 1

    print(f"check_dashes: {len(paths)} file(s) clean")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
