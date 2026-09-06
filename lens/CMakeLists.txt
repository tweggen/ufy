#
# CMake build for lens/ ("unify-lens", a text-mode operating environment for
# Unify -- plans/todo/lens/).
#
# Standalone, the same way unify/ is: there is no top-level CMakeLists in
# this repository, and each module builds on its own. lens depends on the
# engine, which it pulls in as a subdirectory rather than expecting an
# installed package, so one command builds everything.
#
# Targets:
#   lens-layout       static library: the tiling tree and geometry solver.
#                     PURE -- no engine, no terminal library, no I/O.
#   lens-layout-test  the G1.2/G1.3 model tests.
#
# The layering rule (ARCHITECTURE.md section 1) is one arrow:
#   app -> panels -> model,  app -> term
# with model/ and panels/ seeing neither FTXUI nor an engine header. It is
# enforced mechanically, not by convention -- see test/check-layering.sh,
# gates G1.5 and G1.6.
#

cmake_minimum_required(VERSION 3.16)
project(unify-lens CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# Same knob, same spelling as the engine's, so one flag configures both.
set(UNIFY_SANITIZE "" CACHE STRING "Sanitizer(s) to build with (e.g. 'address,undefined'); empty disables")
if(NOT MSVC AND UNIFY_SANITIZE)
    add_compile_options(-fsanitize=${UNIFY_SANITIZE} -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${UNIFY_SANITIZE})
endif()

# The engine. Built as a subproject so `cmake -S lens` is the only command a
# newcomer needs; LENS_UNIFY_DIR exists so a packager can point elsewhere.
set(LENS_UNIFY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../unify"
    CACHE PATH "Path to the unify engine module")
if(NOT TARGET vault-unify-core)
    add_subdirectory("${LENS_UNIFY_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/unify")
endif()

enable_testing()

# ---------------------------------------------------------------------------
# layout/ -- the tiling tree and the solver.
#
# Deliberately its own library with no dependencies at all. It is the part of
# lens most worth testing and least worth coupling: a tiling solver that
# needed a terminal to be tested would not get tested.
# ---------------------------------------------------------------------------

add_library(lens-layout STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/layout/layout-tree.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/layout/solver.cpp"
)
target_include_directories(lens-layout PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
if(NOT MSVC)
    target_compile_options(lens-layout PRIVATE -Wall -Wextra)
endif()

add_executable(lens-layout-test "${CMAKE_CURRENT_SOURCE_DIR}/test/layout-test.cpp")
target_link_libraries(lens-layout-test PRIVATE lens-layout)
if(NOT MSVC)
    target_compile_options(lens-layout-test PRIVATE -Wall -Wextra)
endif()
add_test(NAME lens-layout COMMAND lens-layout-test)
set_tests_properties(lens-layout PROPERTIES TIMEOUT 120)
