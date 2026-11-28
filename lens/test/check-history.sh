#!/usr/bin/env bash
#
# check-history.sh -- gate G2.8, the half that needs no terminal.
#
# Usage:
#   check-history.sh <unify-lens-binary>
#
# Three claims about the shipped binary and a history file on disk:
#
#   1. a history file is READ: its newest line is what Up recalls;
#   2. a history file is WRITTEN: what was typed is appended, in readline's
#      format, after what was already there;
#   3. a --script run that names no history file leaves $HOME alone. A test
#      that wrote the user's real ~/.unify_history would be a test that
#      edits the user's data, and a golden that read it would depend on it.
#
# The other half -- that the file unify-run -i writes is that file, and
# vice versa -- needs readline driving a real terminal, so it lives in
# pty-interaction-test.cpp.

set -u

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <unify-lens>" >&2
    exit 2
fi
LENS_BIN="$1"

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT
FAIL=0

run_lens() {
    # $1 = key script; the rest are extra options
    local script="$1"; shift
    HOME="$WORKDIR/home" "$LENS_BIN" --geometry 80x24 --layout run \
        --no-session --no-welcome "$@" --script "$script" \
        > "$WORKDIR/screen" 2> "$WORKDIR/stderr"
}

mkdir -p "$WORKDIR/home"

# -- 1. read ------------------------------------------------------------------
printf '%s\n' 'older( 1 );' '? newest( $x );' > "$WORKDIR/history"
printf 'Up\n' > "$WORKDIR/up.keys"
run_lens "$WORKDIR/up.keys" --history "$WORKDIR/history"
if ! grep -q '?- ? newest( \$x );' "$WORKDIR/screen"; then
    echo "check-history.sh: Up did not recall the history file's newest line." >&2
    sed 's/^/    /' "$WORKDIR/screen" "$WORKDIR/stderr" >&2
    FAIL=1
fi

# -- 2. write -----------------------------------------------------------------
printf 'type typed( 2 );\nEnter\n' > "$WORKDIR/type.keys"
run_lens "$WORKDIR/type.keys" --history "$WORKDIR/history"
printf '%s\n' 'older( 1 );' '? newest( $x );' 'typed( 2 );' > "$WORKDIR/expected"
if ! diff -u "$WORKDIR/expected" "$WORKDIR/history" >&2; then
    echo "check-history.sh: the history file was not appended to as expected." >&2
    FAIL=1
fi
if ls "$WORKDIR"/*.lens-tmp >/dev/null 2>&1; then
    echo "check-history.sh: a temporary file was left behind." >&2
    FAIL=1
fi

# -- 3. --script without --history leaves $HOME alone ------------------------
run_lens "$WORKDIR/type.keys"
if [ -e "$WORKDIR/home/.unify_history" ]; then
    echo "check-history.sh: --script wrote \$HOME/.unify_history unasked." >&2
    FAIL=1
fi

# and --no-history means neither read nor write, even when one is named
cp "$WORKDIR/history" "$WORKDIR/before"
run_lens "$WORKDIR/type.keys" --history "$WORKDIR/history" --no-history
if ! cmp -s "$WORKDIR/before" "$WORKDIR/history"; then
    echo "check-history.sh: --no-history still wrote the history file." >&2
    FAIL=1
fi

exit "$FAIL"
