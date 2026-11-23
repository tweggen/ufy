#!/usr/bin/env bash
#
# check-resize-roundtrip.sh -- gate G1.3, on the screen.
#
# The model test already asserts tree equality across a resize. This asserts
# the consequence a user would actually notice: shrinking to 80x24 and
# growing back leaves EXACTLY the screen you would have had without ever
# resizing -- not merely a valid one.
#
# It is a separate script rather than a golden because the claim is an
# equivalence between two runs. A golden would pin what the screen looks
# like; this pins that the two are the same, which stays true when the
# screen's appearance changes.

set -u

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <unify-lens> <screens-dir>" >&2
    exit 2
fi

LENS_BIN="$1"
SCREENS="$2"

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

# The same tiling gestures, once with a resize round trip and once without.
printf 'C-x 3\nC-x 2\n' > "$WORKDIR/direct.keys"

"$LENS_BIN" --geometry 120x40 --layout browse \
    --script "$WORKDIR/direct.keys" > "$WORKDIR/direct" || exit 1
"$LENS_BIN" --geometry 120x40 --layout browse \
    --script "$SCREENS/resize-roundtrip.keys" > "$WORKDIR/roundtrip" || exit 1

if diff -u "$WORKDIR/direct" "$WORKDIR/roundtrip" > "$WORKDIR/diff"; then
    echo "G1.3: a resize round trip restores the screen exactly."
    exit 0
fi

echo "G1.3: shrinking to 80x24 and back did NOT restore the layout." >&2
echo "  The solver is mutating the tree instead of degrading a solution." >&2
cat "$WORKDIR/diff" >&2
exit 1
