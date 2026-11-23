#!/usr/bin/env bash
# Diff the frozen legacy twin against the src/guider_multistar.cpp lines it was
# copied from (#142). Prints nothing and exits 0 when the copy is exact.
#
#   check_twin.sh            compare with guider_multistar.cpp at the source commit
#   check_twin.sh HEAD       compare with guider_multistar.cpp at another revision
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
rev="${1:-7755af25}"
ranges='53,181p;605,708p;717,925p'
diff <(git show "$rev:src/guider_multistar.cpp" | sed -n "$ranges" | tr -d '\r') \
    <(sed -n '/^\/\/ BEGIN COPY$/,/^\/\/ END COPY$/p' tests/guiding/legacy/multistar_legacy.cpp | sed '1d;$d')
