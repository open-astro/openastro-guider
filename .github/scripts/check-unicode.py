#!/usr/bin/env python3
"""Scan tracked text files for dangerous/invisible Unicode characters.

Guards against "Trojan Source" attacks (CVE-2021-42574), where bidirectional
control characters or invisible code points make source render differently from
how the compiler reads it.

Run from the repo root:  python3 .github/scripts/check-unicode.py
Exits non-zero (and prints file:line:col U+XXXX) if any forbidden code point is
found. Intentional control characters in tests should be written as escapes
(e.g. "\\u200E") rather than literal characters so the source stays clean.
"""

import subprocess
import sys

# Forbidden code points keyed by integer value, so this scanner is itself pure
# ASCII and never trips over its own table. Limited to characters that are
# unambiguously suspect in a source tree: bidi controls plus zero-width /
# invisible spaces. ZWJ/ZWNJ (U+200C/U+200D) are intentionally omitted because
# they appear in legitimate emoji sequences in Markdown.
FORBIDDEN = {
    # Bidirectional formatting controls (the core Trojan Source vector)
    0x202A: "LEFT-TO-RIGHT EMBEDDING",
    0x202B: "RIGHT-TO-LEFT EMBEDDING",
    0x202C: "POP DIRECTIONAL FORMATTING",
    0x202D: "LEFT-TO-RIGHT OVERRIDE",
    0x202E: "RIGHT-TO-LEFT OVERRIDE",
    0x2066: "LEFT-TO-RIGHT ISOLATE",
    0x2067: "RIGHT-TO-LEFT ISOLATE",
    0x2068: "FIRST STRONG ISOLATE",
    0x2069: "POP DIRECTIONAL ISOLATE",
    0x200E: "LEFT-TO-RIGHT MARK",
    0x200F: "RIGHT-TO-LEFT MARK",
    0x061C: "ARABIC LETTER MARK",
    # Invisible / zero-width spaces
    0x200B: "ZERO WIDTH SPACE",
    0x2060: "WORD JOINER",
    0xFEFF: "ZERO WIDTH NO-BREAK SPACE (BOM)",
}

# Paths under these prefixes are skipped.
#
# locale/ holds the gettext .po translation catalogs. Right-to-left languages
# (Arabic) and some others legitimately embed bidi marks and zero-width spaces
# in translated UI strings -- those are real content, not an attack surface, so
# the catalogs are excluded. Authored source (src/, tests/, scripts, docs) is
# always scanned. Add a prefix here only if an upstream import legitimately
# needs an exception.
EXCLUDE_PREFIXES = (
    "locale/",
)


def tracked_files():
    out = subprocess.run(
        ["git", "ls-files", "-z"],
        check=True,
        capture_output=True,
        text=True,
        encoding="utf-8",  # decode paths as UTF-8 regardless of runner locale
    ).stdout
    for path in out.split("\0"):
        if path and not path.startswith(EXCLUDE_PREFIXES):
            yield path


def main():
    findings = []
    for path in tracked_files():
        try:
            with open(path, "r", encoding="utf-8") as fh:
                lines = fh.readlines()
        except (UnicodeDecodeError, OSError):
            # Binary file (image, native lib, ...) or unreadable -- skip.
            continue
        for lineno, line in enumerate(lines, start=1):
            for col, ch in enumerate(line, start=1):
                name = FORBIDDEN.get(ord(ch))
                if name is not None:
                    findings.append("%s:%d:%d: U+%04X %s" % (path, lineno, col, ord(ch), name))

    if findings:
        print("Forbidden Unicode characters found:\n")
        for f in findings:
            print("  " + f)
        print("\n%d occurrence(s). See CVE-2021-42574 (Trojan Source)." % len(findings))
        return 1

    print("Unicode scan OK -- no forbidden characters.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
