cmake_minimum_required(VERSION 3.14)
project(dv_solve C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Export all symbols from the shared libraries on Windows so the ctypes loader
# can resolve them (MSVC exports nothing by default). No effect on ELF/Mach-O.
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

# ------------------------------------------------------------------ #
# Vendored SAT solver (kissat) — Phase B.0                            #
# ------------------------------------------------------------------ #

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/kissat.cmake)

# MSVC portability for the dv-solve C sources (applies to every target defined
# below — libraries, the CLI and the C tests). ZSP_WIN_COMPAT is set in
# kissat.cmake. Force-includes the builtins/stat shim and puts the POSIX shim
# headers on the include path; kissat handles its own flags above.
if(MSVC)
    add_compile_options(/FI${ZSP_WIN_COMPAT}/msvc_compat.h /wd4244 /wd4267 /wd4146)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS WIN32_LEAN_AND_MEAN)
    include_directories(${ZSP_WIN_COMPAT})
endif()

# The CLI (dv-solve-smt2) and the C unit-test executables are developer tools,
# not part of any wheel. They also pull in POSIX-only APIs (setenv) and internal
# data symbols that aren't exported from the DLLs, so skip them on MSVC.
option(DV_SOLVE_BUILD_TOOLS "Build the dv-solve CLI and C test executables" ON)
if(MSVC)
    set(DV_SOLVE_BUILD_TOOLS OFF)
endif()

if(DV_SOLVE_BUILD_TOOLS)
add_executable(test_kissat_smoke tests/c/test_kissat_smoke.c)
target_link_libraries(test_kissat_smoke PRIVATE kissat)
target_include_directories(test_kissat_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/resources/kissat/src)
endif()
# Registered with CTest below, after enable_testing().

# ------------------------------------------------------------------ #
# Contradiction analysis (optional module)                            #
# ------------------------------------------------------------------ #

option(ZSP_CONTRADICTION_ANALYSIS "Build contradiction analysis module" OFF)

set(ZSP_CONTRA_SOURCES
    src/c/zsp_contradiction.c
)

# ------------------------------------------------------------------ #
# Source list                                                          #
# ------------------------------------------------------------------ #

set(ZSP_SOURCES
    src/c/zsp_block_alloc.c
    src/c/zsp_builder.c
    src/c/zsp_compile.c
    src/c/zsp_conflict.c
    src/c/zsp_ctx.c
    src/c/zsp_nogood.c
    src/c/zsp_pool.c
    src/c/zsp_problem.c
    src/c/zsp_propagate.c
    src/c/zsp_prop_templates.c
    src/c/zsp_search.c
    src/c/zsp_shave.c
    src/c/zsp_stack.c
    src/c/zsp_checkpoint.c
    src/c/zsp_trail.c
    src/c/zsp_validate.c
    src/c/zsp_placement.c
    src/c/zsp_wiremask.c
    src/c/zsp_costguided.c
    src/c/zsp_lcg.c
    src/c/zsp_explain.c
    src/c/zsp_clause_prop.c
    src/c/zsp_sat.c
    src/c/zsp_aig.c
    src/c/zsp_aig_cnf.c
    src/c/zsp_bitblast.c
    src/c/zsp_bbsolver.c
    src/c/zsp_arena.c
    src/c/zsp_bvdom.c
    src/c/zsp_bvbounds.c
)

# ------------------------------------------------------------------ #
# Shared library target                                                #
# ------------------------------------------------------------------ #

add_library(dv_solve SHARED ${ZSP_SOURCES})
target_link_libraries(dv_solve PRIVATE kissat)

if(ZSP_CONTRADICTION_ANALYSIS)
    target_sources(dv_solve PRIVATE ${ZSP_CONTRA_SOURCES})
    target_compile_definitions(dv_solve PRIVATE ZSP_CONTRADICTION_ANALYSIS=1)
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

add_library(dv_solve_debug SHARED ${ZSP_SOURCES} ${ZSP_CONTRA_SOURCES})
target_link_libraries(dv_solve_debug PRIVATE kissat)

target_compile_definitions(dv_solve_debug PRIVATE ZSP_CONTRADICTION_ANALYSIS=1)

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
    src/c/zsp_dpi.c
    ${ZSP_SOURCES}
)
target_link_libraries(dv_solve_dpi PRIVATE kissat)

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

