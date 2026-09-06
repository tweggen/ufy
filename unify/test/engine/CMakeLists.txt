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
