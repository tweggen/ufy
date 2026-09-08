#!/usr/bin/env bash
#
# check-spec-runner.sh -- does `--spec` still fail when it should?
#
# The spec runner's whole value is that a red case is red. A runner that
# quietly passes everything is worse than no runner at all: it looks like
# coverage. So each fixture in test/spec/bad/ is a way the suite could be
# wrong, and this asserts that each one is caught, with the right exit code
# and a message that names the problem.
#
#   exit 1  a case failed        -- the suite is red
#   exit 2  the spec cannot run  -- unreadable, unparsable, or states nothing
#
# Usage: check-spec-runner.sh <unify-lens-binary> <spec-bad-dir>

set -u

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <unify-lens> <spec-bad-dir>" >&2
    exit 2
fi

LENS_BIN="$1"
BAD_DIR="$2"
STATUS=0

# fixture | expected exit | a phrase the output must contain
CASES="
false-expectation|1|cursor is 0, not 99
unknown-expectation|1|no such quantity
orphan-fact|1|there is no case
no-cases|2|states no case
parse-error|2|parse error
bad-keys|1|is not a key sequence
"

echo "$CASES" | while IFS='|' read -r name expected needle; do
    [ -n "$name" ] || continue

    out="$( "$LENS_BIN" --spec "$BAD_DIR/${name}.ufy" 2>&1 )"
    rc=$?

    if [ "$rc" -ne "$expected" ]; then
        echo "spec runner: ${name}.ufy exited $rc, expected $expected" >&2
        echo "$out" | sed 's/^/    /' >&2
        exit 1
    fi
    case "$out" in
        *"$needle"*) ;;
        *)
            echo "spec runner: ${name}.ufy did not mention '$needle'" >&2
            echo "$out" | sed 's/^/    /' >&2
            exit 1
            ;;
    esac
done || STATUS=1

if [ "$STATUS" -eq 0 ]; then
    echo "spec runner: every way of being wrong is still caught."
fi
exit $STATUS
