#
# CMake build for combine/modules/unify ("Unify" logic-language engine).
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
#                       unification, solver, engine, world, runtime context),
#                       with no dependency on cpprest / rest-server.
#   vault-unify-rest    static library: adds the REST server frontend.
#                       Only built if UNIFY_BUILD_REST is ON *and* cpprestsdk
#                       plus a CMake-buildable combine/modules/rest-server can
#                       actually be found; otherwise this is a guarded no-op
#                       (see the UNIFY_BUILD_REST block below).
#   unify-run           executable: minimal CLI runner used by the (future)
#                       golden-output test harness; see tools/unify-run.cpp.
#
# Options:
#   UNIFY_BUILD_REST    (default OFF)  build the REST server frontend.
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

option(UNIFY_BUILD_REST "Build the REST server frontend (needs cpprestsdk + rest-server)" OFF)
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
# src/vault-unify-rest-server.cpp{,.hpp} are the only files in this module
# that touch cpprest / combine/modules/rest-server (verified by grepping all
# of src/ for cpprest, rest-server and http includes); everything else here
# is the pure engine core and only needs Boost + Threads.
#
# The xdebug-* files are pure Boost.Asio/Boost.Thread except for one POSIX
# ::access() call in vault-unify-xdebug.cpp, so they are split out behind
# UNIFY_BUILD_XDEBUG instead of being patched for portability.
# ---------------------------------------------------------------------------

file(GLOB UNIFY_ALL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

set(UNIFY_REST_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vault-unify-rest-server.cpp"
)

set(UNIFY_XDEBUG_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vault-unify-xdebug.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vault-unify-xdebug-context.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vault-unify-xdebug-tcp.cpp"
)

set(UNIFY_CORE_SOURCES ${UNIFY_ALL_SOURCES})
list(REMOVE_ITEM UNIFY_CORE_SOURCES ${UNIFY_REST_SOURCES})
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
        # combine/include: the repo-wide shared headers. Only
        # vault-unify-xdebug-tcp.cpp uses it (vault::BoostAsioIoService from
        # vault/vault.hpp); VAULT_NO_PPLX below keeps that header from
        # dragging cpprest's pplx into this cpprest-free core build.
        "${CMAKE_CURRENT_SOURCE_DIR}/../../include"
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
# Optional REST server frontend.
#
# Sketched/guarded per ROADMAP.md Phase 0: cpprest may simply not be present,
# and combine/modules/rest-server (its Jamfile-only sibling module) has no
# CMake port of its own yet either -- that is out of scope here. If both
# preconditions happen to be met, build it; otherwise warn and skip instead
# of failing the whole configure.
# ---------------------------------------------------------------------------
if(UNIFY_BUILD_REST)
    find_package(cpprestsdk CONFIG QUIET)
    set(UNIFY_REST_SERVER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../rest-server")

    if(NOT cpprestsdk_FOUND)
        message(WARNING
            "UNIFY_BUILD_REST is ON but find_package(cpprestsdk) did not find it "
            "(e.g. via vcpkg: 'vcpkg install cpprestsdk'). Skipping vault-unify-rest.")
    elseif(NOT EXISTS "${UNIFY_REST_SERVER_DIR}/CMakeLists.txt")
        message(WARNING
            "UNIFY_BUILD_REST is ON but combine/modules/rest-server has no "
            "CMakeLists.txt yet (only its Jamfile) -- it needs its own CMake "
            "port before vault-unify-rest can be built. Skipping.")
    else()
        add_subdirectory("${UNIFY_REST_SERVER_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/rest-server")

        add_library(vault-unify-rest STATIC "${UNIFY_REST_SOURCES}")
        target_link_libraries(vault-unify-rest
            PUBLIC
                vault-unify-core
                vault-rest-server
                cpprestsdk::cpprest
        )
        if(NOT MSVC)
            target_compile_options(vault-unify-rest PRIVATE -Wall -Wextra)
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# unify-run: minimal CLI runner (see tools/unify-run.cpp for how it drives
# RuntimeContext / SolveJob). Used by the golden-output test harness that is
# the rest of Phase 0.
# ---------------------------------------------------------------------------
add_executable(unify-run tools/unify-run.cpp)
target_link_libraries(unify-run PRIVATE vault-unify-core)
if(NOT MSVC)
    target_compile_options(unify-run PRIVATE -Wall -Wextra)
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
