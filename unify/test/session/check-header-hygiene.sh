#!/usr/bin/env bash
#
# check-header-hygiene.sh -- gate G0.2.
#
# Usage:
#   check-header-hygiene.sh <path-to-vault-unify-session.hpp>
#
# Asserts that the public session header names no engine type and exposes no
# raw pointer. G0.1 (the isolation compile target) catches a stray #include;
# this catches the subtler failure, where someone forward-declares an engine
# type and puts it in a signature -- which compiles in isolation perfectly
# well and destroys the abstraction just the same.
#
# Comments and string literals are stripped before matching. That is not
# leniency: the header's job is to EXPLAIN why an engine type must not appear,
# so the words "Clause" and "World" have to be sayable in prose. Matching raw
# text would create exactly the incentive to stop explaining.
#
# Exit codes:
#   0  clean
#   1  a violation was found (each is printed with its line)
#   2  usage/setup error

set -u

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <header>" >&2
    exit 2
fi

HEADER="$1"
if [ ! -f "$HEADER" ]; then
    echo "check-header-hygiene.sh: '$HEADER' does not exist." >&2
    exit 2
fi

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT
STRIPPED="$WORKDIR/stripped"

# Strip, in order: block comments, line comments, string and char literals.
# Blank lines are kept so reported line numbers still match the real file.
# (`sed` cannot do multi-line reliably across implementations; this is the
# one place the harness uses awk, for a state machine over block comments.)
awk '
{
    line = $0
    out  = ""
    i    = 1
    n    = length( line )
    while ( i <= n ) {
        c  = substr( line, i, 1 )
        c2 = substr( line, i, 2 )
        if ( inblock ) {
            if ( c2 == "*/" ) { inblock = 0; i += 2 } else { i += 1 }
            continue
        }
        if ( c2 == "/*" ) { inblock = 1; i += 2; continue }
        if ( c2 == "//" ) { break }
        if ( c == "\"" || c == "'"'"'" ) {
            q = c
            i += 1
            while ( i <= n ) {
                d = substr( line, i, 1 )
                if ( d == "\\" ) { i += 2; continue }
                i += 1
                if ( d == q ) { break }
            }
            continue
        }
        out = out c
        i += 1
    }
    print out
}
' "$HEADER" > "$STRIPPED"

STATUS=0

report() {
    STATUS=1
    printf '%s\n' "$1" >&2
}

# 1. Engine type names, as whole words. `WorldChanged` and `clauseCount` are
#    fine and must stay fine -- they are boundary vocabulary that happens to
#    share a prefix, so the word boundaries are doing real work here.
for SYMBOL in Clause UnifyContext World SolveJob Engine ExecutionState Term WorldPtr; do
    if grep -nE "\\b${SYMBOL}\\b" "$STRIPPED" | grep -v '^\s*$' > "$WORKDIR/hits" 2>/dev/null; then
        if [ -s "$WORKDIR/hits" ]; then
            report "G0.2: engine type '${SYMBOL}' appears in the session header:"
            sed 's/^/    /' "$WORKDIR/hits" >&2
        fi
    fi
done

# 2. Boost, in any form. The boundary is standard C++ only; a front end must
#    not need Boost to talk to a core, and a BEAM core will not have it.
if grep -nE '\bboost\b|BOOST_' "$STRIPPED" > "$WORKDIR/hits" 2>/dev/null; then
    if [ -s "$WORKDIR/hits" ]; then
        report "G0.2: Boost appears in the session header:"
        sed 's/^/    /' "$WORKDIR/hits" >&2
    fi
fi

# 3. Raw pointers, anywhere in code. Not merely "in a signature": a raw
#    pointer in a struct field is the same leak one indirection later, and
#    "is this a signature?" is not a question a grep can answer honestly.
#    References are permitted (EventSink&); ownership crosses as values and
#    shared_ptr only.
#    The one exemption is `*this`, which is a dereference of an object the
#    caller already holds, not a pointer type crossing the boundary; it is
#    spelled out rather than pattern-matched loosely so that a future
#    `Foo *p` cannot slip in behind a broad regex.
if sed 's/\*[[:space:]]*this\b//g' "$STRIPPED" | grep -n '\*' > "$WORKDIR/hits" 2>/dev/null; then
    if [ -s "$WORKDIR/hits" ]; then
        report "G0.2: raw pointer ('*') in session header code (comments are exempt):"
        sed 's/^/    /' "$WORKDIR/hits" >&2
    fi
fi

# 4. Only standard-library includes. An #include of ours would fail G0.1's
#    compile too, but naming it here gives a readable error instead of a
#    compiler cascade.
if grep -nE '^\s*#\s*include\s*"' "$HEADER" > "$WORKDIR/hits" 2>/dev/null; then
    if [ -s "$WORKDIR/hits" ]; then
        report "G0.2: quoted #include in the session header (must be <> std headers only):"
        sed 's/^/    /' "$WORKDIR/hits" >&2
    fi
fi

if [ "$STATUS" -eq 0 ]; then
    echo "G0.2: $(basename "$HEADER") is clean."
fi
exit "$STATUS"
