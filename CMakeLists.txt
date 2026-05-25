cmake_minimum_required(VERSION 3.14)
project(dv_solve C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ------------------------------------------------------------------ #
# Vendored SAT solver (kissat) — Phase B.0                            #
# ------------------------------------------------------------------ #

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/kissat.cmake)

add_executable(test_kissat_smoke tests/c/test_kissat_smoke.c)
target_link_libraries(test_kissat_smoke PRIVATE kissat)
target_include_directories(test_kissat_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/resources/kissat/src)
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
    LIBRARY DESTINATION lib
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
)

# ------------------------------------------------------------------ #
# SMT-LIB2 lexer test                                                 #
# ------------------------------------------------------------------ #

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
