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

LENS_BIN="$1"
EXPECTED="$2"

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT
OUT="$WORKDIR/transcript out.txt"   # a space, because paths have them

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
printf 'type %s\nEnter\n' "$OUT" >> "$WORKDIR/script.keys"

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
if ! grep -q "wrote [0-9]* lines to $OUT" "$WORKDIR/screen"; then
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
