#!/usr/bin/env bash
# Diff the frozen legacy twins against the src/guide_algorithm_*.cpp lines they were copied
# from (#142). Prints nothing and exits 0 when both copies are exact.
#
#   check_twin.sh            compare with the sources at the source commit
#   check_twin.sh HEAD       compare with the sources at another revision
#   check_twin.sh --worktree compare with the sources in the working tree
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
rev="${1:-4eac9d9c}"
source_lines() {
    if [ "$rev" = "--worktree" ]; then
        sed -n "$2" "src/$1" | tr -d '\r'
    else
        git show "$rev:src/$1" | sed -n "$2" | tr -d '\r'
    fi
}
copy_lines() {
    sed -n '/^\/\/ BEGIN COPY$/,/^\/\/ END COPY$/p' "tests/algorithms/legacy/$1" | sed '1d;$d'
}
status=0
diff <(source_lines guide_algorithm_hysteresis.cpp '42,46p;70,167p') <(copy_lines hysteresis_legacy.cpp) || status=1
diff <(source_lines guide_algorithm_resistswitch.cpp '42,43p;65,240p') <(copy_lines resistswitch_legacy.cpp) || status=1
exit $status
