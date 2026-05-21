cmake_minimum_required(VERSION 3.14)
project(dv_solve C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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
    src/c/zsp_placement.c
    src/c/zsp_wiremask.c
    src/c/zsp_costguided.c
    src/c/zsp_lcg.c
    src/c/zsp_explain.c
    src/c/zsp_clause_prop.c
)

# ------------------------------------------------------------------ #
# Shared library target                                                #
# ------------------------------------------------------------------ #

add_library(zsp_solver SHARED ${ZSP_SOURCES})

if(ZSP_CONTRADICTION_ANALYSIS)
    target_sources(zsp_solver PRIVATE ${ZSP_CONTRA_SOURCES})
    target_compile_definitions(zsp_solver PRIVATE ZSP_CONTRADICTION_ANALYSIS=1)
endif()

target_include_directories(zsp_solver PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/c>
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(zsp_solver PRIVATE -fsanitize=address -g -O0)
    target_link_options(zsp_solver PRIVATE -fsanitize=address)
endif()

# ------------------------------------------------------------------ #
# Installation                                                         #
# ------------------------------------------------------------------ #

install(TARGETS zsp_solver
    LIBRARY DESTINATION lib
)

# ------------------------------------------------------------------ #
# Debug library (always includes contradiction analysis)              #
# ------------------------------------------------------------------ #

add_library(zsp_solver_debug SHARED ${ZSP_SOURCES} ${ZSP_CONTRA_SOURCES})

target_compile_definitions(zsp_solver_debug PRIVATE ZSP_CONTRADICTION_ANALYSIS=1)

target_include_directories(zsp_solver_debug PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/c>
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(zsp_solver_debug PRIVATE -fsanitize=address -g -O0)
    target_link_options(zsp_solver_debug PRIVATE -fsanitize=address)
endif()

install(TARGETS zsp_solver_debug
    LIBRARY DESTINATION lib
)

# ------------------------------------------------------------------ #
# DPI shared library target                                            #
# ------------------------------------------------------------------ #

add_library(zsp_solver_dpi SHARED
    src/c/zsp_dpi.c
    ${ZSP_SOURCES}
)

target_include_directories(zsp_solver_dpi PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/c>
)

target_compile_definitions(zsp_solver_dpi PRIVATE)

install(TARGETS zsp_solver_dpi
    LIBRARY DESTINATION lib
)
