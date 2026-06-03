#!/usr/bin/env python3
"""Scan tracked text files for dangerous/invisible Unicode characters.

Guards against "Trojan Source" attacks (CVE-2021-42574), where bidirectional
control characters or invisible code points make source render differently from
how the compiler reads it.

Run from the repo root:  python3 .github/scripts/check-unicode.py
Exits non-zero (and prints file:line:col U+XXXX) if any forbidden code point is
found. Intentional control characters in tests should be written as escapes
(e.g. "\\u200E") rather than literal characters so the source stays clean.

Detection deliberately does NOT depend on a file decoding cleanly as UTF-8.
An earlier version skipped any file that raised UnicodeDecodeError, which let
an attacker defeat this security control simply by not using clean UTF-8 (a
bidi payload in a UTF-16 / invalid-UTF-8 file slipped through silently). This
version instead:
  * skips only genuine binaries (NUL byte, and not a UTF-16/UTF-32 BOM);
  * scans UTF-8 and BOM-declared UTF-16 text by decoding for precise line/col;
  * for any remaining undecodable *text* file, flags the file itself (a
    non-UTF-8 text file is anomalous in a UTF-8 tree) AND byte-scans for the
    forbidden code points in every encoding they could realistically reach a
    compiler through, so nothing is silently passed.
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

# Encodings a hidden character could realistically reach a toolchain through.
# Every forbidden code point is > U+00FF, so single-byte legacy encodings
# (Latin-1, Windows-1252, ...) physically cannot represent them — UTF-8 and
# UTF-16 (both endiannesses) are the byte-level vectors that matter for a source
# tree. UTF-32 is not used in practice here and is omitted.
BYTE_ENCODINGS = ("utf-8", "utf-16-le", "utf-16-be")

# Forbidden byte sequences for the raw-byte backstop: {bytes: (cp, encoding)}.
FORBIDDEN_BYTES = {
    chr(cp).encode(enc): (cp, enc)
    for cp in FORBIDDEN
    for enc in BYTE_ENCODINGS
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

# Byte-order marks that mark a NUL-containing file as UTF-16/UTF-32 *text*
# rather than binary (their ASCII content is full of NUL bytes).
_UTF_BOMS = (
    b"\xff\xfe\x00\x00",  # UTF-32-LE
    b"\x00\x00\xfe\xff",  # UTF-32-BE
    b"\xff\xfe",          # UTF-16-LE
    b"\xfe\xff",          # UTF-16-BE
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


def looks_binary(data):
    """A NUL byte means binary -- unless a UTF-16/UTF-32 BOM marks it as text."""
    if not data:
        return False
    if any(data.startswith(bom) for bom in _UTF_BOMS):
        return False
    return b"\x00" in data


def scan_text(path, text, findings):
    """Report forbidden code points in decoded text with 1-based line:col."""
    line = 1
    col = 0
    for ch in text:
        if ch == "\n":
            line += 1
            col = 0
            continue
        col += 1
        name = FORBIDDEN.get(ord(ch))
        if name is not None:
            findings.append("%s:%d:%d: U+%04X %s" % (path, line, col, ord(ch), name))


def main():
    findings = []
    for path in tracked_files():
        try:
            with open(path, "rb") as fh:
                data = fh.read()
        except OSError:
            continue

        if looks_binary(data):
            continue

        # UTF-8 is the tree's canonical encoding; decode for precise locations.
        try:
            scan_text(path, data.decode("utf-8"), findings)
            continue
        except UnicodeDecodeError:
            pass

        # Not valid UTF-8 but not binary. Honour a declared UTF-16 BOM.
        if any(data.startswith(bom) for bom in (b"\xff\xfe", b"\xfe\xff")):
            try:
                scan_text(path, data.decode("utf-16"), findings)
                continue
            except UnicodeDecodeError:
                pass

        # Undecodable text: do NOT silently skip (that was the old bypass).
        # Flag the anomaly and byte-scan for forbidden sequences as a backstop.
        findings.append("%s: non-UTF-8 text file (cannot verify cleanly; treat as suspect)" % path)
        for seq, (cp, enc) in FORBIDDEN_BYTES.items():
            if seq in data:
                findings.append("%s: contains %s-encoded U+%04X %s" % (path, enc, cp, FORBIDDEN[cp]))

    if findings:
        print("Forbidden Unicode characters found:\n")
        for f in findings:
            print("  " + f)
        print("\n%d finding(s). See CVE-2021-42574 (Trojan Source)." % len(findings))
        return 1

    print("Unicode scan OK -- no forbidden characters.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
