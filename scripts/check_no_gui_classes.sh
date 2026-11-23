#!/usr/bin/env bash
# Fail if code under src/ (or the directory given) names a wx GUI class again
# (#115 step 9). The daemon is a wxAppConsole built with wxUSE_GUI=0, so such
# code would not compile anyway; this catches it in the cheap lint job, before a
# build, and with the file and line.
#
# Matches the words wxFrame, wxDialog, wxWindow, wxPanel, wxImage and wxBitmap.
# Comments are ignored, so history notes can still name the classes: a line
# whose text starts with //, /* or * (a block comment's body), and the part of a
# line after // or inside /* ... */ on that line. A /* comment spanning lines
# whose body lines do not start with * is not recognised; start them with *.
set -euo pipefail

pattern='\b(wxFrame|wxDialog|wxWindow|wxPanel|wxImage|wxBitmap)\b'
dir="${1:-src}"

if [ ! -d "$dir" ]; then
    echo "check_no_gui_classes: no directory $dir" >&2
    exit 2
fi

found=0
while IFS= read -r hit; do
    # hit is "file:line:text"
    text="${hit#*:*:}"
    code="$(printf '%s\n' "$text" | sed -E 's#/\*.*\*/##g; s#//.*$##; s#^[[:space:]]+##')"
    case "$code" in
        '/*'* | '*'*) continue ;;
    esac
    if printf '%s\n' "$code" | grep -qE "$pattern"; then
        echo "$hit"
        found=1
    fi
done < <(grep -rnE "$pattern" "$dir" || true)

if [ "$found" -ne 0 ]; then
    echo "check_no_gui_classes: the lines above use a wx GUI class; the guider has no GUI (#115)" >&2
    exit 1
fi
echo "check_no_gui_classes: no wx GUI class in $dir"
