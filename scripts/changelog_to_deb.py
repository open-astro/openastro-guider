#!/usr/bin/env python3
"""Generate debian/changelog from the root CHANGELOG.md.

CHANGELOG.md (Keep a Changelog format) is the single source of truth for
release history; debian/changelog is a build artifact derived from it by
build-deb.sh and is not tracked in git.

Mapping:
  - Every "## [X.Y.Z] - YYYY-MM-DD" section becomes one Debian changelog
    stanza for X.Y.Z, dated noon local time on the release date.
  - Bullets keep their "### Added/Changed/..." category as a prefix and are
    re-wrapped to Debian's continuation-line layout. Markdown emphasis,
    inline code, and links are stripped. The "Commit References" subsection
    is omitted (git metadata, not release notes).
  - If --version names a release that has no dated section yet (still under
    "## [Unreleased]"), the top stanza is synthesized from the Unreleased
    content with distribution UNRELEASED and the current time.
  - Pre-1.0 date-only sections ("## [2026-01-23]") have no version number to
    build a stanza from and are omitted; that history stays in CHANGELOG.md.
"""

import argparse
import datetime
import email.utils
import re
import sys

HEADING_RE = re.compile(r"^## \[([^\]]+)\](?: - (\d{4}-\d{2}-\d{2}))?\s*$")
CATEGORY_RE = re.compile(r"^### (.+?)\s*$")
VERSION_RE = re.compile(r"^\d+\.\d+\.\d+[A-Za-z0-9.~+-]*$")
SKIP_CATEGORIES = {"commit references"}

# Order matters: links first (their text may contain emphasis), then
# emphasis/code markers, innermost first.
MARKDOWN_SUBS = [
    (re.compile(r"!?\[([^\]]*)\]\([^)]*\)"), r"\1"),  # [text](url) -> text
    (re.compile(r"\*\*([^*]+)\*\*"), r"\1"),  # **bold**
    (re.compile(r"\*([^*\s][^*]*)\*"), r"\1"),  # *emphasis*
    (re.compile(r"`([^`]*)`"), r"\1"),  # `code`
]


def strip_markdown(text):
    for pattern, repl in MARKDOWN_SUBS:
        text = pattern.sub(repl, text)
    return re.sub(r"\s+", " ", text).strip()


def parse_changelog(path):
    """Return a list of sections: {label, date, bullets: [(category, text)]}."""
    sections = []
    current = None
    category = None
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            m = HEADING_RE.match(line)
            if m:
                current = {"label": m.group(1), "date": m.group(2), "bullets": []}
                sections.append(current)
                category = None
                continue
            if current is None:
                continue
            m = CATEGORY_RE.match(line)
            if m:
                category = m.group(1)
                continue
            if category and category.strip().lower() in SKIP_CATEGORIES:
                continue
            if line.startswith("- "):
                current["bullets"].append([category, line[2:].strip()])
            elif line.strip() and current["bullets"]:
                # Continuation line or nested bullet: fold into the open bullet.
                current["bullets"][-1][1] += " " + line.strip().lstrip("- ")
    return sections


def wrap_bullet(category, text, width=78):
    text = strip_markdown(text)
    if category:
        text = "%s: %s" % (category, text)
    words = text.split()
    lines = []
    line = "  *"
    for word in words:
        if len(line) + 1 + len(word) > width and line not in ("  *", "   "):
            lines.append(line)
            line = "   "
        line += " " + word
    lines.append(line)
    return lines


def emit_stanza(out, package, version, distribution, bullets, date, maintainer):
    out.write("%s (%s) %s; urgency=low\n\n" % (package, version, distribution))
    if bullets:
        for category, text in bullets:
            out.write("\n".join(wrap_bullet(category, text)) + "\n")
    else:
        out.write("  * See CHANGELOG.md for release notes.\n")
    out.write("\n -- %s  %s\n" % (maintainer, email.utils.format_datetime(date)))


def section_date(section):
    d = datetime.date.fromisoformat(section["date"])
    return datetime.datetime(d.year, d.month, d.day, 12, 0, 0).astimezone()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--changelog", required=True, help="path to CHANGELOG.md")
    ap.add_argument("--out", required=True, help="path to debian/changelog to write")
    ap.add_argument("--package", required=True, help="Debian source package name")
    ap.add_argument("--version", required=True, help="version being built (from version.md)")
    ap.add_argument("--maintainer", required=True, help='"Name <email>"')
    args = ap.parse_args()

    sections = parse_changelog(args.changelog)
    released = [s for s in sections if VERSION_RE.match(s["label"]) and s["date"]]
    unreleased = next((s for s in sections if s["label"].lower() == "unreleased"), None)

    stanzas = []
    if any(s["label"] == args.version for s in released):
        if unreleased and unreleased["bullets"]:
            print(
                "warning: version %s is already released in CHANGELOG.md but "
                "[Unreleased] is not empty; the unreleased work will be in the "
                "binary but not in its changelog. Run /release to cut a new "
                "version, or ignore if intended." % args.version,
                file=sys.stderr,
            )
    else:
        bullets = unreleased["bullets"] if unreleased else []
        stanzas.append((args.version, "UNRELEASED", bullets, datetime.datetime.now().astimezone()))

    for s in released:
        stanzas.append((s["label"], "stable", s["bullets"], section_date(s)))

    if not stanzas:
        sys.exit("error: no versioned sections found in %s" % args.changelog)

    with open(args.out, "w", encoding="utf-8") as out:
        for i, (version, dist, bullets, date) in enumerate(stanzas):
            if i:
                out.write("\n")
            emit_stanza(out, args.package, version, dist, bullets, date, args.maintainer)


if __name__ == "__main__":
    main()
