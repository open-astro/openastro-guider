#!/usr/bin/env bash
# Fail if phd2.bin links a GUI library (#115, slice S8.3).
#
# Since #115 step 8 the daemon is a wxAppConsole linking wxBase and wxNet only.
# This check fails if `readelf -d` lists a NEEDED entry for a wx GUI library
# (wx_gtk*, wx_*core*) or for GTK, GDK or X11. It replaces the P0 symbol
# ratchet (check_wx_gui_symbols.sh and its allowlist): with no wx GUI library
# on the link line and wxUSE_GUI=0, a GUI call is already a compile or link
# error, so what is left to catch is a GUI library coming back onto the link.
#
# Usage: scripts/check_no_gui_libs.sh PHD2_BIN
#
# Needs binutils (readelf).

set -euo pipefail
export LC_ALL=C

if [ $# -ne 1 ]; then
    sed -n 's/^# \{0,1\}Usage: /Usage: /p' "${BASH_SOURCE[0]}" >&2
    exit 2
fi
bin="$1"
if [ ! -f "$bin" ]; then
    echo "error: $bin not found" >&2
    exit 2
fi

needed="$(readelf -d "$bin" | sed -n 's/.*(NEEDED).*\[\(.*\)\]$/\1/p')"
if [ -z "$needed" ]; then
    echo "error: readelf lists no NEEDED entry in $bin (not a dynamically linked ELF file?)" >&2
    exit 2
fi
echo "NEEDED by $bin:"
# shellcheck disable=SC2001
echo "$needed" | sed 's/^/  /'

gui="$(echo "$needed" | grep -E 'wx_gtk|gtk|gdk|X11|wx_.*core' || true)"
if [ -n "$gui" ]; then
    msg="phd2.bin links GUI libraries: $(echo "$gui" | tr '\n' ' ')- the daemon must link wxBase and wxNet only (#115)"
    if [ "${GITHUB_ACTIONS:-}" = "true" ]; then echo "::error::$msg"; else echo "error: $msg"; fi
    exit 1
fi
echo "OK: no wx GUI, GTK, GDK or X11 library"
