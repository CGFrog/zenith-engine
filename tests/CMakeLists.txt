# Automated tests. Each folder declares its tests with zenith_add_test
# (cmake/ZenithTesting.cmake); every test runs from the top of the build tree.
include("${PROJECT_SOURCE_DIR}/cmake/ZenithTesting.cmake")

# Editor runs keep their settings (workspaces.json, the smoke scene, ...)
# in their own directory under the build tree, never the user's, starting
# from an empty one each ctest run so they open the default workspaces.
set(ZENITH_EDITOR_TEST_SETTINGS_DIR "${PROJECT_BINARY_DIR}/editor-test-settings")

# The demo project's render settings, repeated by the projects generated for
# the editor module smoke and the game validation runs.
file(READ "${PROJECT_SOURCE_DIR}/projects/demo.zenith-project" ZENITH_DEMO_PROJECT_JSON)
string(JSON ZENITH_DEMO_RENDER_SETTINGS GET "${ZENITH_DEMO_PROJECT_JSON}" renderSettings)

add_subdirectory(support)
add_subdirectory(engine)
add_subdirectory(game)
add_subdirectory(editor)
add_subdirectory(capture)
add_subdirectory(packaging)
add_subdirectory(validation)

zenith_add_test(zenith_test_temp_directory EXECUTABLE "${CMAKE_COMMAND}"
    COMMAND_ARGS -E make_directory "${ZENITH_TEST_TEMP_DIR}"
    FIXTURES_SETUP zenith_test_temp)
