# Build kissat as a static library directly from its sources.
#
# Phase B.0 — Phase B.1 transition: kissat source was originally vendored
# at resources/kissat/ and built from there. Phase B.1 forks it into
# src/c/sat/kissat/ — see docs/phase_b1_plan.md. The vendored snapshot
# at resources/kissat/ is preserved as the upstream baseline so we can
# diff our changes against it.

set(KISSAT_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src/c/sat/kissat)
set(KISSAT_SRC ${KISSAT_SRC_ROOT}/src)

# Read VERSION file
file(READ ${KISSAT_SRC_ROOT}/VERSION KISSAT_VERSION)
string(STRIP "${KISSAT_VERSION}" KISSAT_VERSION)

# Generate build.h with the fields the upstream generate-build-header.sh emits.
# We intentionally substitute placeholder values for ID/BUILD/DIR so the build
# is reproducible and does not depend on a working git tree.
set(KISSAT_BUILD_H ${CMAKE_CURRENT_BINARY_DIR}/kissat_gen/build.h)
file(WRITE ${KISSAT_BUILD_H}
"#define VERSION \"${KISSAT_VERSION}\"\n"
"#define COMPILER \"${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}\"\n"
"#define ID \"dv-solve-forked\"\n"
"#define BUILD \"dv-solve\"\n"
"#define DIR \"${KISSAT_SRC_ROOT}\"\n"
)

# Library sources: every src/*.c except the four application-only files and
# main.c (see makefile.in: APPSRC + main.c are excluded from LIBSRC).
file(GLOB KISSAT_ALL_SRCS ${KISSAT_SRC}/*.c)
set(KISSAT_EXCLUDED
    ${KISSAT_SRC}/main.c
    ${KISSAT_SRC}/application.c
    ${KISSAT_SRC}/handle.c
    ${KISSAT_SRC}/parse.c
    ${KISSAT_SRC}/witness.c
)
list(REMOVE_ITEM KISSAT_ALL_SRCS ${KISSAT_EXCLUDED})

add_library(kissat STATIC ${KISSAT_ALL_SRCS})

target_include_directories(kissat PUBLIC ${KISSAT_SRC})
# build.h is included as "build.h"; expose its directory privately to the
# kissat sources so other code can't accidentally pick it up.
target_include_directories(kissat PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/kissat_gen)
# dv-solve fork: kissat sources can #include "zsp_alloc.h" for the optional
# allocator routing. The include must be PRIVATE — upstream Kissat must not
# acquire dv-solve as a transitive dependency.
target_include_directories(kissat PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/c)

# Compile flags mirroring kissat's default release configuration:
#   NDEBUG    - production assertions off
#   QUIET     - omit verbose output
#   NPROOFS   - omit DRAT proof tracing
# We deliberately do NOT define COMPACT yet; that's a future tuning knob.
target_compile_definitions(kissat PRIVATE NDEBUG QUIET NPROOFS)

# Kissat is C99 with GNU extensions in places; -std=c99 matches their default.
target_compile_options(kissat PRIVATE
    -std=c99
    -W -Wall
    -O3
    -fPIC
    -Wno-unused-parameter
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-unused-but-set-variable
)

set_target_properties(kissat PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Kissat uses sqrt/log10 — link libm publicly so consumers pick it up too.
target_link_libraries(kissat PUBLIC m)