add_executable(test_zsp_sat tests/c/test_zsp_sat.c)
target_link_libraries(test_zsp_sat PRIVATE dv_solve kissat)
target_include_directories(test_zsp_sat PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/c)
add_test(NAME test_zsp_sat COMMAND test_zsp_sat)

add_executable(test_kissat_zsp_alloc tests/c/test_kissat_zsp_alloc.c)
target_link_libraries(test_kissat_zsp_alloc PRIVATE dv_solve kissat)
target_include_directories(test_kissat_zsp_alloc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/c)
add_test(NAME test_kissat_zsp_alloc COMMAND test_kissat_zsp_alloc)

add_executable(test_zsp_sat_incremental tests/c/test_zsp_sat_incremental.c)
target_link_libraries(test_zsp_sat_incremental PRIVATE dv_solve kissat)
target_include_directories(test_zsp_sat_incremental PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/c)
add_test(NAME test_zsp_sat_incremental COMMAND test_zsp_sat_incremental)

add_executable(test_zsp_aig tests/c/test_zsp_aig.c)
target_link_libraries(test_zsp_aig PRIVATE dv_solve)
target_include_directories(test_zsp_aig PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/c)
add_test(NAME test_zsp_aig COMMAND test_zsp_aig)

add_executable(test_zsp_aig_cnf tests/c/test_zsp_aig_cnf.c)
target_link_libraries(test_zsp_aig_cnf PRIVATE dv_solve kissat)
target_include_directories(test_zsp_aig_cnf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/c)
add_test(NAME test_zsp_aig_cnf COMMAND test_zsp_aig_cnf)

add_executable(test_zsp_bitblast tests/c/test_zsp_bitblast.c)
target_link_libraries(test_zsp_bitblast PRIVATE dv_solve kissat)
target_include_directories(test_zsp_bitblast PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/c)
add_test(NAME test_zsp_bitblast COMMAND test_zsp_bitblast)

add_executable(test_zsp_bbsolver tests/c/test_zsp_bbsolver.c)
target_link_libraries(test_zsp_bbsolver PRIVATE dv_solve kissat)
target_include_directories(test_zsp_bbsolver PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/c)
add_test(NAME test_zsp_bbsolver COMMAND test_zsp_bbsolver)

add_executable(test_zsp_block_alloc_cache tests/c/test_zsp_block_alloc_cache.c)
target_link_libraries(test_zsp_block_alloc_cache PRIVATE dv_solve)
target_include_directories(test_zsp_block_alloc_cache PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/c)
add_test(NAME test_zsp_block_alloc_cache COMMAND test_zsp_block_alloc_cache)

add_executable(test_zsp_arena tests/c/test_zsp_arena.c)
target_link_libraries(test_zsp_arena PRIVATE dv_solve)
target_include_directories(test_zsp_arena PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/c)
add_test(NAME test_zsp_arena COMMAND test_zsp_arena)

add_executable(test_zsp_bvdom tests/c/test_zsp_bvdom.c)
target_link_libraries(test_zsp_bvdom PRIVATE dv_solve)
target_include_directories(test_zsp_bvdom PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/c)
add_test(NAME test_zsp_bvdom COMMAND test_zsp_bvdom)

add_executable(test_zsp_bvbounds tests/c/test_zsp_bvbounds.c)
target_link_libraries(test_zsp_bvbounds PRIVATE dv_solve)
target_include_directories(test_zsp_bvbounds PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/c)
add_test(NAME test_zsp_bvbounds COMMAND test_zsp_bvbounds)

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

# ------------------------------------------------------------------ #
# dv-solve-smt2 executable                                            #
# ------------------------------------------------------------------ #

set(SMT2_SOURCES
    src/c/smt2/smt2_lexer.c
    src/c/smt2/smt2_parser.c
    src/c/smt2/smt2_frontend.c
    src/c/smt2/smt2_main.c
)

add_executable(dv-solve-smt2 ${SMT2_SOURCES})
target_link_libraries(dv-solve-smt2 PRIVATE dv_solve)
target_include_directories(dv-solve-smt2 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/c
)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(dv-solve-smt2 PRIVATE -fsanitize=address -g -O0)
    target_link_options(dv-solve-smt2 PRIVATE -fsanitize=address)
endif()
install(TARGETS dv-solve-smt2 RUNTIME DESTINATION bin)

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
