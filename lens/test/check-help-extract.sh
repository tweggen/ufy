#!/usr/bin/env bash
#
# check-help-extract.sh -- that the help extractor still REFUSES.
#
# Usage:
#   check-help-extract.sh <lens-help-extract> <fixture-dir>
#
# tools/help-extract.cpp is strict by design: a language document it can no
# longer read stops the build, because the alternative -- quietly emitting
# fewer topics -- keeps the build green while F1 stops answering. A strict
# parser nobody tests on bad input is a strict parser in name only, so each
# fixture here is one way the documents could drift, and this asserts each
# is still caught, with exit 1 and a message that names the problem.
#
# The same discipline as check-spec-runner.sh (ACCEPTANCE.md rule 10).

set -u

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <lens-help-extract> <fixture-dir>" >&2
    exit 2
fi
TOOL="$1"
DIR="$2"

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT
FAIL=0

expect_refusal() {
    # $1 spec, $2 language, $3 the words the message must contain
    local out="$WORKDIR/out.inc"
    rm -f "$out"
    "$TOOL" "$DIR/$1" "$DIR/$2" "$out" > /dev/null 2> "$WORKDIR/stderr"
    local rc=$?
    if [ "$rc" -ne 1 ]; then
        echo "FAIL $1 + $2: exit $rc, expected 1" >&2
        FAIL=1
    elif ! grep -q -- "$3" "$WORKDIR/stderr"; then
        echo "FAIL $1 + $2: the message does not say '$3':" >&2
        sed 's/^/    /' "$WORKDIR/stderr" >&2
        FAIL=1
    elif [ -e "$out" ]; then
        echo "FAIL $1 + $2: refused, but wrote an output anyway" >&2
        FAIL=1
    else
        echo "ok   $1 + $2 refused: $(head -1 "$WORKDIR/stderr")"
    fi
}

expect_refusal spec-no-section.md    language-good.md "no '## 9. Builtins' heading"
expect_refusal spec-bad-signature.md language-good.md "spec-bad-signature.md:9: a bold line in section 9 with no"
expect_refusal spec-empty.md         language-good.md "yielded no builtins"
expect_refusal spec-duplicate.md     language-good.md "documented twice"
expect_refusal spec-good.md          language-bad-row.md "language-bad-row.md:5: a reserved-words row with 2 cells"

# And the control: good input still works, and a bold word that merely
# starts a wrapped line is not mistaken for a heading.
if "$TOOL" "$DIR/spec-good.md" "$DIR/language-good.md" "$WORKDIR/good.inc" \
        2> "$WORKDIR/stderr" \
   && grep -q '"builtin.print"' "$WORKDIR/good.inc" \
   && grep -q '"keyword.cut"' "$WORKDIR/good.inc"; then
    echo "ok   good input extracts"
else
    echo "FAIL good input did not extract:" >&2
    cat "$WORKDIR/stderr" >&2
    FAIL=1
fi

exit "$FAIL"
