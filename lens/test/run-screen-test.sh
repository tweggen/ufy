#!/usr/bin/env bash
#
# run-screen-test.sh -- golden-screen test runner for unify-lens.
#
# Usage:
#   run-screen-test.sh <unify-lens-binary> <geometry> <layout> <script> <expected>
#
# Runs unify-lens headlessly (`--script`, gate G1.7), captures the final
# character grid from stdout, and diffs it against <expected>.
#
# Deliberately the same conventions as the engine's test/run-golden-test.sh,
# so a CI log reads the same way whichever suite produced it:
#
#   0   the screen matches (or UNIFY_UPDATE_GOLDEN just recorded it)
#   77  no golden file yet -> CTest SKIP
#   1   the screen differs; a unified diff is printed
#   2   usage/setup error
#
# Regenerate every screen with:
#   UNIFY_UPDATE_GOLDEN=1 ctest --test-dir <build> -R lens-screen
#
# What these goldens are FOR, and what they are not for: rule 6 of the
# method (ACCEPTANCE.md) says to assert on the model and use grids for
# layout. So these check that a panel renders, that the four stock layouts
# are what they claim to be, and that 80x24 degrades rather than clips. They
# are NOT the oracle for behaviour -- that lives in the model tests, where a
# cosmetic change does not force a dozen re-recordings.

set -u

if [ "$#" -lt 5 ] || [ "$#" -gt 6 ]; then
    echo "usage: $0 <unify-lens> <geometry> <layout> <script> <expected> [session]" >&2
    exit 2
fi

LENS_BIN="$1"
GEOMETRY="$2"
LAYOUT="$3"
SCRIPT="$4"
EXPECTED="$5"
# A sixth argument of "session" starts a real engine. Off by default so a
# shell golden stays a test of the shell: a screen recorded with an engine
# attached moves whenever the engine's output moves.
SESSION_FLAG="--no-session"
WELCOME_FLAG="--no-welcome"
case "${6:-}" in
    session) SESSION_FLAG="--session" ;;
    welcome) WELCOME_FLAG="--welcome" ;;
esac

if [ ! -x "$LENS_BIN" ]; then
    echo "run-screen-test.sh: '$LENS_BIN' is not executable." >&2
    exit 2
fi
if [ ! -f "$SCRIPT" ]; then
    echo "run-screen-test.sh: script '$SCRIPT' does not exist." >&2
    exit 2
fi

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT
ACTUAL="$WORKDIR/actual"

"$LENS_BIN" --geometry "$GEOMETRY" --layout "$LAYOUT" \
    "$SESSION_FLAG" "$WELCOME_FLAG" \
    --script "$SCRIPT" > "$ACTUAL" 2>"$WORKDIR/stderr"
RC=$?

# Engine item E4's user-visible proof (G2.3): a program's output belongs in
# the transcript, so nothing may reach lens's own streams. The screen is
# exactly as many lines as the geometry has rows -- an extra line means
# something printed past the renderer.
EXPECTED_ROWS="${GEOMETRY#*x}"
ACTUAL_ROWS="$(wc -l < "$ACTUAL")"
if [ "$ACTUAL_ROWS" -ne "$EXPECTED_ROWS" ]; then
    echo "run-screen-test.sh: the screen is $ACTUAL_ROWS lines but the" >&2
    echo "  geometry has $EXPECTED_ROWS rows -- something printed to stdout" >&2
    echo "  outside the renderer (engine item E4 leaking?)." >&2
    exit 1
fi
if [ -s "$WORKDIR/stderr" ]; then
    echo "run-screen-test.sh: lens wrote to stderr:" >&2
    sed 's/^/    /' "$WORKDIR/stderr" >&2
    exit 1
fi

if [ "$RC" -ne 0 ]; then
    echo "run-screen-test.sh: unify-lens exited $RC" >&2
    cat "$WORKDIR/stderr" >&2
    exit 1
fi

if [ "${UNIFY_UPDATE_GOLDEN:-0}" = "1" ]; then
    mkdir -p "$(dirname "$EXPECTED")"
    cp "$ACTUAL" "$EXPECTED"
    echo "recorded $(basename "$EXPECTED")"
    exit 0
fi

if [ ! -f "$EXPECTED" ]; then
    echo "run-screen-test.sh: no golden file '$EXPECTED' yet; skipping." >&2
    exit 77
fi

if diff -u "$EXPECTED" "$ACTUAL" > "$WORKDIR/diff"; then
    exit 0
fi

echo "screen differs from $(basename "$EXPECTED"):" >&2
cat "$WORKDIR/diff" >&2
exit 1
