#
# Session-boundary tests -- gate G0 of plans/todo/lens/ACCEPTANCE.md.
#
# Three things are built here, and they are deliberately three rather than
# one, because they fail for different reasons and a single target would
# hide which:
#
#   unify-session-isolation   G0.1 -- does the boundary header stand alone?
#   unify-session-hygiene     G0.2 -- does it name an engine type? (a script)
#   unify-session-contract    G0.3-G0.11 -- does an implementation obey it?
#

# ---------------------------------------------------------------------------
# G0.1 -- compile isolation.
#
# The boundary header is COPIED into a directory of its own and the isolation
# translation unit is compiled with that directory as its ONLY include path.
# Compiling against include/ instead would prove nothing: vault-unify.hpp
# lives there too, so `#include "vault-unify.hpp"` from inside the boundary
# header would resolve happily and the gate would pass while the abstraction
# leaked. Staging it is what makes the check mean what it says.
# ---------------------------------------------------------------------------

set(UNIFY_SESSION_ISOLATION_DIR "${CMAKE_CURRENT_BINARY_DIR}/isolation-include")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/../../include/vault-unify-session.hpp"
    "${UNIFY_SESSION_ISOLATION_DIR}/vault-unify-session.hpp"
    COPYONLY
)

add_executable(unify-session-isolation
    "${CMAKE_CURRENT_SOURCE_DIR}/isolation-tu.cpp"
)
set_target_properties(unify-session-isolation PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
# No target_link_libraries: it must not need the engine to build or to link.
target_include_directories(unify-session-isolation PRIVATE
    "${UNIFY_SESSION_ISOLATION_DIR}"
)
if(NOT MSVC)
    target_compile_options(unify-session-isolation PRIVATE -Wall -Wextra)
endif()

add_test(NAME unify-session-isolation COMMAND unify-session-isolation)

# ---------------------------------------------------------------------------
# G0.2 -- header hygiene.
# ---------------------------------------------------------------------------

find_program(BASH_PROGRAM bash)
if(BASH_PROGRAM)
    add_test(
        NAME unify-session-hygiene
        COMMAND "${BASH_PROGRAM}"
                "${CMAKE_CURRENT_SOURCE_DIR}/check-header-hygiene.sh"
                "${CMAKE_CURRENT_SOURCE_DIR}/../../include/vault-unify-session.hpp"
    )
else()
    message(WARNING "bash not found; skipping the G0.2 session-header hygiene test.")
endif()

# ---------------------------------------------------------------------------
# G0.3-G0.11 -- the contract suite.
#
# One suite, run against every Session implementation. Adding an
# implementation means adding a registration call in contract-main.cpp and
# nothing else; if a suite case needs to know which implementation it is
# running against, that case is testing the implementation rather than the
# contract and belongs somewhere else.
# ---------------------------------------------------------------------------

add_executable(unify-session-contract
    "${CMAKE_CURRENT_SOURCE_DIR}/contract-main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/contract-suite.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/fake-session.cpp"
)
set_target_properties(unify-session-contract PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
target_include_directories(unify-session-contract PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(unify-session-contract PRIVATE vault-unify-core)
if(NOT MSVC)
    target_compile_options(unify-session-contract PRIVATE -Wall -Wextra)
endif()

add_test(NAME unify-session-contract COMMAND unify-session-contract)
set_tests_properties(unify-session-contract PROPERTIES TIMEOUT 120)
