#!/usr/bin/env bash
# Ratchet on phd2.bin's wxWidgets GUI dependencies (#115, slice P0).
#
# Steps 4-8 of #115 remove the daemon's use of the wx GUI libraries (core, aui,
# and any other non-base wx lib) until phd2.bin links only wxBase + wxNet. This
# check stops new GUI dependencies from landing while that happens: it lists the
# symbols phd2.bin imports from the wx GUI libraries it loads and compares them
# with the committed allowlist, scripts/wx_gui_symbols.allowlist.
#
#   - a symbol that is not in the allowlist fails the check (a new GUI call);
#   - a symbol in the allowlist that the binary no longer imports is only a
#     warning, with a hint to shrink the list (run with --update and commit).
#
# The comparison is made on demangled names with the ELF symbol version
# (@WXU_3.2, @@WXU_3.2.1, ...) stripped, so the list does not depend on the
# wx 3.2.x patch release or the distro: CI builds on Ubuntu 24.04 (wx 3.2.4),
# RC51 and the dev hosts on Debian 13 (wx 3.2.8). A symbol that a wxBase
# library also defines (weak template instantiations and the like) is not
# counted as a GUI symbol.
#
# Usage: scripts/check_wx_gui_symbols.sh [--update] [--allowlist FILE] PHD2_BIN
#
# Needs binutils (nm, readelf) and ldd. PHD2_BIN must be built for the host,
# since ldd resolves the wx libraries it loads.

set -euo pipefail
export LC_ALL=C

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
allowlist="$script_dir/wx_gui_symbols.allowlist"
update=0
bin=""

usage() {
    sed -n 's/^# \{0,1\}Usage: /Usage: /p' "${BASH_SOURCE[0]}" >&2
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
        --update) update=1 ;;
        --allowlist)
            [ $# -ge 2 ] || usage
            allowlist="$2"
            shift
            ;;
        -h | --help) usage ;;
        -*)
            echo "unknown option: $1" >&2
            usage
            ;;
        *)
            [ -z "$bin" ] || usage
            bin="$1"
            ;;
    esac
    shift
done
[ -n "$bin" ] || usage
if [ ! -f "$bin" ]; then
    echo "error: $bin not found" >&2
    exit 2
fi

in_ci() { [ "${GITHUB_ACTIONS:-}" = "true" ]; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# Demangled symbol names, one per line, without the ELF version suffix.
strip_versions() { sed -E 's/@{1,2}[A-Za-z0-9_.]+$//'; }

# The wx shared libraries phd2.bin loads (its NEEDED entries and what they pull
# in), as "soname path" pairs.
ldd "$bin" | awk '$2 == "=>" && $1 ~ /^libwx_/ { print $1, $3 }' | sort -u > "$tmp/wxlibs"
if [ ! -s "$tmp/wxlibs" ]; then
    echo "error: $bin loads no wxWidgets library (wrong file, or ldd could not resolve it)" >&2
    exit 2
fi
if awk '$2 !~ /^\// { bad = 1 } END { exit !bad }' "$tmp/wxlibs"; then
    echo "error: ldd could not resolve every wx library of $bin:" >&2
    cat "$tmp/wxlibs" >&2
    exit 2
fi

# wxBase and its non-GUI companions (net, xml) are the libraries the daemon is
# meant to keep; every other wx library is a GUI one.
gui_libs=()
: > "$tmp/base_defined"
: > "$tmp/gui_defined"
while read -r soname path; do
    if [[ "$soname" =~ ^libwx_baseu(_[a-z]+)?- ]]; then
        nm -D --defined-only -j -C "$path" | strip_versions >> "$tmp/base_defined"
    else
        gui_libs+=("$soname")
        nm -D --defined-only -j -C "$path" | strip_versions >> "$tmp/gui_defined"
    fi
done < "$tmp/wxlibs"
sort -u -o "$tmp/base_defined" "$tmp/base_defined"
sort -u -o "$tmp/gui_defined" "$tmp/gui_defined"
comm -23 "$tmp/gui_defined" "$tmp/base_defined" > "$tmp/gui_only"

nm -D --undefined-only -j -C "$bin" | strip_versions | sort -u > "$tmp/undefined"
comm -12 "$tmp/undefined" "$tmp/gui_only" > "$tmp/current"

if [ "${#gui_libs[@]}" -eq 0 ]; then
    echo "wx GUI libraries: none (phd2.bin links wxBase only)"
else
    echo "wx GUI libraries: ${gui_libs[*]}"
fi
echo "GUI symbols imported by $bin: $(wc -l < "$tmp/current")"

if [ "$update" -eq 1 ]; then
    {
        echo "# wx GUI symbols phd2.bin may import (#115, slice P0)."
        echo "# Checked by scripts/check_wx_gui_symbols.sh; regenerate with"
        echo "#   scripts/check_wx_gui_symbols.sh --update build/phd2.bin"
        echo "# Demangled, without ELF symbol versions, sorted with LC_ALL=C."
        echo "# This list may only shrink: steps 4-8 of #115 remove these uses."
        cat "$tmp/current"
    } > "$allowlist"
    echo "wrote $allowlist ($(wc -l < "$tmp/current") symbols)"
    exit 0
fi

if [ ! -f "$allowlist" ]; then
    echo "error: allowlist $allowlist not found (create it with --update)" >&2
    exit 2
fi
{ grep -v -e '^#' -e '^[[:space:]]*$' "$allowlist" || true; } | sort -u > "$tmp/allowed"

comm -23 "$tmp/current" "$tmp/allowed" > "$tmp/new"
comm -13 "$tmp/current" "$tmp/allowed" > "$tmp/gone"

if [ -s "$tmp/gone" ]; then
    n="$(wc -l < "$tmp/gone")"
    msg="$n allowlisted wx GUI symbol(s) are no longer imported; shrink the allowlist: scripts/check_wx_gui_symbols.sh --update <phd2.bin>, then commit $(basename "$allowlist")"
    if in_ci; then echo "::warning::$msg"; else echo "warning: $msg"; fi
    sed 's/^/  - /' "$tmp/gone"
fi

if [ -s "$tmp/new" ]; then
    n="$(wc -l < "$tmp/new")"
    msg="$n new wx GUI symbol(s) imported by phd2.bin that are not in $(basename "$allowlist"). #115 is removing the daemon's GUI dependencies; use a wxBase or wx-free equivalent instead of adding to the list"
    if in_ci; then echo "::error::$msg"; else echo "error: $msg"; fi
    sed 's/^/  + /' "$tmp/new"
    exit 1
fi

echo "OK: no new wx GUI symbols"
