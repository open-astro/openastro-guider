#!/usr/bin/env bash
# Diff the frozen legacy twin against the src/scope.cpp lines it was copied
# from (#142). Prints nothing and exits 0 when the copy is exact.
#
#   check_twin.sh            compare with scope.cpp at the source commit
#   check_twin.sh HEAD       compare with scope.cpp at another revision
#   check_twin.sh --worktree compare with the scope.cpp in the working tree
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
rev="${1:-abe96d85}"
ranges='55,65p;838,844p;901,945p;979,1000p;1013,1641p'
if [ "$rev" = "--worktree" ]; then
    source_lines() { sed -n "$ranges" src/scope.cpp | tr -d '\r'; }
else
    source_lines() { git show "$rev:src/scope.cpp" | sed -n "$ranges" | tr -d '\r'; }
fi
diff <(source_lines) \
    <(sed -n '/^\/\/ BEGIN COPY$/,/^\/\/ END COPY$/p' tests/closed_loop/legacy/scope_calibration_legacy.cpp | sed '1d;$d')
