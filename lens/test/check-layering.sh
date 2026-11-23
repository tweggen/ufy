#!/usr/bin/env bash
#
# check-layering.sh -- gates G1.5 and G1.6 of plans/todo/lens/ACCEPTANCE.md.
#
# Usage:
#   check-layering.sh <lens-src-dir>
#
# ARCHITECTURE.md section 1 states the layering as one arrow --
# app -> panels -> model, and app -> term -- with two absolute rules:
#
#   G1.5  No file outside src/term/ includes an FTXUI header. That is what
#         makes "replacing the terminal library is a two-file job" a fact
#         rather than an intention, and it is the reason the plan can afford
#         to have chosen FTXUI from documentation rather than from use.
#
#   G1.6  model/ and panels/ include no engine header. They see the session
#         boundary types and their own types, and nothing else -- which is
#         what makes the golden-screen harness possible at all, since a
#         model that needed an engine could not be rendered in a test.
#
# Note the deliberate exception in G1.6: vault-unify-session.hpp IS allowed.
# It is the boundary, it is pure standard C++ (gate G0.1 proves it), and
# seeing it is the entire point of having drawn one. Banning it would force
# panels to invent a parallel vocabulary for the same values.
#
# Exit codes:
#   0  clean
#   1  a violation was found (each is printed with its file and line)
#   2  usage/setup error

set -u

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <lens-src-dir>" >&2
    exit 2
fi

SRC="$1"
if [ ! -d "$SRC" ]; then
    echo "check-layering.sh: '$SRC' is not a directory." >&2
    exit 2
fi

STATUS=0

report() {
    STATUS=1
    printf '%s\n' "$1" >&2
}

# --- G1.5: FTXUI is confined to term/ --------------------------------------

FTXUI_HITS="$(grep -rn --include='*.hpp' --include='*.cpp' \
    -E '#[[:space:]]*include[[:space:]]*[<"]ftxui/' "$SRC" 2>/dev/null \
    | grep -v "^${SRC}/term/" || true)"

if [ -n "$FTXUI_HITS" ]; then
    report "G1.5: FTXUI is included outside src/term/:"
    printf '%s\n' "$FTXUI_HITS" | sed 's/^/    /' >&2
fi

# --- G1.6: model/ and panels/ see no engine header -------------------------
#
# Matched on the include line, not on any mention of the name, so a comment
# explaining why an engine type must not appear here does not trip the gate.
# That exemption is the same one the session header's hygiene check makes,
# and for the same reason: the rule has to be explainable in the file that
# obeys it.

for LAYER in model panels; do
    if [ ! -d "${SRC}/${LAYER}" ]; then
        continue
    fi

    ENGINE_HITS="$(grep -rn --include='*.hpp' --include='*.cpp' \
        -E '#[[:space:]]*include[[:space:]]*[<"](vault-unify|vault-unification)' \
        "${SRC}/${LAYER}" 2>/dev/null \
        | grep -v 'vault-unify-session\.hpp' \
        | grep -v 'vault-unify-session-value\.hpp' || true)"

    if [ -n "$ENGINE_HITS" ]; then
        report "G1.6: ${LAYER}/ includes an engine header:"
        printf '%s\n' "$ENGINE_HITS" | sed 's/^/    /' >&2
    fi

    BOOST_HITS="$(grep -rn --include='*.hpp' --include='*.cpp' \
        -E '#[[:space:]]*include[[:space:]]*[<"]boost/' \
        "${SRC}/${LAYER}" 2>/dev/null || true)"

    if [ -n "$BOOST_HITS" ]; then
        report "G1.6: ${LAYER}/ includes a Boost header (the model is standard C++):"
        printf '%s\n' "$BOOST_HITS" | sed 's/^/    /' >&2
    fi
done

# --- layout/ and modreg/ are pure ------------------------------------------
#
# Not in the gates as written, and worth checking anyway: these two are the
# parts most useful to test in isolation, so a dependency creeping in would
# cost more than it looks. Same rules as model/.

for LAYER in layout modreg; do
    if [ ! -d "${SRC}/${LAYER}" ]; then
        continue
    fi
    PURE_HITS="$(grep -rn --include='*.hpp' --include='*.cpp' \
        -E '#[[:space:]]*include[[:space:]]*[<"](ftxui/|boost/|vault-unify)' \
        "${SRC}/${LAYER}" 2>/dev/null || true)"
    if [ -n "$PURE_HITS" ]; then
        report "${LAYER}/ must stay free of FTXUI, Boost and the engine:"
        printf '%s\n' "$PURE_HITS" | sed 's/^/    /' >&2
    fi
done

if [ "$STATUS" -eq 0 ]; then
    echo "G1.5/G1.6: lens layering is clean."
fi
exit "$STATUS"
