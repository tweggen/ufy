#
# CMake build for unify/ ("Unify" logic-language engine).
#
# This is the Phase 0 ("Reproducible baseline") CMake port called for by
# ROADMAP.md. It is meant to build cleanly on Linux (the historical/only
# target so far, via the legacy Jamfile in this same directory) with a
# modern compiler and a reasonably recent Boost. It intentionally does NOT
# touch any C++03-era boost usage in src/ (boost::shared_ptr, boost::thread,
# boost::function, Boost.Spirit Qi) -- that migration is ROADMAP.md Phase 5.5,
# not this step.
#
# Targets:
#   vault-unify-core   static library: the engine core (parser, terms,
#                       unification, solver, engine, world, runtime context).
#                       Boost and Threads are its only dependencies.
#   unify-run           executable: the CLI front end -- batch mode (used by
#                       the golden-output test harness) plus the interactive
#                       REPL; see tools/unify-run.cpp and tools/unify-repl.cpp.
#
# Options:
#   UNIFY_USE_READLINE  (default ON) link GNU readline into unify-run, if it
#                       can be found, so the REPL gets line editing and
#                       history. Purely optional: without it the REPL reads
#                       plain lines from stdin.
#   UNIFY_BUILD_XDEBUG  (default ON on UNIX, OFF elsewhere) build the xdebug
#                       TCP debugger backend. It is pure Boost.Asio except for
#                       one POSIX ::access() call in vault-unify-xdebug.cpp,
#                       so it is excluded by default on non-UNIX platforms
#                       rather than patched.
#   UNIFY_SANITIZE      (default empty) comma-separated -fsanitize= value(s),
#                       e.g. "address,undefined". Applied to both targets via
#                       add_compile_options/add_link_options. Not supported
#                       under MSVC.
#

