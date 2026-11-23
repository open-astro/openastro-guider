#!/usr/bin/env bash
# Diff the frozen legacy twins against the source lines they were copied from (#142):
# multistar_legacy.cpp against src/guider_multistar.cpp at 7755af25, and guider_sm_legacy.cpp
# against src/guider.cpp at 579829c3 (slice T2c). Prints nothing and exits 0 when the copies are
# exact.
#
#   check_twin.sh            compare with the sources at their source commits
#   check_twin.sh HEAD       compare with the sources at another revision
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
status=0

# check SOURCE REV RANGES TWIN: the ranges of SOURCE at REV, CR stripped, against the
# BEGIN COPY / END COPY blocks of TWIN, in order
check() {
    diff <(git show "$2:$1" | sed -n "$3" | tr -d '\r') \
        <(sed -n '/^\/\/ BEGIN COPY$/,/^\/\/ END COPY$/p' "$4" | grep -v -e '^// BEGIN COPY$' -e '^// END COPY$') ||
        status=1
}

check src/guider_multistar.cpp "${1:-7755af25}" '53,181p;605,708p;717,925p' tests/guiding/legacy/multistar_legacy.cpp
check src/guider.cpp "${1:-579829c3}" '136,159p;816,913p;954,1104p;1130,1162p;1325,1368p;1388,1394p;1399,1524p' \
    tests/guiding/legacy/guider_sm_legacy.cpp
exit $status
