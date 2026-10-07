cmake_minimum_required(VERSION 3.14)
# MSVC_RUNTIME_LIBRARY (dv-solve-smt2 on the static C runtime, below) rather
# than /MD in the default flags. CMake 3.15+.
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()
project(dv_solve C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Threading backend for the parallel cube engine (src/c/dvs_thread.c). Uses
# pthreads on POSIX; on Windows the Win32 threading APIs need no extra link
# library, so Threads::Threads is a no-op there.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# Export all symbols from the shared libraries on Windows so the ctypes loader
# can resolve them (MSVC exports nothing by default). No effect on ELF/Mach-O.
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

# ------------------------------------------------------------------ #
# ivpm packages directory                                             #
# ------------------------------------------------------------------ #
# Where `ivpm update` put the fetched dependencies (currently just
# cadical). ./packages is only correct when dv-solve is the root project;
# as a dependency inside another workspace the packages directory is the
# *parent* of the dv-solve source tree. Candidates, in priority order:
#
#   1. -DPACKAGES_DIR=... — what the ivpm_build PEP 517 backend passes on
#      the cmake command line (CmakeBuilder.configure). It already resolves
#      to <proj>/packages for a root project and to the parent directory
#      otherwise, so it is right in both layouts.
#   2. $IVPM_PACKAGES — exported by the generated packages.envrc once direnv
#      has loaded it. Not set by `ivpm update` itself, so it is a fallback.
#   3. <source>/packages — dv-solve configured standalone as a root project.
#   4. The directory containing dv-solve — checked out under another
#      project's packages/ dir and configured by plain cmake with no
#      -DPACKAGES_DIR.
if(NOT PACKAGES_DIR)
    if(DEFINED ENV{IVPM_PACKAGES})
        set(PACKAGES_DIR $ENV{IVPM_PACKAGES})
    elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/packages)
        set(PACKAGES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/packages)
    else()
        get_filename_component(PACKAGES_DIR ${CMAKE_CURRENT_SOURCE_DIR} DIRECTORY)
    endif()
endif()
set(PACKAGES_DIR ${PACKAGES_DIR} CACHE PATH "ivpm packages (dependency) directory")
message(STATUS "dv-solve: PACKAGES_DIR=${PACKAGES_DIR}")

# ------------------------------------------------------------------ #
# Vendored SAT solver (kissat) — Phase B.0                            #
# ------------------------------------------------------------------ #

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/kissat.cmake)

# ------------------------------------------------------------------ #
# Optional incremental SAT backend (CaDiCaL)                          #
# ------------------------------------------------------------------ #
# CaDiCaL is C++ and adds a libstdc++ dependency. It is the incremental
# backend for the bit-blast / formal path; kissat stays the one-shot and
# the lightweight-embeddable backend. -DDVS_WITH_CADICAL=OFF yields a
# pure-C, kissat-only build with no C++ runtime (see
# docs/cadical_integration_plan.md).
option(DVS_WITH_CADICAL "Build the incremental CaDiCaL SAT backend (adds a C++/libstdc++ dependency)" ON)
if(DVS_WITH_CADICAL)
    enable_language(CXX)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/cadical.cmake)
endif()

# Apply the CaDiCaL backend wiring to a dv-solve library target: link the
# static archive, define DVS_WITH_CADICAL for the dvs_sat dispatcher, and
# force C++ linking so libstdc++ is pulled in (a C target won't auto-detect
# the C++ runtime from a static archive).
function(dvs_apply_cadical tgt)
    if(DVS_WITH_CADICAL)
        target_link_libraries(${tgt} PRIVATE cadical)
        target_compile_definitions(${tgt} PRIVATE DVS_WITH_CADICAL=1)
        set_target_properties(${tgt} PROPERTIES LINKER_LANGUAGE CXX)
    endif()
endfunction()

# MSVC portability for the dv-solve C sources (applies to every target defined
# below — libraries, the CLI and the C tests). DVS_WIN_COMPAT is set in
# kissat.cmake. Force-includes the builtins/stat shim and puts the POSIX shim
# headers on the include path; kissat handles its own flags above.
if(MSVC)
    add_compile_options(/FI${DVS_WIN_COMPAT}/msvc_compat.h /wd4244 /wd4267 /wd4146)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS WIN32_LEAN_AND_MEAN)
    include_directories(${DVS_WIN_COMPAT})
endif()

