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

find_program(BASH_PROGRAM bash)

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

# ---------------------------------------------------------------------------
# modreg/ -- the command table, keymaps, and the help surfaces generated from
# them. Pure: it depends on neither the engine nor a terminal library, which
# is what lets the "H" criterion be tested without either.
# ---------------------------------------------------------------------------

add_library(lens-modreg STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/modreg/command.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/modreg/keymap.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/modreg/help.cpp"
)
target_include_directories(lens-modreg PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
if(NOT MSVC)
    target_compile_options(lens-modreg PRIVATE -Wall -Wextra)
endif()

add_executable(lens-modreg-test "${CMAKE_CURRENT_SOURCE_DIR}/test/modreg-test.cpp")
target_link_libraries(lens-modreg-test PRIVATE lens-modreg)
if(NOT MSVC)
    target_compile_options(lens-modreg-test PRIVATE -Wall -Wextra)
endif()
add_test(NAME lens-modreg COMMAND lens-modreg-test)
set_tests_properties(lens-modreg PROPERTIES TIMEOUT 120)

# ---------------------------------------------------------------------------
# model/ -- the character grid. Pure: it is the value `view` produces and the
# only thing the terminal layer ever draws.
# ---------------------------------------------------------------------------

add_library(lens-model STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/model/cell-grid.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/model/help-content.cpp"
)
target_link_libraries(lens-model PUBLIC lens-modreg)
target_include_directories(lens-model PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
if(NOT MSVC)
    target_compile_options(lens-model PRIVATE -Wall -Wextra)
endif()

# The shell's own behaviour needs the model, the view and the layouts, so it
# links the same sources the app does rather than a library -- they are not
# worth a library of their own until a second consumer appears.
add_executable(lens-shell-test
    "${CMAKE_CURRENT_SOURCE_DIR}/test/shell-test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app/layouts.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/model/model.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/model/view.cpp"
)
target_link_libraries(lens-shell-test PRIVATE lens-model lens-modreg lens-layout)
if(NOT MSVC)
    target_compile_options(lens-shell-test PRIVATE -Wall -Wextra)
endif()
add_test(NAME lens-shell COMMAND lens-shell-test)
set_tests_properties(lens-shell PROPERTIES TIMEOUT 120)

add_executable(lens-grid-test "${CMAKE_CURRENT_SOURCE_DIR}/test/grid-test.cpp")
target_link_libraries(lens-grid-test PRIVATE lens-model)
if(NOT MSVC)
    target_compile_options(lens-grid-test PRIVATE -Wall -Wextra)
endif()
add_test(NAME lens-grid COMMAND lens-grid-test)
set_tests_properties(lens-grid PROPERTIES TIMEOUT 120)

# ---------------------------------------------------------------------------
# term/ -- FTXUI, and nothing else in the tree sees it (gate G1.5).
#
# Optional so that everything above the seam still builds, and every gate
# except the ones that need a screen still runs, on a machine with no network
# to fetch it. That is not a hypothetical convenience: the whole of G1.2,
# G1.3, G1.7 and the "H" criteria are provable without a terminal, and making
# them depend on a git fetch would be coupling the testable parts to the
# untestable one.
# ---------------------------------------------------------------------------

option(LENS_BUILD_TERM "Build the FTXUI terminal backend (fetches FTXUI)" ON)

if(LENS_BUILD_TERM)
    include(FetchContent)
    # Pinned, never a branch: a terminal library that changed under the
    # goldens would look like a lens regression.
    FetchContent_Declare(ftxui
        GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
        GIT_TAG        v7.0.3
        GIT_SHALLOW    TRUE
    )
    set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(FTXUI_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(FTXUI_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
    set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(ftxui)

    add_library(lens-term STATIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src/term/ftxui-terminal.cpp"
    )
    target_include_directories(lens-term PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(lens-term
        PUBLIC  lens-model lens-modreg
        PRIVATE ftxui::screen ftxui::dom ftxui::component
    )
    if(NOT MSVC)
        target_compile_options(lens-term PRIVATE -Wall -Wextra)
    endif()
endif()

# ---------------------------------------------------------------------------
# app/ -- the composition root, and the only place that knows both the
# terminal and the model exist.
# ---------------------------------------------------------------------------

add_executable(unify-lens
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app/layouts.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/model/model.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/model/view.cpp"
)
target_link_libraries(unify-lens PRIVATE lens-model lens-modreg lens-layout)
if(LENS_BUILD_TERM)
    target_link_libraries(unify-lens PRIVATE lens-term)
    target_compile_definitions(unify-lens PRIVATE LENS_HAVE_TERM=1)
endif()
if(NOT MSVC)
    target_compile_options(unify-lens PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# G1.1 / G1.2 / G1.7 -- golden screens.
#
# Every one runs through `--script`, which is the mechanism G1.7 asks for and
# also what makes a lens bug reproducible by a file. No terminal is involved,
# so these run in CI exactly as they run here.
#
# What these goldens are FOR: rule 6 of the method says assert on the model
# and use grids for layout. So they check that a panel renders, that the four
# stock layouts are what they claim to be, and that 80x24 degrades rather
# than clips. Behaviour is asserted in the model tests, where a cosmetic
# change does not force a dozen re-recordings.
# ---------------------------------------------------------------------------

if(BASH_PROGRAM)
    set(LENS_SCREEN_RUNNER "${CMAKE_CURRENT_SOURCE_DIR}/test/run-screen-test.sh")
    set(LENS_SCREEN_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/test/screens")
    set(LENS_GOLDEN_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/test/golden")

    # name | geometry | layout | key script
    set(LENS_SCREENS
        "browse-120x40|120x40|browse|empty"
        "browse-80x24|80x24|browse|empty"
        "run-120x40|120x40|run|empty"
        "run-80x24|80x24|run|empty"
        "debug-120x40|120x40|debug|empty"
        "debug-80x24|80x24|debug|empty"
        "full-120x40|120x40|full|empty"
        "full-80x24|80x24|full|empty"
        "tiling-120x40|120x40|browse|tiling"
        "tiling-80x24|80x24|browse|tiling"
        "maximise-120x40|120x40|browse|maximise"
        "resize-roundtrip|120x40|browse|resize-roundtrip"
        "chord-pending|120x40|browse|chord-pending"
        "help-120x40|120x40|browse|help"
        "help-80x24|80x24|browse|help"
        "palette-120x40|120x40|browse|palette"
        "palette-filtered|120x40|browse|palette-filtered"
    )

    foreach(LENS_SCREEN ${LENS_SCREENS})
        string(REPLACE "|" ";" PARTS "${LENS_SCREEN}")
        list(GET PARTS 0 SCREEN_NAME)
        list(GET PARTS 1 SCREEN_GEOMETRY)
        list(GET PARTS 2 SCREEN_LAYOUT)
        list(GET PARTS 3 SCREEN_SCRIPT)

        add_test(
            NAME "lens-screen-${SCREEN_NAME}"
            COMMAND "${BASH_PROGRAM}" "${LENS_SCREEN_RUNNER}"
                    "$<TARGET_FILE:unify-lens>"
                    "${SCREEN_GEOMETRY}"
                    "${SCREEN_LAYOUT}"
                    "${LENS_SCREEN_DIR}/${SCREEN_SCRIPT}.keys"
                    "${LENS_GOLDEN_DIR}/${SCREEN_NAME}.expected"
        )
        set_tests_properties("lens-screen-${SCREEN_NAME}" PROPERTIES
            SKIP_RETURN_CODE 77
            TIMEOUT 60
        )
    endforeach()

    # G1.4: below 80x24 lens exits with the pinned message and a non-zero
    # status, and does not render. Its own test because "it refuses" cannot
    # be a golden screen -- there is no screen.
    add_test(
        NAME lens-too-small
        COMMAND "${BASH_PROGRAM}" "${CMAKE_CURRENT_SOURCE_DIR}/test/check-too-small.sh"
                "$<TARGET_FILE:unify-lens>"
                "${LENS_SCREEN_DIR}/empty.keys"
    )
    set_tests_properties(lens-too-small PROPERTIES TIMEOUT 60)

    # G1.3 on the screen: an equivalence between two runs, so a script
    # rather than a golden -- it stays true when the screen changes.
    add_test(
        NAME lens-resize-roundtrip
        COMMAND "${BASH_PROGRAM}"
                "${CMAKE_CURRENT_SOURCE_DIR}/test/check-resize-roundtrip.sh"
                "$<TARGET_FILE:unify-lens>"
                "${LENS_SCREEN_DIR}"
    )
    set_tests_properties(lens-resize-roundtrip PROPERTIES TIMEOUT 60)
endif()

# ---------------------------------------------------------------------------
# G1.5 / G1.6 -- the layering gates.
#
# A script rather than a compile check, because what they forbid is an
# #include, and the cheapest honest way to prove a file does not include
# something is to look. Verified to be able to FAIL, not just to pass.
# ---------------------------------------------------------------------------

if(BASH_PROGRAM)
    add_test(
        NAME lens-layering
        COMMAND "${BASH_PROGRAM}"
                "${CMAKE_CURRENT_SOURCE_DIR}/test/check-layering.sh"
                "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
else()
    message(WARNING "bash not found; skipping the lens layering gates.")
endif()

add_executable(lens-layout-test "${CMAKE_CURRENT_SOURCE_DIR}/test/layout-test.cpp")
target_link_libraries(lens-layout-test PRIVATE lens-layout)
if(NOT MSVC)
    target_compile_options(lens-layout-test PRIVATE -Wall -Wextra)
endif()
add_test(NAME lens-layout COMMAND lens-layout-test)
set_tests_properties(lens-layout PROPERTIES TIMEOUT 120)
