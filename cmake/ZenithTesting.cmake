include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/CompilerSettings.cmake")

# Every test gets a temporary directory inside its own build tree. Tests name
# their temporary paths uniquely within one suite, but another build tree of
# this repository (a worktree, a second checkout) runs the same tests with the
# same names: two zenith_prefabs runs sharing %TEMP% corrupted each other's
# prefab files and hit entt assertions, and editors in two trees deleted each
# other's game module shadow copies. TMP and TEMP feed
# std::filesystem::temp_directory_path on Windows, TMPDIR elsewhere. The
# zenith_test_temp_directory test (the zenith_test_temp fixture) creates it.
set(ZENITH_TEST_TEMP_DIR "${PROJECT_BINARY_DIR}/test-temp")

function(_zenith_apply_test_temp_directory)
    get_property(directory_tests DIRECTORY PROPERTY TESTS)
    list(REMOVE_ITEM directory_tests zenith_test_temp_directory)
    if(NOT directory_tests)
        return()
    endif()
    file(TO_NATIVE_PATH "${ZENITH_TEST_TEMP_DIR}" test_temp_dir_native)
    set_property(TEST ${directory_tests} APPEND PROPERTY FIXTURES_REQUIRED zenith_test_temp)
    set_property(TEST ${directory_tests} APPEND PROPERTY ENVIRONMENT_MODIFICATION
        "TMP=set:${test_temp_dir_native}" "TEMP=set:${test_temp_dir_native}"
        "TMPDIR=set:${ZENITH_TEST_TEMP_DIR}")
endfunction()

# Gives every test of the calling directory the build tree's temporary
# directory, once that directory's CMakeLists.txt finishes (so it covers tests
# declared with plain add_test too). zenith_add_test calls it; a directory
# that declares tests only with add_test calls it itself.
function(zenith_use_test_temp_directory)
    get_property(already_scheduled DIRECTORY PROPERTY ZENITH_TEST_TEMP_DIRECTORY_SCHEDULED)
    if(NOT already_scheduled)
        set_property(DIRECTORY PROPERTY ZENITH_TEST_TEMP_DIRECTORY_SCHEDULED ON)
        cmake_language(DEFER CALL _zenith_apply_test_temp_directory)
    endif()
endfunction()

set(_ZENITH_TEST_TARGET_KEYWORDS SOURCES LIBS DEFINITIONS INCLUDE_DIRECTORIES DEPENDS)

function(_zenith_configure_test_target target)
    cmake_parse_arguments(PARSE_ARGV 1 TARGET "NEEDS_ASSETS" "" "${_ZENITH_TEST_TARGET_KEYWORDS}")
    if(TARGET_LIBS)
        target_link_libraries(${target} PRIVATE ${TARGET_LIBS})
    endif()
    if(TARGET_INCLUDE_DIRECTORIES)
        target_include_directories(${target} PRIVATE ${TARGET_INCLUDE_DIRECTORIES})
    endif()
    if(TARGET_NEEDS_ASSETS)
        target_compile_definitions(${target} PRIVATE ZENITH_ASSET_DIR="${ZENITH_ASSET_BINARY_DIR}")
        add_dependencies(${target} zenith_assets)
    endif()
    if(TARGET_DEFINITIONS)
        target_compile_definitions(${target} PRIVATE ${TARGET_DEFINITIONS})
    endif()
    if(TARGET_DEPENDS)
        add_dependencies(${target} ${TARGET_DEPENDS})
    endif()
    zenith_target_warnings(${target})
endfunction()

# zenith_add_test_executable(<target> SOURCES <file>...
#     [LIBS <library>...] [DEFINITIONS <definition>...]
#     [INCLUDE_DIRECTORIES <dir>...] [DEPENDS <target>...] [NEEDS_ASSETS])
#
# An executable that tests run (or that a test drives) with the project's
# warning level. LIBS link privately. NEEDS_ASSETS defines ZENITH_ASSET_DIR as
# the build tree's assets/ copy and builds zenith_assets first.
function(zenith_add_test_executable target)
    cmake_parse_arguments(PARSE_ARGV 1 TARGET "NEEDS_ASSETS" "" "${_ZENITH_TEST_TARGET_KEYWORDS}")
    if(NOT TARGET_SOURCES)
        message(FATAL_ERROR "zenith_add_test_executable(${target}) needs SOURCES")
    endif()
    add_executable(${target} ${TARGET_SOURCES})
    _zenith_configure_test_target(${target} ${ARGN})
