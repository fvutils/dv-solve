# Makes every defined global symbol of an object or archive local, except the
# ones to keep and any vague-linkage (weak or GNU unique) definition.
#
#   cmake -DNM=<nm> -DOBJCOPY=<objcopy> -DIN=<file> [-DOUT=<file>]
#         [-DKEEP=<pattern,...>] [-DKEEP_FILE=<file>] -P dvs_localize.cmake
#
# Vague-linkage symbols (inline functions, template instantiations, their
# static locals and guard variables) live in COMDAT groups. A program that
# also defines one keeps its own group and discards ours, so our references
# must resolve to its copy by name: localized, they would point into the
# discarded section ("defined in discarded section"). They are left global,
# and with the same visibility the compiler gave them.
#
# KEEP patterns use objcopy's -w wildcards. OUT defaults to IN (in place).

if(NOT OUT)
    set(OUT ${IN})
endif()
execute_process(COMMAND ${NM} --defined-only -P ${IN}
    OUTPUT_VARIABLE _nm RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "${NM} failed on ${IN}")
endif()

string(REPLACE "," ";" _keep "${KEEP}")
if(KEEP_FILE)
    file(STRINGS ${KEEP_FILE} _file_keep)
    list(APPEND _keep ${_file_keep})
endif()
# nm -P prints "name type value size"; W/V are weak, u is GNU unique.
string(REGEX MATCHALL "[^\n ]+ [WVu] " _vague "${_nm}")
foreach(_s IN LISTS _vague)
    string(REGEX REPLACE " [WVu] $" "" _s "${_s}")
    list(APPEND _keep ${_s})
endforeach()
list(REMOVE_DUPLICATES _keep)
list(JOIN _keep "\n" _keep)
file(WRITE ${OUT}.keep "${_keep}\n")

if(OUT STREQUAL IN)
    set(_files ${IN})
else()
    set(_files ${IN} ${OUT})
endif()
execute_process(COMMAND ${OBJCOPY} -w --keep-global-symbols=${OUT}.keep ${_files}
    RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "${OBJCOPY} failed on ${IN}")
endif()