cmake_minimum_required(VERSION 3.16)
project(vault-unify CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# UNIFY_SANITIZE: comma-separated -fsanitize= value(s), e.g. "address,undefined".
# Empty (the default) builds with no sanitizer. This is a standalone project
# with exactly two targets (vault-unify-core, unify-run), so applying the
# flags directory-wide via add_compile_options/add_link_options here -- rather
# than duplicating target_compile_options/target_link_options on each target
# -- is the simplest correct approach; it also automatically reaches the
# test/ subdirectory's targets, if any. Not supported under MSVC.
set(UNIFY_SANITIZE "" CACHE STRING "Sanitizer(s) to build with (e.g. 'address,undefined'); empty disables")
if(NOT MSVC AND UNIFY_SANITIZE)
    add_compile_options(-fsanitize=${UNIFY_SANITIZE} -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${UNIFY_SANITIZE})
endif()

if(UNIX)
    option(UNIFY_BUILD_XDEBUG "Build the xdebug TCP debugger backend" ON)
else()
    option(UNIFY_BUILD_XDEBUG "Build the xdebug TCP debugger backend" OFF)
endif()

find_package(Boost REQUIRED COMPONENTS thread system filesystem)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Source partitioning.
#
# Everything in src/ is the pure engine core and needs only Boost +
# Threads.
#
# The xdebug-* files are pure Boost.Asio/Boost.Thread except for one POSIX
# ::access() call in vault-unify-xdebug.cpp, so they are split out behind
# UNIFY_BUILD_XDEBUG instead of being patched for portability.
# ---------------------------------------------------------------------------

file(GLOB UNIFY_ALL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

set(UNIFY_XDEBUG_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vault-unify-xdebug.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vault-unify-xdebug-context.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vault-unify-xdebug-tcp.cpp"
)

set(UNIFY_CORE_SOURCES ${UNIFY_ALL_SOURCES})
if(NOT UNIFY_BUILD_XDEBUG)
    list(REMOVE_ITEM UNIFY_CORE_SOURCES ${UNIFY_XDEBUG_SOURCES})
endif()

add_library(vault-unify-core STATIC ${UNIFY_CORE_SOURCES})

target_include_directories(vault-unify-core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC
        # NB: src/ holds this module's *private* headers (vault-unification.hpp,
        # vault-unify-clause-builtin.hpp, vault-unify-solvejob.hpp, ...), all
        # included project-wide with angle brackets. The legacy Jamfile made
        # src/ a project-wide include path too (<include>src under
        # `project: requirements`); we keep that as PUBLIC here for the same
        # reason -- tools/unify-run.cpp reaches into vault-unify-solvejob.hpp
        # for its job-completion barrier (see that file for why). Tightening
        # this into a real public/private split is header-hygiene cleanup for
        # a later phase, not this CMake port.
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PRIVATE
        # ../include: the repo-wide shared headers, vendored from the vault
        # repository's combine/include/. Only vault-unify-xdebug-tcp.cpp uses
        # it (vault::BoostAsioIoService from vault/vault.hpp); the
        # VAULT_NO_PPLX definition below keeps that header from dragging
        # cpprest's pplx in, which nothing here needs.
        "${CMAKE_CURRENT_SOURCE_DIR}/../include"
)

target_compile_definitions(vault-unify-core PRIVATE VAULT_NO_PPLX)

target_link_libraries(vault-unify-core
    PUBLIC
        Boost::thread
        Boost::system
        Boost::filesystem
        Threads::Threads
)

# The C++03-era boost::bind/boost::asio usage in vault-unify-xdebug-tcp.cpp
# relies on the pre-C++11 global placeholders (_1, _2, ...); newer Boost
# warns (or with BOOST_BIND_NO_PLACEHOLDERS, fails) unless told this is
# intentional.
target_compile_definitions(vault-unify-core PUBLIC BOOST_BIND_GLOBAL_PLACEHOLDERS)

if(MSVC)
    target_compile_definitions(vault-unify-core PUBLIC _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(vault-unify-core PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# unify-run: the CLI front end (see tools/unify-run.cpp for how it drives
# RuntimeContext / SolveJob). Batch mode is what the golden-output test
# harness -- the rest of Phase 0 -- invokes; tools/unify-repl.cpp adds the
# interactive REPL of ROADMAP.md Phase 4 on top of the same RuntimeContext.
# ---------------------------------------------------------------------------
add_executable(unify-run
    tools/unify-run.cpp
    tools/unify-repl.cpp
    tools/unify-tool-support.cpp
)
target_link_libraries(unify-run PRIVATE vault-unify-core)
if(NOT MSVC)
    target_compile_options(unify-run PRIVATE -Wall -Wextra)
endif()

# UNIFY_USE_READLINE: give the REPL line editing, history and a persistent
# ~/.unify_history via GNU readline. Strictly optional and auto-detected --
# when readline is not installed the REPL falls back to plain std::getline
# (see readInputLine() in tools/unify-repl.cpp), so no build, and no CI job,
# ever gains a hard dependency on it. Set to OFF to force the fallback.
#
# HINTS cover Homebrew's readline, which is keg-only (macOS ships an
# libedit-based libreadline with no header of its own, so plain
# find_path()/find_library() would either find nothing or find the wrong
# thing there).
option(UNIFY_USE_READLINE "Use GNU readline for the unify-run REPL, if available" ON)

if(UNIFY_USE_READLINE)
    find_path(UNIFY_READLINE_INCLUDE_DIR
        NAMES readline/readline.h
        HINTS /opt/homebrew/opt/readline /usr/local/opt/readline
        PATH_SUFFIXES include
    )
    find_library(UNIFY_READLINE_LIBRARY
        NAMES readline
        HINTS /opt/homebrew/opt/readline /usr/local/opt/readline
        PATH_SUFFIXES lib
    )
    # Only some readline builds keep the history functions in a library of
    # their own; where they do not, libreadline already carries them.
    find_library(UNIFY_READLINE_HISTORY_LIBRARY
        NAMES history
        HINTS /opt/homebrew/opt/readline /usr/local/opt/readline
        PATH_SUFFIXES lib
    )

    if(UNIFY_READLINE_INCLUDE_DIR AND UNIFY_READLINE_LIBRARY)
        message(STATUS "Unify REPL: using GNU readline (${UNIFY_READLINE_LIBRARY})")
        target_compile_definitions(unify-run PRIVATE UNIFY_HAVE_READLINE)
        target_include_directories(unify-run PRIVATE "${UNIFY_READLINE_INCLUDE_DIR}")
        target_link_libraries(unify-run PRIVATE "${UNIFY_READLINE_LIBRARY}")
        if(UNIFY_READLINE_HISTORY_LIBRARY)
            target_link_libraries(unify-run PRIVATE "${UNIFY_READLINE_HISTORY_LIBRARY}")
        endif()
    else()
        message(STATUS "Unify REPL: GNU readline not found, using plain stdin")
    endif()
endif()

# ---------------------------------------------------------------------------
# Golden-output tests. The harness itself is a separate piece of work; this
# is just the hook it plugs into.
# ---------------------------------------------------------------------------
enable_testing()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/CMakeLists.txt")
    # tests are registered via test/CMakeLists.txt if present
    add_subdirectory(test)
endif()
