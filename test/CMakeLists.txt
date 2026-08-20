#
# Golden-output tests for combine/modules/unify (ROADMAP.md Phase 0).
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
)
# NOTE: mediaplayer.ufy is currently an empty file (ROADMAP.md) and is
# intentionally not part of the golden suite until it has content again.
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
