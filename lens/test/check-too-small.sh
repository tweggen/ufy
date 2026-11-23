#!/usr/bin/env bash
#
# check-too-small.sh -- gate G1.4.
#
# "Below 80x24, lens exits with the pinned message and a non-zero status; it
# does not render."
#
# All three halves are asserted, because each fails differently: a zero exit
# status makes a script think it worked, a missing message leaves the user
# with no idea why nothing happened, and rendering anyway produces the
# illegible screen the rule exists to prevent.

set -u

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <unify-lens> <empty-script>" >&2
    exit 2
fi

LENS_BIN="$1"
SCRIPT="$2"
STATUS=0

check() {
    local geometry="$1"
    local out err rc
    WORKDIR="$(mktemp -d)"
    "$LENS_BIN" --geometry "$geometry" --layout browse --script "$SCRIPT" \
        > "$WORKDIR/out" 2> "$WORKDIR/err"
    rc=$?

    if [ "$rc" -eq 0 ]; then
        echo "G1.4: at $geometry lens exited 0; it must refuse." >&2
        STATUS=1
    fi
    if [ -s "$WORKDIR/out" ]; then
        echo "G1.4: at $geometry lens rendered $(wc -l < "$WORKDIR/out") lines; it must not render." >&2
        STATUS=1
    fi
    if ! grep -q "lens needs at least 80x24" "$WORKDIR/err"; then
        echo "G1.4: at $geometry the message did not name the minimum:" >&2
        sed 's/^/    /' "$WORKDIR/err" >&2
        STATUS=1
    fi
    if ! grep -q "unify-run -i" "$WORKDIR/err"; then
        echo "G1.4: at $geometry the message did not offer a way forward." >&2
        STATUS=1
    fi
    rm -rf "$WORKDIR"
}

check 79x24
check 80x23
check 40x10

# And the boundary itself must WORK, or the gate is just an off-by-one.
WORKDIR="$(mktemp -d)"
if ! "$LENS_BIN" --geometry 80x24 --layout browse --script "$SCRIPT" \
        > "$WORKDIR/out" 2>/dev/null; then
    echo "G1.4: 80x24 is the documented minimum but lens refused it." >&2
    STATUS=1
fi
if [ ! -s "$WORKDIR/out" ]; then
    echo "G1.4: 80x24 rendered nothing." >&2
    STATUS=1
fi
rm -rf "$WORKDIR"

if [ "$STATUS" -eq 0 ]; then
    echo "G1.4: lens refuses below 80x24 and renders at it."
fi
exit "$STATUS"