endfunction()

# zenith_add_test_module(<target> SOURCES <file>... [same options as above])
#
# A game module (a MODULE library) that tests load at run time.
function(zenith_add_test_module target)
    cmake_parse_arguments(PARSE_ARGV 1 TARGET "NEEDS_ASSETS" "" "${_ZENITH_TEST_TARGET_KEYWORDS}")
    if(NOT TARGET_SOURCES)
        message(FATAL_ERROR "zenith_add_test_module(${target}) needs SOURCES")
    endif()
    add_library(${target} MODULE ${TARGET_SOURCES})
    _zenith_configure_test_target(${target} ${ARGN})
endfunction()

# zenith_add_test(<name>
#     [EXECUTABLE <target or path>] [COMMAND_ARGS <arg>...]
#     [SOURCES <file>...] [LIBS <library>...] [DEFINITIONS <definition>...]
#     [INCLUDE_DIRECTORIES <dir>...] [DEPENDS <target>...] [NEEDS_ASSETS]
#     [LABELS <label>...] [ENVIRONMENT <VAR=value>...] [TIMEOUT <seconds>]
#     [SKIP_RETURN_CODE <code>] [PROCESSORS <count>]
#     [FIXTURES_SETUP <fixture>...] [FIXTURES_REQUIRED <fixture>...]
#     [WORKING_DIRECTORY <dir>])
#
# Adds the CTest <name>, running EXECUTABLE (default <name>_tests) with
# COMMAND_ARGS from WORKING_DIRECTORY (default: the top of the build tree,
# wherever the test is declared). With SOURCES the executable is created here
# as by zenith_add_test_executable; without them EXECUTABLE must name an
# existing target or a program, and the target options are rejected. The
# remaining options set the test properties of the same names.
function(zenith_add_test name)
    cmake_parse_arguments(PARSE_ARGV 1 TEST "NEEDS_ASSETS" "EXECUTABLE;WORKING_DIRECTORY;TIMEOUT;SKIP_RETURN_CODE;PROCESSORS"
        "${_ZENITH_TEST_TARGET_KEYWORDS};COMMAND_ARGS;LABELS;ENVIRONMENT;FIXTURES_SETUP;FIXTURES_REQUIRED")
    if(TEST_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "zenith_add_test(${name}) got unknown arguments: ${TEST_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT TEST_EXECUTABLE)
        set(TEST_EXECUTABLE ${name}_tests)
    endif()

    if(TEST_SOURCES)
        set(target_arguments)
        if(TEST_NEEDS_ASSETS)
            list(APPEND target_arguments NEEDS_ASSETS)
        endif()
        foreach(keyword IN LISTS _ZENITH_TEST_TARGET_KEYWORDS)
            if(TEST_${keyword})
                list(APPEND target_arguments ${keyword} ${TEST_${keyword}})
            endif()
        endforeach()
        zenith_add_test_executable(${TEST_EXECUTABLE} ${target_arguments})
    elseif(TEST_NEEDS_ASSETS OR TEST_LIBS OR TEST_DEFINITIONS OR TEST_INCLUDE_DIRECTORIES OR TEST_DEPENDS)
        message(FATAL_ERROR "zenith_add_test(${name}) sets executable options without SOURCES")
    endif()

    # Every test runs from the top of the build tree, wherever it is declared.
    if(NOT TEST_WORKING_DIRECTORY)
        set(TEST_WORKING_DIRECTORY "${PROJECT_BINARY_DIR}")
    endif()
    add_test(NAME ${name} COMMAND ${TEST_EXECUTABLE} ${TEST_COMMAND_ARGS} WORKING_DIRECTORY "${TEST_WORKING_DIRECTORY}")

    foreach(keyword IN ITEMS ENVIRONMENT LABELS TIMEOUT SKIP_RETURN_CODE PROCESSORS FIXTURES_SETUP FIXTURES_REQUIRED)
        if(DEFINED TEST_${keyword})
            set_tests_properties(${name} PROPERTIES ${keyword} "${TEST_${keyword}}")
        endif()
    endforeach()
    zenith_use_test_temp_directory()
endfunction()
