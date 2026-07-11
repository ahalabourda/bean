set(counter_file "${BEAN_BUILD_COUNTER_FILE}")
set(header_file "${BEAN_BUILD_HEADER_FILE}")

if(counter_file STREQUAL "" OR header_file STREQUAL "")
    message(FATAL_ERROR "BEAN_BUILD_COUNTER_FILE and BEAN_BUILD_HEADER_FILE must be provided.")
endif()

get_filename_component(header_dir "${header_file}" DIRECTORY)
file(MAKE_DIRECTORY "${header_dir}")

set(current_build 0)
if(EXISTS "${counter_file}")
    file(READ "${counter_file}" counter_raw)
    string(STRIP "${counter_raw}" counter_raw)
    if(counter_raw MATCHES "^[0-9]+$")
        set(current_build "${counter_raw}")
    endif()
endif()

math(EXPR next_build "${current_build} + 1")

file(WRITE "${counter_file}" "${next_build}\n")
file(WRITE "${header_file}" "#pragma once\n\n#define BEAN_BUILD_NUMBER ${next_build}\n")
