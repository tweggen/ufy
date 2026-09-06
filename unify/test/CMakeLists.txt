#
# Golden-output tests for the Unify engine (ROADMAP.md Phase 0).
#
# Each sample .ufy program listed below is run through the `unify-run`
# executable and its stdout is diffed against test/golden/<name>.expected
# by run-golden-test.sh. See that script for exit-code conventions
# (0 = match, 1 = mismatch with diff, 77 = no golden file yet -> SKIP,
# 2 = usage/setup error) and for how to (re)generate golden files via
# UNIFY_UPDATE_GOLDEN=1 (see also test/golden/README.md).
#
# The harness is invoked as `bash run-golden-test.sh ...` explicitly so it
# does not depend on the script's executable bit surviving checkout.
#

#
# Session-boundary tests -- plans/todo/lens/ACCEPTANCE.md gate G0.
#
# Deliberately added BEFORE the bash check below: that check `return()`s out
# of this file when bash is missing, and gate G0.1 is a compile test that has
# no business being skipped because a shell is absent.
#
add_subdirectory(session)

find_program(BASH_PROGRAM bash)
if(NOT BASH_PROGRAM)
    message(WARNING "bash not found; skipping Unify golden-output tests.")
    return()
endif()

set(UNIFY_GOLDEN_RUNNER "${CMAKE_CURRENT_SOURCE_DIR}/run-golden-test.sh")
set(UNIFY_GOLDEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/golden")
set(UNIFY_MODULE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")

# name -> path to the .ufy program (relative to the module root).
set(UNIFY_GOLDEN_TESTS
    "test-engine|${UNIFY_MODULE_DIR}/test-engine.ufy"
    "test2|${UNIFY_MODULE_DIR}/test2.ufy"
    "mapsyntax|${CMAKE_CURRENT_SOURCE_DIR}/mapsyntax.ufy"
    "pathfinder|${UNIFY_MODULE_DIR}/pathfinder.ufy"
    "mediaplayer|${UNIFY_MODULE_DIR}/mediaplayer.ufy"
)
# NOTE: mediaplayer.ufy (ROADMAP.md Phase 2) is now a self-contained
# multi-room media-player controller demo again; test/golden/mediaplayer.expected
# does not exist yet in this environment (no Boost toolchain here to build
# unify-run and generate it), so this test SKIPs until a golden file is
# generated on a machine that can build the module -- same as every other
# golden test below with no pre-existing golden file.
# NOTE: pathfinder.ufy references home-automation driver builtins (e.g.
# zw_node_value) that may not exist in vault-unify-core; it may only parse,
# not solve. run-golden-test.sh handles that gracefully -- it diffs whatever
# stdout unify-run produces (even if unify-run itself exits non-zero), so
# the golden file simply pins down today's actual behaviour.

foreach(UNIFY_GOLDEN_TEST ${UNIFY_GOLDEN_TESTS})
    string(REPLACE "|" ";" UNIFY_GOLDEN_TEST_PARTS "${UNIFY_GOLDEN_TEST}")
    list(GET UNIFY_GOLDEN_TEST_PARTS 0 UNIFY_GOLDEN_NAME)
    list(GET UNIFY_GOLDEN_TEST_PARTS 1 UNIFY_GOLDEN_PROGRAM)

    add_test(
        NAME "unify-golden-${UNIFY_GOLDEN_NAME}"
        COMMAND "${BASH_PROGRAM}" "${UNIFY_GOLDEN_RUNNER}"
                "$<TARGET_FILE:unify-run>"
                "${UNIFY_GOLDEN_PROGRAM}"
                "${UNIFY_GOLDEN_DIR}/${UNIFY_GOLDEN_NAME}.expected"
    )
    set_tests_properties("unify-golden-${UNIFY_GOLDEN_NAME}" PROPERTIES
        SKIP_RETURN_CODE 77
        # A runaway query (no depth/step limits yet -- ROADMAP.md Phase 3)
        # must not hang the test run.
        TIMEOUT 120
    )
endforeach()

# Negative test: a deliberately malformed .ufy program (ROADMAP.md Phase 1,
# "parse errors report file, line, column and the offending line"). unify-run
# is expected to exit 1 (see tools/unify-run.cpp) and to still produce empty
# stdout -- the diagnostic itself goes to stderr, not stdout, so the golden
# file test/golden/parse-error.expected is intentionally empty. The expected
# exit code is passed to run-golden-test.sh via the UNIFY_EXPECT_EXIT
# environment variable (see that script for details).
add_test(
    NAME "unify-golden-parse-error"
    COMMAND "${BASH_PROGRAM}" "${UNIFY_GOLDEN_RUNNER}"
            "$<TARGET_FILE:unify-run>"
            "${CMAKE_CURRENT_SOURCE_DIR}/parse-error.ufy"
            "${UNIFY_GOLDEN_DIR}/parse-error.expected"
)
set_tests_properties("unify-golden-parse-error" PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 120
    ENVIRONMENT "UNIFY_EXPECT_EXIT=1"
)

#
# Conformance tests (ROADMAP.md Phase 1: "Add a conformance test per rule",
# see ../SPEC.md). Each program in test/conformance/ exercises exactly one
# rule documented in SPEC.md; the expected output for each is derived by
# hand in that program's own header comment (see SPEC.md for the citations
# backing that prediction). Same harness, same golden-file conventions as
# UNIFY_GOLDEN_TESTS above; golden files live at
# golden/conformance-<name>.expected and, like the pre-existing sample
# programs, do not exist yet in this environment (no Boost toolchain here
# to build unify-run and generate them) -- these tests SKIP until a golden
# file is generated on a machine that can build the module.
#

set(UNIFY_CONFORMANCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/conformance")

# name -> path to the .ufy program (relative to test/conformance/).
set(UNIFY_CONFORMANCE_TESTS
    "clause-order|${UNIFY_CONFORMANCE_DIR}/clause-order.ufy"
    "unification-basics|${UNIFY_CONFORMANCE_DIR}/unification-basics.ufy"
    "map-unify|${UNIFY_CONFORMANCE_DIR}/map-unify.ufy"
    "deref|${UNIFY_CONFORMANCE_DIR}/deref.ufy"
    "infix-unify|${UNIFY_CONFORMANCE_DIR}/infix-unify.ufy"
    "negation|${UNIFY_CONFORMANCE_DIR}/negation.ufy"
    "if-statement|${UNIFY_CONFORMANCE_DIR}/if-statement.ufy"
    "strings-numbers|${UNIFY_CONFORMANCE_DIR}/strings-numbers.ufy"
    "arithmetic|${UNIFY_CONFORMANCE_DIR}/arithmetic.ufy"
    "comparison|${UNIFY_CONFORMANCE_DIR}/comparison.ufy"
    "cut|${UNIFY_CONFORMANCE_DIR}/cut.ufy"
    "findall-arrays|${UNIFY_CONFORMANCE_DIR}/findall-arrays.ufy"
    "loops|${UNIFY_CONFORMANCE_DIR}/loops.ufy"
    "assert-retract|${UNIFY_CONFORMANCE_DIR}/assert-retract.ufy"
    "strings|${UNIFY_CONFORMANCE_DIR}/strings.ufy"
    "recursion|${UNIFY_CONFORMANCE_DIR}/recursion.ufy"
    # NOTE: imports-lib.ufy (same directory) is deliberately NOT listed here
    # -- it is a library file, only ever reached via imports.ufy's own
    # `import "imports-lib.ufy";`, and has no golden-test entry of its own.
    "imports|${UNIFY_CONFORMANCE_DIR}/imports.ufy"
)

foreach(UNIFY_CONFORMANCE_TEST ${UNIFY_CONFORMANCE_TESTS})
    string(REPLACE "|" ";" UNIFY_CONFORMANCE_TEST_PARTS "${UNIFY_CONFORMANCE_TEST}")
    list(GET UNIFY_CONFORMANCE_TEST_PARTS 0 UNIFY_CONFORMANCE_NAME)
    list(GET UNIFY_CONFORMANCE_TEST_PARTS 1 UNIFY_CONFORMANCE_PROGRAM)

    add_test(
        NAME "unify-golden-conformance-${UNIFY_CONFORMANCE_NAME}"
        COMMAND "${BASH_PROGRAM}" "${UNIFY_GOLDEN_RUNNER}"
                "$<TARGET_FILE:unify-run>"
                "${UNIFY_CONFORMANCE_PROGRAM}"
                "${UNIFY_GOLDEN_DIR}/conformance-${UNIFY_CONFORMANCE_NAME}.expected"
    )
    set_tests_properties("unify-golden-conformance-${UNIFY_CONFORMANCE_NAME}" PROPERTIES
        SKIP_RETURN_CODE 77
        TIMEOUT 120
    )
endforeach()