# The C unit-test executables are developer tools, not part of any wheel. They
# also pull in POSIX-only APIs (setenv) and internal data symbols that aren't
# exported from the DLLs, so skip them on MSVC.
# Force the portable (no __int128, no GCC overflow builtins) arithmetic path of
# src/c/dvs_i128.h -- what MSVC builds use -- so it can be tested on Linux.
option(DVS_NO_INT128 "Use the portable 64-bit code instead of __int128" OFF)
if(DVS_NO_INT128)
    add_compile_definitions(DVS_NO_INT128=1)
endif()

# The C test and benchmark executables. Wheel builds turn them off through the
# DV_SOLVE_BUILD_TOOLS environment variable (set in pyproject.toml's
# [tool.cibuildwheel] section), since the ivpm_build backend has no way to pass
# -D options and the wheel ships none of them.
if(DEFINED ENV{DV_SOLVE_BUILD_TOOLS})
    set(_dvs_tools_default $ENV{DV_SOLVE_BUILD_TOOLS})
else()
    set(_dvs_tools_default ON)
endif()
option(DV_SOLVE_BUILD_TOOLS "Build the C test and benchmark executables" ${_dvs_tools_default})
if(MSVC)
    set(DV_SOLVE_BUILD_TOOLS OFF)
endif()

# The dv-solve-smt2 CLI. It ships in the wheel (pip puts it on PATH), so it is
# built independently of DV_SOLVE_BUILD_TOOLS.
option(DVS_BUILD_SMT2 "Build the dv-solve-smt2 SMT-LIB2 command-line tool" ON)

if(DV_SOLVE_BUILD_TOOLS)
add_executable(test_kissat_smoke tests/c/test_kissat_smoke.c)
target_link_libraries(test_kissat_smoke PRIVATE kissat)
target_include_directories(test_kissat_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/resources/kissat/src)
endif()
# Registered with CTest below, after enable_testing().

# ------------------------------------------------------------------ #
# Contradiction analysis (optional module)                            #
# ------------------------------------------------------------------ #

option(DVS_CONTRADICTION_ANALYSIS "Build contradiction analysis module" OFF)

# Soundness step checker (tests only; never in a release). Every learnt clause
# is checked, in a forked child, against the problem with learning off: a
# solution that violates the clause aborts the run. See
# docs/soundness_coverage_plan.md, P1.
option(DVS_STEP_CHECK "Check every learnt clause against the problem (slow; tests only)" OFF)
if(DVS_STEP_CHECK)
    add_compile_definitions(DVS_STEP_CHECK=1)
endif()

set(DVS_CONTRA_SOURCES
    src/c/dvs_contradiction.c
)

# ------------------------------------------------------------------ #
# Version                                                             #
# ------------------------------------------------------------------ #
# src/dv_solve/__version__.py is the single source of the version. The
# SMT-LIB2 tool reports it (--version, get-info :version).

file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/src/dv_solve/__version__.py _dvs_base
    REGEX "^BASE = ")
