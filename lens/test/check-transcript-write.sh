#!/usr/bin/env bash
#
# check-transcript-write.sh -- gate G2.9, end to end.
#
# Usage:
#   check-transcript-write.sh <unify-lens-binary> <expected-text>
#
# The model test asserts that `M-x transcript.write-file` ASKS for the right
# thing; this asserts that the shipped binary, with a real engine attached,
# actually leaves the transcript on disk. The file cannot be a golden screen
# -- it is not on the screen -- and the path has to be a fresh temporary
# directory, so the key script is written here rather than checked in.
#
# Regenerate the expected text with UNIFY_UPDATE_GOLDEN=1, like every other
# golden in the tree.

set -u

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <unify-lens> <expected-text>" >&2
    exit 2
fi

# Absolute, because lens runs from inside the temporary directory below.
absolute() { case "$1" in /*|[A-Za-z]:*) printf '%s' "$1" ;; *) printf '%s/%s' "$PWD" "$1" ;; esac; }
LENS_BIN="$(absolute "$1")"
EXPECTED="$(absolute "$2")"

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

# The name lens is given is RELATIVE, and lens runs in $WORKDIR. The path is
# typed into the minibuffer, so no shell ever sees it: on Windows, an MSYS
# /tmp/... path would reach the native binary untranslated and name nothing,
# and the translated C:/Users/... form is long enough to crowd the 80-column
# status line this test reads. A space stays in it, because paths have them.
NAME="transcript out.txt"
OUT="$WORKDIR/$NAME"

# Quoted, so `$x` reaches the engine as the variable it is; the one line that
# needs the temporary path is appended after.
cat > "$WORKDIR/script.keys" <<'KEYS'
type colour( red );
Enter
type colour( $x )
Enter
type colour(
Enter
M-x
type transcript.write-file
Enter
C-u
KEYS
printf 'type %s\nEnter\n' "$NAME" >> "$WORKDIR/script.keys"

cd "$WORKDIR" || exit 1
"$LENS_BIN" --geometry 80x24 --layout run --session --no-welcome \
    --script "$WORKDIR/script.keys" > "$WORKDIR/screen" 2>"$WORKDIR/stderr"
RC=$?

if [ "$RC" -ne 0 ]; then
    echo "check-transcript-write.sh: unify-lens exited $RC" >&2
    cat "$WORKDIR/stderr" >&2
    exit 1
fi
if [ -s "$WORKDIR/stderr" ]; then
    echo "check-transcript-write.sh: lens wrote to stderr:" >&2
    sed 's/^/    /' "$WORKDIR/stderr" >&2
    exit 1
fi
if [ ! -f "$OUT" ]; then
    echo "check-transcript-write.sh: no file was written. Final screen:" >&2
    sed 's/^/    /' "$WORKDIR/screen" >&2
    exit 1
fi

# The status line says what happened, so a user is not left guessing.
if ! grep -q "wrote [0-9]* lines to $NAME" "$WORKDIR/screen"; then
    echo "check-transcript-write.sh: the status line did not report the write:" >&2
    sed 's/^/    /' "$WORKDIR/screen" >&2
    exit 1
fi

if [ "${UNIFY_UPDATE_GOLDEN:-0}" = "1" ]; then
    cp "$OUT" "$EXPECTED"
    echo "recorded $(basename "$EXPECTED")"
    exit 0
fi

if diff -u "$EXPECTED" "$OUT"; then
    exit 0
fi
echo "check-transcript-write.sh: the written transcript differs from $(basename "$EXPECTED")" >&2
exit 1
