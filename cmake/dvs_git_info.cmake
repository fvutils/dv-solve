# Writes ${OUT}: the commit and dirty state of the source tree at build time,
# for dvs_build_info(). Run on every build (add_custom_target); the file is
# rewritten only when its content changes, so an unchanged tree rebuilds
# nothing.
execute_process(COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY ${SRC} OUTPUT_VARIABLE _commit
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0 OR _commit STREQUAL "")
    set(_commit "unknown")
endif()
# Tracked files only: build directories and notes in the tree are untracked.
execute_process(COMMAND git status --porcelain --untracked-files=no
    WORKING_DIRECTORY ${SRC} OUTPUT_VARIABLE _status
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(_status STREQUAL "")
    set(_dirty 0)
else()
    set(_dirty 1)
endif()
set(_text "#define DVS_BI_COMMIT \"${_commit}\"\n#define DVS_BI_DIRTY ${_dirty}\n")
if(EXISTS ${OUT})
    file(READ ${OUT} _old)
else()
    set(_old "")
endif()
if(NOT _old STREQUAL _text)
    file(WRITE ${OUT} "${_text}")
endif()
