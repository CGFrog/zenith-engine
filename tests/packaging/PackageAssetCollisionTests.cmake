# The configure-time check that keeps a packaged project's assets from silently
# replacing engine runtime assets in the package's merged assets/ folder
# (cmake/PackageProject.cmake). Run with:
#   cmake -DZENITH_TEST_ROOT=<scratch folder> -P PackageAssetCollisionTests.cmake
# With -DZENITH_COLLISION_CHECK=<overrides> it instead runs the check on the
# scratch fixture, so the outer run can observe a failing configure.

cmake_policy(VERSION 3.24)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/PackageProject.cmake")

if(NOT DEFINED ZENITH_TEST_ROOT)
    message(FATAL_ERROR "Pass ZENITH_TEST_ROOT")
endif()
set(engine_dir "${ZENITH_TEST_ROOT}/engine assets")
set(project_dir "${ZENITH_TEST_ROOT}/project assets")
set(engine_assets "fonts/ui.ttf;primitives/cube.gltf")

if(DEFINED ZENITH_COLLISION_CHECK)
    zenith_check_package_asset_collisions("${engine_dir}" "${engine_assets}" "${project_dir}"
        "${ZENITH_COLLISION_CHECK}")
    message(STATUS "collision check passed")
    return()
endif()

set(failures 0)
# condition is if() code, evaluated in this scope.
macro(expect condition message)
    cmake_language(EVAL CODE "if(${condition})
        set(expectation_held TRUE)
    else()
        set(expectation_held FALSE)
    endif()")
    if(NOT expectation_held)
        message(SEND_ERROR "FAILED: ${message}")
        math(EXPR failures "${failures} + 1")
    endif()
endmacro()

file(REMOVE_RECURSE "${ZENITH_TEST_ROOT}")
foreach(file IN ITEMS "fonts/ui.ttf" "primitives/cube.gltf" "basic_map.gltf")
    file(WRITE "${engine_dir}/${file}" "engine")
endforeach()
foreach(file IN ITEMS "Primitives/Cube.gltf" "levels/room.gltf" "basic_map.gltf")
    file(WRITE "${project_dir}/${file}" "project")
endforeach()

zenith_find_package_asset_collisions(collisions "${engine_dir}" "${engine_assets}" "${project_dir}")
expect("\"${collisions}\" STREQUAL \"Primitives/Cube.gltf\""
    "a project asset at an engine runtime asset's path collides, compared without case (found '${collisions}')")
zenith_find_package_asset_collisions(collisions "${engine_dir}" "" "${project_dir}")
expect("\"${collisions}\" STREQUAL \"\""
    "engine assets that are not runtime assets (such as the demo room) never collide")

execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DZENITH_TEST_ROOT=${ZENITH_TEST_ROOT}" "-DZENITH_COLLISION_CHECK="
        -P "${CMAKE_CURRENT_LIST_FILE}"
    RESULT_VARIABLE refused_result
    OUTPUT_VARIABLE refused_output
    ERROR_VARIABLE refused_error)
expect("NOT refused_result EQUAL 0" "a collision fails the configure")
string(FIND "${refused_error}" "Primitives/Cube.gltf" named)
string(FIND "${refused_error}" "ZENITH_PACKAGE_ASSET_OVERRIDES" remedy)
expect("NOT named EQUAL -1 AND NOT remedy EQUAL -1"
    "the failure names the colliding path and the override list:\n${refused_error}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DZENITH_TEST_ROOT=${ZENITH_TEST_ROOT}" "-DZENITH_COLLISION_CHECK=primitives/cube.gltf"
        -P "${CMAKE_CURRENT_LIST_FILE}"
    RESULT_VARIABLE overridden_result
    OUTPUT_VARIABLE overridden_output
    ERROR_VARIABLE overridden_error)
# Message text wraps, so look for single words.
string(FIND "${overridden_error}" "Primitives/Cube.gltf" warned)
expect("overridden_result EQUAL 0 AND NOT warned EQUAL -1"
    "an overridden path ships the project's file with a warning:\n${overridden_error}")

file(REMOVE_RECURSE "${ZENITH_TEST_ROOT}")
if(NOT failures EQUAL 0)
    message(FATAL_ERROR "${failures} package asset collision expectations failed")
endif()
message(STATUS "Package asset collision checks passed")
