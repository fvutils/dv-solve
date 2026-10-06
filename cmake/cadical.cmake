# Build CaDiCaL as a static library for use as the optional incremental SAT
# backend (see docs/cadical_integration_plan.md).
#
# CaDiCaL is C++ and pulls a libstdc++ dependency into any target that links it
# — which is why it is gated behind DVS_WITH_CADICAL and excluded from the
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
        "or configure with -DDVS_WITH_CADICAL=OFF for the pure-C build.")
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

# NBUILD also skips CaDiCaL's ./configure feature probes, so replicate the one
# that matters for portability: closefrom() exists only in glibc >= 2.34 (not
# manylinux_2_28) and is absent on macOS. Without it, file.cpp needs
# -DNCLOSEFROM to use its own close() loop instead.
if(MSVC)
    # CaDiCaL targets MinGW on Windows, not MSVC: its own _WIN32 branches cover
    # the process and signal code, but it still includes <unistd.h>,
    # <sys/time.h> and <sys/resource.h> and uses GCC builtins unguarded. The
    # same shims that build kissat supply those (cmake/kissat.cmake). The
    # rename header replaces the ELF/Mach-O symbol localization below; see it.
    target_include_directories(cadical_objs PRIVATE ${DVS_WIN_COMPAT})
    target_compile_options(cadical_objs PRIVATE
        /O2 /EHsc
        /FI${DVS_WIN_COMPAT}/msvc_compat.h
        /FI${CMAKE_CURRENT_LIST_DIR}/cadical_msvc_rename.h
        /wd4244 /wd4267 /wd4146 /wd4996)
    # NOMINMAX: resources.cpp includes <windows.h>, whose min/max macros break
    # std::min/std::max. NCLOSEFROM: there is no closefrom().
    target_compile_definitions(cadical_objs PRIVATE
        NCLOSEFROM NOMINMAX WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)

    add_library(cadical STATIC $<TARGET_OBJECTS:cadical_objs>)
    set_target_properties(cadical PROPERTIES LINKER_LANGUAGE CXX)
    target_include_directories(cadical PUBLIC ${CADICAL_SRC})
    # The shims' getrusage/sysconf/gettimeofday are implemented in kissat's
    # win_compat.c, and resources.cpp's Win32 branch needs psapi.
    target_link_libraries(cadical PUBLIC kissat psapi)

    # A C name the rename header misses would not necessarily fail the link:
    # CaDiCaL's references could bind to kissat's definition instead. List the
    # library's external symbols and fail on any C name left un-renamed.
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
        add_custom_command(TARGET cadical POST_BUILD
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_LIST_DIR}/check_cadical_symbols.py
                    $<TARGET_FILE:cadical>
            VERBATIM)
    else()
        message(WARNING "Python not found: CaDiCaL symbol check skipped")
    endif()
    return()
endif()

include(CheckCXXSourceCompiles)
check_cxx_source_compiles("
extern \"C\" {
#include <unistd.h>
}
int main () { closefrom (3); return 0; }
" DVS_CADICAL_HAVE_CLOSEFROM)
if(NOT DVS_CADICAL_HAVE_CLOSEFROM)
    target_compile_definitions(cadical_objs PRIVATE NCLOSEFROM)
endif()
target_compile_options(cadical_objs PRIVATE -O3 -fPIC -Wno-unused-parameter)

# 2. Partial-link all objects into one relocatable object, then localize every
#    defined global except the ccadical_* API (-w enables the wildcard).
set(CADICAL_MERGED  ${CMAKE_CURRENT_BINARY_DIR}/cadical_merged.o)
set(CADICAL_LOCALIZED ${CMAKE_CURRENT_BINARY_DIR}/cadical_localized.o)
if(APPLE)
    # Mach-O: there is no objcopy, but ld64 does both steps at once. With -r,
    # symbols left out of -exported_symbol become private extern and are then
    # turned static (unless -keep_private_externs). Mach-O names carry a
    # leading underscore.
    #
    # ld64 must be told -arch, and links one architecture per invocation. A
    # universal build (CMAKE_OSX_ARCHITECTURES="x86_64;arm64", as the wheel
    # backend configures) compiles fat objects, so link each slice on its own
    # and lipo the results back together.
    set(_cadical_archs ${CMAKE_OSX_ARCHITECTURES})
    if(NOT _cadical_archs)
        set(_cadical_archs ${CMAKE_SYSTEM_PROCESSOR})
    endif()
    set(_cadical_cmds)
    set(_cadical_slices)
    foreach(_arch IN LISTS _cadical_archs)
        set(_slice ${CMAKE_CURRENT_BINARY_DIR}/cadical_localized_${_arch}.o)
        list(APPEND _cadical_cmds
            COMMAND ${CMAKE_LINKER} -r -arch ${_arch} $<TARGET_OBJECTS:cadical_objs>
                    -exported_symbol _ccadical_* -o ${_slice})
        list(APPEND _cadical_slices ${_slice})
    endforeach()
    add_custom_command(
        OUTPUT ${CADICAL_LOCALIZED}
        ${_cadical_cmds}
        COMMAND lipo -create ${_cadical_slices} -output ${CADICAL_LOCALIZED}
        DEPENDS cadical_objs
        COMMAND_EXPAND_LISTS
        VERBATIM
        COMMENT "Merging + localizing CaDiCaL (keeps only ccadical_* global)")
else()
    add_custom_command(
        OUTPUT ${CADICAL_LOCALIZED}
        COMMAND ${CMAKE_LINKER} -r $<TARGET_OBJECTS:cadical_objs> -o ${CADICAL_MERGED}
        COMMAND ${CMAKE_OBJCOPY} -w --keep-global-symbol=ccadical_*
                ${CADICAL_MERGED} ${CADICAL_LOCALIZED}
        DEPENDS cadical_objs
        COMMAND_EXPAND_LISTS
        VERBATIM
        COMMENT "Merging + localizing CaDiCaL (keeps only ccadical_* global)")
endif()
add_custom_target(cadical_localize DEPENDS ${CADICAL_LOCALIZED})

# 3. Wrap the localized object as the linkable static library.
set_source_files_properties(${CADICAL_LOCALIZED} PROPERTIES
    EXTERNAL_OBJECT TRUE GENERATED TRUE)
add_library(cadical STATIC ${CADICAL_LOCALIZED})
add_dependencies(cadical cadical_localize)
set_target_properties(cadical PROPERTIES LINKER_LANGUAGE CXX)
# ccadical.h (the C API dv-solve calls) lives in src/.
target_include_directories(cadical PUBLIC ${CADICAL_SRC})
