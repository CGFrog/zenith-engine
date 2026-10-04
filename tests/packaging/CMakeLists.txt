# The packaging configure check against project assets replacing engine
# runtime assets (cmake/PackageProject.cmake).
zenith_add_test(zenith_package_asset_collisions EXECUTABLE "${CMAKE_COMMAND}"
    COMMAND_ARGS "-DZENITH_TEST_ROOT=${PROJECT_BINARY_DIR}/package-asset-collision-tests"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/PackageAssetCollisionTests.cmake")
