#!/usr/bin/env bash
#
# run-golden-test.sh -- golden-output test runner for Unify (.ufy) programs.
#
# Usage:
#   run-golden-test.sh <unify-run-binary> <program.ufy> <expected-file>
#
# Runs <unify-run-binary> on <program.ufy>, captures stdout (this is where
# unify-run's `print`/`emit` builtins write their output -- see
# tools/unify-run.cpp), and diffs it against <expected-file>.
#
# Exit codes:
#   0   output matches the golden file (or UNIFY_UPDATE_GOLDEN just wrote it)
#   77  no golden file exists yet for this program (CTest "skipped" code,
#       wired up via SKIP_RETURN_CODE in test/CMakeLists.txt)
#   1   output differs from the golden file (a unified diff is printed)
#   2   usage/setup error (bad args, binary not found, program not found, ...)
#
# Special mode: if the environment variable UNIFY_UPDATE_GOLDEN is set to
# "1", the actual (normalized) output is written to <expected-file>
# (creating its parent directory if needed) instead of being diffed, and the
# script exits 0. This is how golden files are (re)generated -- typically
# via `UNIFY_UPDATE_GOLDEN=1 ctest --output-on-failure`, run once on a
# machine that can build/run unify-run (see test/golden/README.md).
#
# Optional environment variable UNIFY_EXPECT_EXIT=<code> additionally
# requires <unify-run-binary> to exit with exactly <code> (in addition to
# the usual stdout diff against <expected-file>); a mismatch is reported and
# the script exits 1. This is used for negative tests (e.g. deliberately
# malformed .ufy programs expected to fail parsing with exit code 1) -- see
# test/CMakeLists.txt's unify-golden-parse-error test. When unset (the
# default, used by all other golden tests), the binary's exit code is
# ignored, as before.

set -u

if [ "$#" -ne 3 ]; then
    echo "usage: $0 <unify-run-binary> <program.ufy> <expected-file>" >&2
    exit 2
fi

UNIFY_RUN_BIN="$1"
PROGRAM_UFY="$2"
EXPECTED_FILE="$3"

if [ ! -x "$UNIFY_RUN_BIN" ]; then
    echo "run-golden-test.sh: '$UNIFY_RUN_BIN' is not an executable file." >&2
    exit 2
fi

if [ ! -f "$PROGRAM_UFY" ]; then
    echo "run-golden-test.sh: program '$PROGRAM_UFY' does not exist." >&2
    exit 2
fi

WORKDIR="$(mktemp -d)"
cleanup() {
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

ACTUAL_RAW="$WORKDIR/actual.raw"
ACTUAL_NORM="$WORKDIR/actual.normalized"
EXPECTED_NORM="$WORKDIR/expected.normalized"
DIFF_OUT="$WORKDIR/diff.out"

# Run the program. unify-run may exit non-zero on a detected parse error
# (see tools/unify-run.cpp); that alone is not a harness failure -- some
# sample programs (e.g. pathfinder.ufy, which calls home-automation driver
# builtins that may not exist in the core lib) are only expected to parse,
# not necessarily to solve cleanly. We still capture and diff whatever
# stdout was produced, so a golden file can pin down that behaviour too.
# (Unless UNIFY_EXPECT_EXIT is set -- see above -- the exit code itself is
# not checked here.)
"$UNIFY_RUN_BIN" "$PROGRAM_UFY" > "$ACTUAL_RAW"
ACTUAL_EXIT="$?"

if [ -n "${UNIFY_EXPECT_EXIT:-}" ] && [ "$ACTUAL_EXIT" -ne "$UNIFY_EXPECT_EXIT" ]; then
    echo "run-golden-test.sh: '$PROGRAM_UFY' exited $ACTUAL_EXIT, expected $UNIFY_EXPECT_EXIT." >&2
    exit 1
fi

# An exit code >= 128 means the binary died from a signal (segfault, abort,
# sanitizer error, ...). That must always fail the test, even though plain
# nonzero exits are tolerated: a crash AFTER stdout is complete (e.g. a
# use-after-free during teardown, aborted by ASan) would otherwise pass the
# stdout diff and go unnoticed.
if [ "$ACTUAL_EXIT" -ge 128 ]; then
    echo "run-golden-test.sh: '$PROGRAM_UFY' died from a signal (exit $ACTUAL_EXIT)." >&2
    exit 1
fi

# Normalize trivially volatile content: strip carriage returns and trailing
# whitespace on each line, and any trailing blank lines, so incidental
# formatting differences don't cause false-positive diffs.
#
# The CR strip is what makes this runnable on Windows: the C runtime turns
# every \n into \r\n on a text-mode stream, so a golden file recorded on
# Unix would otherwise differ on every single line, with a diff showing two
# lines that look identical. The engine never emits a lone CR of its own.
normalize() {
    tr -d '\r' < "$1" \
        | sed -e 's/[ \t]*$//' \
        | sed -e :a -e '/^\n*$/{$d;N;ba' -e '}'
}

normalize "$ACTUAL_RAW" > "$ACTUAL_NORM"

if [ "${UNIFY_UPDATE_GOLDEN:-}" = "1" ]; then
    mkdir -p "$(dirname "$EXPECTED_FILE")"
    cp "$ACTUAL_NORM" "$EXPECTED_FILE"
    echo "run-golden-test.sh: wrote golden file '$EXPECTED_FILE'."
    exit 0
fi

if [ ! -f "$EXPECTED_FILE" ]; then
    echo "SKIP (no golden file yet for '$PROGRAM_UFY' -- run with UNIFY_UPDATE_GOLDEN=1 to create '$EXPECTED_FILE')"
    exit 77
fi

normalize "$EXPECTED_FILE" > "$EXPECTED_NORM"

if diff -u "$EXPECTED_NORM" "$ACTUAL_NORM" > "$DIFF_OUT" 2>&1; then
    exit 0
else
    echo "run-golden-test.sh: output for '$PROGRAM_UFY' does not match '$EXPECTED_FILE':"
    sed -e "s|$EXPECTED_NORM|expected|" -e "s|$ACTUAL_NORM|actual|" "$DIFF_OUT"
    exit 1
fi