string(REGEX REPLACE "^BASE = \"([^\"]*)\".*" "\\1" DVS_VERSION "${_dvs_base}")
message(STATUS "dv-solve: version ${DVS_VERSION}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dv_solve/__version__.py)
set_source_files_properties(src/c/smt2/smt2_main.c src/c/smt2/smt2_frontend.c
    PROPERTIES COMPILE_DEFINITIONS "DVS_VERSION=\"${DVS_VERSION}\"")

# ------------------------------------------------------------------ #
# Source list                                                          #
# ------------------------------------------------------------------ #

set(DVS_SOURCES
    src/c/dvs_block_alloc.c
    src/c/dvs_builder.c
    src/c/dvs_compile.c
    src/c/dvs_conflict.c
    src/c/dvs_ctx.c
    src/c/dvs_nogood.c
    src/c/dvs_pool.c
    src/c/dvs_problem.c
    src/c/dvs_propagate.c
    src/c/dvs_prop_templates.c
    src/c/dvs_search.c
    src/c/dvs_shave.c
    src/c/dvs_stack.c
    src/c/dvs_checkpoint.c
    src/c/dvs_trail.c
    src/c/dvs_validate.c
    src/c/dvs_sv.c
    src/c/dvs_placement.c
    src/c/dvs_wiremask.c
    src/c/dvs_costguided.c
    src/c/dvs_lcg.c
    src/c/dvs_explain.c
    src/c/dvs_diffcycle.c
    src/c/dvs_clause_prop.c
    src/c/dvs_sat.c
    src/c/dvs_sat_kissat.c
    src/c/dvs_aig.c
    src/c/dvs_aig_cnf.c
    src/c/dvs_bitblast.c
    src/c/dvs_bbsolver.c
    src/c/dvs_cube.c
    src/c/dvs_thread.c
    src/c/dvs_arena.c
    src/c/dvs_bvdom.c
    src/c/dvs_bvbounds.c
)

# The CaDiCaL backend translation unit (pure C over the ccadical.h C API) is
# compiled into every dv-solve library only when the backend is enabled.
if(DVS_WITH_CADICAL)
    list(APPEND DVS_SOURCES src/c/dvs_sat_cadical.c)
endif()

# ------------------------------------------------------------------ #
# Shared library target                                                #
# ------------------------------------------------------------------ #

add_library(dv_solve SHARED ${DVS_SOURCES})
target_link_libraries(dv_solve PRIVATE kissat Threads::Threads)
dvs_apply_cadical(dv_solve)

if(DVS_CONTRADICTION_ANALYSIS)
    target_sources(dv_solve PRIVATE ${DVS_CONTRA_SOURCES})
    target_compile_definitions(dv_solve PRIVATE DVS_CONTRADICTION_ANALYSIS=1)
endif()

target_include_directories(dv_solve PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/c>
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(dv_solve PRIVATE -fsanitize=address -g -O0)
    target_link_options(dv_solve PRIVATE -fsanitize=address)
endif()

# ------------------------------------------------------------------ #
# Installation                                                         #
# ------------------------------------------------------------------ #

install(TARGETS dv_solve
    LIBRARY DESTINATION lib        # .so/.dylib (Unix)
    RUNTIME DESTINATION lib        # .dll (Windows)
    ARCHIVE DESTINATION lib        # import .lib (Windows)
)

# Public C headers → include/dv_solve (consumed by ivpm-build extra-data and by
# in-tree C consumers). Install all dv-solve headers but exclude the vendored
# kissat fork, whose headers are an internal implementation detail.
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/src/c/
    DESTINATION include/dv_solve
    FILES_MATCHING
        PATTERN "*.h"
        PATTERN "sat/kissat" EXCLUDE
)

# ------------------------------------------------------------------ #
# Debug library (always includes contradiction analysis)              #
# ------------------------------------------------------------------ #

add_library(dv_solve_debug SHARED ${DVS_SOURCES} ${DVS_CONTRA_SOURCES})
target_link_libraries(dv_solve_debug PRIVATE kissat Threads::Threads)
dvs_apply_cadical(dv_solve_debug)

target_compile_definitions(dv_solve_debug PRIVATE DVS_CONTRADICTION_ANALYSIS=1)

target_include_directories(dv_solve_debug PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/c>
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(dv_solve_debug PRIVATE -fsanitize=address -g -O0)
    target_link_options(dv_solve_debug PRIVATE -fsanitize=address)
endif()

install(TARGETS dv_solve_debug
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION lib
    ARCHIVE DESTINATION lib
)

# ------------------------------------------------------------------ #
# DPI shared library target                                            #
# ------------------------------------------------------------------ #

add_library(dv_solve_dpi SHARED
    src/c/dvs_dpi.c
    ${DVS_SOURCES}
)
target_link_libraries(dv_solve_dpi PRIVATE kissat Threads::Threads)
dvs_apply_cadical(dv_solve_dpi)

target_include_directories(dv_solve_dpi PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/c>
)

target_compile_definitions(dv_solve_dpi PRIVATE)

install(TARGETS dv_solve_dpi
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION lib
    ARCHIVE DESTINATION lib
)

# ------------------------------------------------------------------ #
# SMT-LIB2 lexer test                                                 #
# ------------------------------------------------------------------ #

if(DV_SOLVE_BUILD_TOOLS)

enable_testing()

add_executable(test_smt2_lexer
    tests/c/test_smt2_lexer.c
    src/c/smt2/smt2_lexer.c
)
target_include_directories(test_smt2_lexer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/c
)
add_test(NAME test_smt2_lexer COMMAND test_smt2_lexer)
add_test(NAME test_kissat_smoke COMMAND test_kissat_smoke)

add_executable(test_dvs_sat tests/c/test_dvs_sat.c)
target_link_libraries(test_dvs_sat PRIVATE dv_solve kissat)
target_include_directories(test_dvs_sat PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/c)
add_test(NAME test_dvs_sat COMMAND test_dvs_sat)

add_executable(test_kissat_dvs_alloc tests/c/test_kissat_dvs_alloc.c)
target_link_libraries(test_kissat_dvs_alloc PRIVATE dv_solve kissat)
target_include_directories(test_kissat_dvs_alloc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/c)
add_test(NAME test_kissat_dvs_alloc COMMAND test_kissat_dvs_alloc)

add_executable(test_dvs_sat_incremental tests/c/test_dvs_sat_incremental.c)
target_link_libraries(test_dvs_sat_incremental PRIVATE dv_solve kissat)
target_include_directories(test_dvs_sat_incremental PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/c)
add_test(NAME test_dvs_sat_incremental COMMAND test_dvs_sat_incremental)

# CaDiCaL backend tests: exercise real incremental behavior. Skips itself at
# runtime when built with -DDVS_WITH_CADICAL=OFF, so it is always safe to run.
add_executable(test_dvs_sat_cadical tests/c/test_dvs_sat_cadical.c)
target_link_libraries(test_dvs_sat_cadical PRIVATE dv_solve)
target_include_directories(test_dvs_sat_cadical PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/c)
add_test(NAME test_dvs_sat_cadical COMMAND test_dvs_sat_cadical)

add_executable(test_dvs_aig tests/c/test_dvs_aig.c)
target_link_libraries(test_dvs_aig PRIVATE dv_solve)
target_include_directories(test_dvs_aig PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/c)
add_test(NAME test_dvs_aig COMMAND test_dvs_aig)

add_executable(test_dvs_aig_cnf tests/c/test_dvs_aig_cnf.c)
target_link_libraries(test_dvs_aig_cnf PRIVATE dv_solve kissat)
target_include_directories(test_dvs_aig_cnf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/c)
add_test(NAME test_dvs_aig_cnf COMMAND test_dvs_aig_cnf)

add_executable(test_dvs_bitblast tests/c/test_dvs_bitblast.c)
target_link_libraries(test_dvs_bitblast PRIVATE dv_solve kissat)
target_include_directories(test_dvs_bitblast PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/c)
add_test(NAME test_dvs_bitblast COMMAND test_dvs_bitblast)

add_executable(test_dvs_bbsolver tests/c/test_dvs_bbsolver.c)
target_link_libraries(test_dvs_bbsolver PRIVATE dv_solve kissat)
target_include_directories(test_dvs_bbsolver PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/c)
add_test(NAME test_dvs_bbsolver COMMAND test_dvs_bbsolver)

add_executable(test_dvs_bbsolver_incremental tests/c/test_dvs_bbsolver_incremental.c)
target_link_libraries(test_dvs_bbsolver_incremental PRIVATE dv_solve kissat)
target_include_directories(test_dvs_bbsolver_incremental PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/c)
add_test(NAME test_dvs_bbsolver_incremental COMMAND test_dvs_bbsolver_incremental)

add_executable(test_dvs_block_alloc_cache tests/c/test_dvs_block_alloc_cache.c)
target_link_libraries(test_dvs_block_alloc_cache PRIVATE dv_solve)
target_include_directories(test_dvs_block_alloc_cache PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/c)
add_test(NAME test_dvs_block_alloc_cache COMMAND test_dvs_block_alloc_cache)

# The public API through dv_solve.h alone; the test includes no other header.
add_executable(test_public_api tests/c/test_public_api.c)
target_link_libraries(test_public_api PRIVATE dv_solve)
add_test(NAME test_public_api COMMAND test_public_api)

add_executable(test_ctx_dirty_buffer tests/c/test_ctx_dirty_buffer.c)
target_link_libraries(test_ctx_dirty_buffer PRIVATE dv_solve)
add_test(NAME test_ctx_dirty_buffer COMMAND test_ctx_dirty_buffer)

add_executable(test_dvs_arena tests/c/test_dvs_arena.c)
target_link_libraries(test_dvs_arena PRIVATE dv_solve)
target_include_directories(test_dvs_arena PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/c)
add_test(NAME test_dvs_arena COMMAND test_dvs_arena)

add_executable(test_dvs_bvdom tests/c/test_dvs_bvdom.c)
target_link_libraries(test_dvs_bvdom PRIVATE dv_solve)
target_include_directories(test_dvs_bvdom PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/c)
add_test(NAME test_dvs_bvdom COMMAND test_dvs_bvdom)

add_executable(test_prop_exhaustive tests/c/test_prop_exhaustive.c)
target_link_libraries(test_prop_exhaustive PRIVATE dv_solve)
target_include_directories(test_prop_exhaustive PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/c)
add_test(NAME test_prop_exhaustive COMMAND test_prop_exhaustive)

add_executable(test_dvs_bvbounds tests/c/test_dvs_bvbounds.c)
target_link_libraries(test_dvs_bvbounds PRIVATE dv_solve)
target_include_directories(test_dvs_bvbounds PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/c)
add_test(NAME test_dvs_bvbounds COMMAND test_dvs_bvbounds)

add_executable(test_dvs_wide_arith tests/c/test_dvs_wide_arith.c)
target_link_libraries(test_dvs_wide_arith PRIVATE dv_solve)
target_include_directories(test_dvs_wide_arith PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/c)
add_test(NAME test_dvs_wide_arith COMMAND test_dvs_wide_arith)

# ------------------------------------------------------------------ #
# SMT-LIB2 parser test                                                 #
# ------------------------------------------------------------------ #

add_executable(test_smt2_parser
    tests/c/test_smt2_parser.c
    src/c/smt2/smt2_lexer.c
    src/c/smt2/smt2_parser.c
)
target_include_directories(test_smt2_parser PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/c
)
add_test(NAME test_smt2_parser COMMAND test_smt2_parser)

# In-process randomize() benchmark: reuses the frontend parser to build one
# problem once, then times rediversify+readback -- the per-randomize cost an
# embedded (DPI) Verilator integration would pay, with no parse/build/reset/IPC.
add_executable(bench_inproc
    tests/c/bench_inproc.c
    src/c/smt2/smt2_lexer.c
    src/c/smt2/smt2_parser.c
    src/c/smt2/smt2_frontend.c
)
target_link_libraries(bench_inproc PRIVATE dv_solve)
target_include_directories(bench_inproc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/c)

# Replays a captured Verilator session over a pipe, through the in-process
# SMT-LIB front end, and through the builder API (cold and with a compiled-
# context cache), to cost each layer of the SMT-LIB integration.
add_executable(bench_vlt_replay
    tests/c/bench_vlt_replay.c
    src/c/smt2/smt2_lexer.c
    src/c/smt2/smt2_parser.c
    src/c/smt2/smt2_frontend.c
)
target_link_libraries(bench_vlt_replay PRIVATE dv_solve)
target_include_directories(bench_vlt_replay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/c)

# ------------------------------------------------------------------ #
# SMT-LIB2 frontend unit tests                                        #
# ------------------------------------------------------------------ #

add_executable(test_smt2_frontend
    tests/c/test_smt2_frontend.c
    src/c/smt2/smt2_lexer.c
    src/c/smt2/smt2_parser.c
    src/c/smt2/smt2_frontend.c
)
target_link_libraries(test_smt2_frontend PRIVATE dv_solve)
target_include_directories(test_smt2_frontend PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/c
)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(test_smt2_frontend PRIVATE -fsanitize=address -g -O0)
    target_link_options(test_smt2_frontend PRIVATE -fsanitize=address)
endif()
add_test(NAME test_smt2_frontend COMMAND test_smt2_frontend)

endif()  # DV_SOLVE_BUILD_TOOLS

# ------------------------------------------------------------------ #
# dv-solve-smt2 executable                                            #
# ------------------------------------------------------------------ #
# Linked against a static build of the library, so the executable is
# self-contained: pip installs it into <env>/bin, and the relative path from
# there to site-packages/dv_solve (where the shared library lives) differs
# between venv, --user and system installs, so no RPATH could reach it.

if(DVS_BUILD_SMT2)

# On MSVC "self-contained" also means the static C runtime (/MT): pip's
# Scripts directory holds no VCRUNTIME140.dll or MSVCP140.dll, and delvewheel
# bundles them only for the package's own DLLs. Every object linked in has to
# agree on the runtime, so kissat and CaDiCaL get /MT copies for it.
set(_dvs_smt2_kissat kissat)
set(_dvs_smt2_cadical cadical)
set(_dvs_static_crt "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
    dvs_add_kissat(kissat_mt)
    set_target_properties(kissat_mt PROPERTIES MSVC_RUNTIME_LIBRARY ${_dvs_static_crt})
    set(_dvs_smt2_kissat kissat_mt)
    if(DVS_WITH_CADICAL)
        dvs_add_cadical_msvc(cadical_mt_objs cadical_mt kissat_mt)
        set_target_properties(cadical_mt_objs cadical_mt PROPERTIES
            MSVC_RUNTIME_LIBRARY ${_dvs_static_crt})
        set(_dvs_smt2_cadical cadical_mt)
    endif()
endif()

add_library(dv_solve_static STATIC ${DVS_SOURCES})
target_link_libraries(dv_solve_static PUBLIC ${_dvs_smt2_kissat} Threads::Threads)
if(DVS_WITH_CADICAL)
    # PUBLIC rather than dvs_apply_cadical's PRIVATE: a static archive does
    # not carry its dependencies, so they have to reach the executable's link.
    target_link_libraries(dv_solve_static PUBLIC ${_dvs_smt2_cadical})
    target_compile_definitions(dv_solve_static PRIVATE DVS_WITH_CADICAL=1)
endif()
if(DVS_CONTRADICTION_ANALYSIS)
    target_sources(dv_solve_static PRIVATE ${DVS_CONTRA_SOURCES})
    target_compile_definitions(dv_solve_static PRIVATE DVS_CONTRADICTION_ANALYSIS=1)
endif()
target_include_directories(dv_solve_static PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/c>
)
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT MSVC)
    target_compile_options(dv_solve_static PRIVATE -fsanitize=address -g -O0)
endif()

set(SMT2_SOURCES
    src/c/smt2/smt2_lexer.c
    src/c/smt2/smt2_parser.c
    src/c/smt2/smt2_frontend.c
    src/c/smt2/smt2_main.c
)

add_executable(dv-solve-smt2 ${SMT2_SOURCES})
if(MSVC)
    set_target_properties(dv_solve_static dv-solve-smt2 PROPERTIES
        MSVC_RUNTIME_LIBRARY ${_dvs_static_crt})
endif()
target_link_libraries(dv-solve-smt2 PRIVATE dv_solve_static Threads::Threads)
if(DVS_WITH_CADICAL)
    # CaDiCaL is C++; link with the C++ driver so libstdc++/libc++ is pulled in.
    set_target_properties(dv-solve-smt2 PROPERTIES LINKER_LANGUAGE CXX)
endif()
target_include_directories(dv-solve-smt2 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/c
)
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT MSVC)
    target_compile_options(dv-solve-smt2 PRIVATE -fsanitize=address -g -O0)
    target_link_options(dv-solve-smt2 PRIVATE -fsanitize=address)
endif()
# Strip release builds: the wheel ships this file, and the symbol table is
# most of its size. -x on macOS keeps what dyld needs from a Mach-O exe.
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND CMAKE_STRIP AND NOT MSVC)
    if(APPLE)
        set(_dvs_strip_args -x)
    else()
        set(_dvs_strip_args --strip-all)
    endif()
    add_custom_command(TARGET dv-solve-smt2 POST_BUILD
        COMMAND ${CMAKE_STRIP} ${_dvs_strip_args} $<TARGET_FILE:dv-solve-smt2>
        VERBATIM)
endif()
# Drop the GNU property note from the Linux x86_64 executable. The
# manylinux_2_34 image (AlmaLinux 9) builds glibc for x86-64-v2, so its
# crt1.o is marked "ISA needed: x86-64-v2", and the linker copies that into
# every executable (not into shared libraries, which do not link crt1.o).
# auditwheel then refuses the wheel for the baseline x86_64 policy, although
# none of dv-solve's own code needs more than baseline. -z x86-64-baseline
# cannot override it: that image's ld aborts on the merge. The note also holds
# the CET (IBT/SHSTK) markers, which are lost with it.
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND CMAKE_OBJCOPY
        AND CMAKE_SYSTEM_NAME STREQUAL "Linux"
        AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    add_custom_command(TARGET dv-solve-smt2 POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} --remove-section .note.gnu.property
                $<TARGET_FILE:dv-solve-smt2>
        VERBATIM)
endif()
install(TARGETS dv-solve-smt2 RUNTIME DESTINATION bin)

endif()  # DVS_BUILD_SMT2
