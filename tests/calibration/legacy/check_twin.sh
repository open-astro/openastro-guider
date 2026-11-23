#!/usr/bin/env bash
# Diff the frozen calstep twin against the src/calstep_dialog.cpp lines it was
# copied from (#142). Prints nothing and exits 0 when the copy is exact.
#
#   check_twin.sh            compare with calstep_dialog.cpp at the source commit
#   check_twin.sh HEAD       compare with calstep_dialog.cpp at another revision
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
rev="${1:-7755af25}"
diff <(git show "$rev:src/calstep_dialog.cpp" | sed -n '40,77p' | tr -d '\r') \
    <(sed -n '/^\/\/ BEGIN COPY$/,/^\/\/ END COPY$/p' tests/calibration/legacy/calstep_legacy.cpp | sed '1d;$d')
