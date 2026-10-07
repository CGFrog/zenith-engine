# The packaging configure check against project assets replacing engine
# runtime assets (cmake/PackageProject.cmake).
zenith_add_test(zenith_package_asset_collisions EXECUTABLE "${CMAKE_COMMAND}"
    COMMAND_ARGS "-DZENITH_TEST_ROOT=${ZENITH_TEST_TEMP_DIR}/package-asset-collisions"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/PackageAssetCollisionTests.cmake")
