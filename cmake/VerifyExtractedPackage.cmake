cmake_policy(VERSION 3.24)

if(NOT DEFINED ZENITH_ARCHIVE OR NOT EXISTS "${ZENITH_ARCHIVE}")
    message(FATAL_ERROR "Pass an existing archive as ZENITH_ARCHIVE")
endif()
if(NOT DEFINED ZENITH_VERIFY_ROOT)
    set(ZENITH_VERIFY_ROOT "${CMAKE_CURRENT_BINARY_DIR}/package verification with spaces")
endif()
file(REMOVE_RECURSE "${ZENITH_VERIFY_ROOT}")
file(MAKE_DIRECTORY "${ZENITH_VERIFY_ROOT}")
file(ARCHIVE_EXTRACT INPUT "${ZENITH_ARCHIVE}" DESTINATION "${ZENITH_VERIFY_ROOT}")
file(GLOB extracted_children LIST_DIRECTORIES true "${ZENITH_VERIFY_ROOT}/*")
list(LENGTH extracted_children child_count)
if(child_count EQUAL 1 AND IS_DIRECTORY "${extracted_children}")
    set(package_root "${extracted_children}")
else()
    set(package_root "${ZENITH_VERIFY_ROOT}")
endif()
if(WIN32)
    set(game "${package_root}/zenith.exe")
else()
    set(game "${package_root}/zenith")
endif()
execute_process(
    COMMAND "${game}" --verify-package --project "${package_root}/demo.zenith-project"
    WORKING_DIRECTORY "${ZENITH_VERIFY_ROOT}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Extracted package verification failed (${result}):\n${output}\n${error}")
endif()
message(STATUS "${output}")
