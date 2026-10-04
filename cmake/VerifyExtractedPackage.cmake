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
# The packaged project: ZENITH_PACKAGE_PROJECT_NAME, passed with --project,
# or the one .zenith-project at the archive root, which the game finds itself
# (as when the packaged game is started without arguments).
if(DEFINED ZENITH_PACKAGE_PROJECT_NAME AND NOT ZENITH_PACKAGE_PROJECT_NAME STREQUAL "")
    set(project_arguments --project "${package_root}/${ZENITH_PACKAGE_PROJECT_NAME}")
else()
    file(GLOB project_files "${package_root}/*.zenith-project")
    list(LENGTH project_files project_count)
    if(NOT project_count EQUAL 1)
        message(FATAL_ERROR "Expected one .zenith-project at the package root, found ${project_count}; "
            "pass ZENITH_PACKAGE_PROJECT_NAME")
    endif()
    set(project_arguments "")
endif()
# Loads the project's game module and every packaged scene without a GPU.
execute_process(
    COMMAND "${game}" --verify-package ${project_arguments}
    WORKING_DIRECTORY "${ZENITH_VERIFY_ROOT}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Extracted package verification failed (${result}):\n${output}\n${error}")
endif()
if(NOT output MATCHES "Verified scene ")
    message(FATAL_ERROR "Extracted package verification checked no project scene:\n${output}")
endif()
message(STATUS "${output}")
