# Build CaDiCaL as a static library for use as the optional incremental SAT
# backend (see docs/cadical_integration_plan.md).
#
# CaDiCaL is C++ and pulls a libstdc++ dependency into any target that links it
# — which is why it is gated behind ZSP_WITH_CADICAL and excluded from the
# lightweight, pure-C embeddable build. kissat stays the one-shot / embeddable
# backend.
#
# Source comes from <packages>/cadical (fetched by `ivpm update`, pinned to
# rel-3.0.0 in ivpm.yaml). The packages directory is only ./packages when
# dv-solve is the root project; as a dependency inside someone else's
# workspace it is the *sibling* directory holding dv-solve. See the
# CADICAL_SRC_ROOT resolution below. We compile the library sources with
# CaDiCaL's -DNBUILD escape hatch (no ./configure / generated build.hpp;
# version.cpp falls back to VERSION "3.0.0").
#
# SYMBOL COLLISION: both kissat and CaDiCaL vendor Armin Biere's "kitten"
# sub-solver, so libkissat.a and libcadical.a both define kitten_*/citten_* (and
# other internals). Linking both into one shared object is a multiple-definition
# error. We resolve it by partial-linking all CaDiCaL objects into ONE
# relocatable object and then localizing every symbol except the ccadical_* C
# API (the only surface dv-solve uses). After the merge, CaDiCaL's internal
# cross-references still resolve (same object), while its globals — kitten
# included — become file-local and cannot collide with kissat. This also
# future-proofs against any other shared-internal-name clashes.

# PACKAGES_DIR is resolved in the top-level CMakeLists; -DCADICAL_SRC_ROOT
# overrides it for an out-of-workspace CaDiCaL checkout.
set(CADICAL_SRC_ROOT ${PACKAGES_DIR}/cadical
    CACHE PATH "Root of the CaDiCaL source tree (default: <PACKAGES_DIR>/cadical)")
set(CADICAL_SRC ${CADICAL_SRC_ROOT}/src)

if(NOT EXISTS ${CADICAL_SRC}/ccadical.cpp)
    message(FATAL_ERROR
        "CaDiCaL sources not found at ${CADICAL_SRC}. "
        "Run `ivpm update` to fetch them, or set -DCADICAL_SRC_ROOT=<path> "
        "(or -DPACKAGES_DIR=<ivpm packages dir>), "
        "or configure with -DZSP_WITH_CADICAL=OFF for the pure-C build.")
endif()

if(MSVC)
    message(FATAL_ERROR "cadical.cmake: MSVC path not implemented "
        "(ZSP_WITH_CADICAL should be OFF on MSVC).")
endif()

# Library sources: every src/*.cpp plus src/*.c (kitten.c = embedded
# kitten/citten sub-solver), minus the two application mains.
file(GLOB CADICAL_ALL_SRCS ${CADICAL_SRC}/*.cpp ${CADICAL_SRC}/*.c)
list(REMOVE_ITEM CADICAL_ALL_SRCS
    ${CADICAL_SRC}/cadical.cpp
    ${CADICAL_SRC}/mobical.cpp
)

# 1. Compile CaDiCaL sources to objects (no archive yet).
add_library(cadical_objs OBJECT ${CADICAL_ALL_SRCS})
target_include_directories(cadical_objs PUBLIC ${CADICAL_SRC})
target_compile_features(cadical_objs PRIVATE cxx_std_17)
set_target_properties(cadical_objs PROPERTIES POSITION_INDEPENDENT_CODE ON)
#   NBUILD - skip generated build.hpp; NDEBUG - assertions off
target_compile_definitions(cadical_objs PRIVATE NBUILD NDEBUG)
target_compile_options(cadical_objs PRIVATE -O3 -fPIC -Wno-unused-parameter)

# 2. Partial-link all objects into one relocatable object, then localize every
#    defined global except the ccadical_* API (-w enables the wildcard).
set(CADICAL_MERGED  ${CMAKE_CURRENT_BINARY_DIR}/cadical_merged.o)
set(CADICAL_LOCALIZED ${CMAKE_CURRENT_BINARY_DIR}/cadical_localized.o)
add_custom_command(
    OUTPUT ${CADICAL_LOCALIZED}
    COMMAND ${CMAKE_LINKER} -r $<TARGET_OBJECTS:cadical_objs> -o ${CADICAL_MERGED}
    COMMAND ${CMAKE_OBJCOPY} -w --keep-global-symbol=ccadical_*
            ${CADICAL_MERGED} ${CADICAL_LOCALIZED}
    DEPENDS cadical_objs
    COMMAND_EXPAND_LISTS
    VERBATIM
    COMMENT "Merging + localizing CaDiCaL (keeps only ccadical_* global)")
add_custom_target(cadical_localize DEPENDS ${CADICAL_LOCALIZED})

# 3. Wrap the localized object as the linkable static library.
set_source_files_properties(${CADICAL_LOCALIZED} PROPERTIES
    EXTERNAL_OBJECT TRUE GENERATED TRUE)
add_library(cadical STATIC ${CADICAL_LOCALIZED})
add_dependencies(cadical cadical_localize)
set_target_properties(cadical PROPERTIES LINKER_LANGUAGE CXX)
# ccadical.h (the C API dv-solve calls) lives in src/.
target_include_directories(cadical PUBLIC ${CADICAL_SRC})
