#
# Engine-item tests -- the engine work plans/todo/lens gate G0 depends on.
#
# Separate from test/session/ on purpose. The session contract suite asserts
# these properties THROUGH the boundary, which is where they matter to a
# front end; these assert them AT the engine, which is where they are
# implemented. When both fail, the pair says whether the engine broke or the
# adapter did -- and that is worth two targets.
#
# Unlike the golden tests these are ordinary C++ executables, so they need
# no shell and are not skippable.
#

add_executable(unify-engine-provenance
    "${CMAKE_CURRENT_SOURCE_DIR}/provenance-catalogue-test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/unify-tool-support.cpp"
)
set_target_properties(unify-engine-provenance PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
target_include_directories(unify-engine-provenance PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tools"
)
target_link_libraries(unify-engine-provenance PRIVATE vault-unify-core)
if(NOT MSVC)
    target_compile_options(unify-engine-provenance PRIVATE -Wall -Wextra)
endif()

add_test(NAME unify-engine-provenance COMMAND unify-engine-provenance)
set_tests_properties(unify-engine-provenance PROPERTIES TIMEOUT 120)

# ---------------------------------------------------------------------------
# Engine item E7 -- structured solution values.
# plans/todo/lens/E7-STRUCTURED-VALUES.md.
#
# Two targets rather than one, because the item has two halves that fail
# for different reasons: the resolution of variables nested inside a bound
# term (E7.0/E7.2, which needs a real query and a real UnifyContext), and
# the term-to-Value conversion itself (E7.1, which is a pure function of a
# term and needs no engine at all). When both go red, the pair says which.
# ---------------------------------------------------------------------------

foreach(UNIFY_E7_TEST nested-binding term-value)
    add_executable("unify-engine-${UNIFY_E7_TEST}"
        "${CMAKE_CURRENT_SOURCE_DIR}/${UNIFY_E7_TEST}-test.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/unify-tool-support.cpp"
    )
    set_target_properties("unify-engine-${UNIFY_E7_TEST}" PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )
    target_include_directories("unify-engine-${UNIFY_E7_TEST}" PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../../src"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools"
    )
    target_link_libraries("unify-engine-${UNIFY_E7_TEST}" PRIVATE vault-unify-core)
    if(NOT MSVC)
        target_compile_options("unify-engine-${UNIFY_E7_TEST}" PRIVATE -Wall -Wextra)
    endif()

    add_test(NAME "unify-engine-${UNIFY_E7_TEST}"
             COMMAND "unify-engine-${UNIFY_E7_TEST}")
    set_tests_properties("unify-engine-${UNIFY_E7_TEST}" PROPERTIES TIMEOUT 120)
endforeach()
