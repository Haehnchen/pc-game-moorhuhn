# Usage: cmake -DCLANG_FORMAT=<path> -DSOURCE_DIR=<repo> [-DFIX=ON] -P cmake/FormatCheck.cmake
# Covers production code and tests so both stay on the same .clang-format style.
if(NOT CLANG_FORMAT OR NOT SOURCE_DIR)
    message(FATAL_ERROR "CLANG_FORMAT and SOURCE_DIR are required")
endif()

file(GLOB_RECURSE files
    "${SOURCE_DIR}/src/*.cpp" "${SOURCE_DIR}/src/*.hpp"
    "${SOURCE_DIR}/tests/*.cpp" "${SOURCE_DIR}/tests/*.hpp")
list(SORT files)

if(FIX)
    set(mode -i)
else()
    set(mode --dry-run --Werror)
endif()

execute_process(COMMAND "${CLANG_FORMAT}" ${mode} ${files} RESULT_VARIABLE result)

if(NOT result EQUAL 0)
    message(FATAL_ERROR "clang-format found unformatted files; run 'make format'")
endif()
