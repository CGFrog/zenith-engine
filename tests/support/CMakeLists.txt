# Shared test support (docs/TESTING.md): helpers every test may use, and the
# main of the doctest test binaries.
add_library(zenith_test_support STATIC Files.cpp StandaloneChecks.cpp TempDirectory.cpp)
target_include_directories(zenith_test_support PUBLIC "${PROJECT_SOURCE_DIR}/tests")
target_link_libraries(zenith_test_support PUBLIC doctest::doctest glm::glm)
target_compile_definitions(zenith_test_support PRIVATE ZENITH_TEST_TEMP_DIR="${ZENITH_TEST_TEMP_DIR}")
zenith_target_warnings(zenith_test_support)

# The Vulkan device probe GPU tests skip with; links its own volk, like the
# capture driver, since the engine's is private.
add_library(zenith_test_vulkan_probe STATIC VulkanProbe.cpp)
target_include_directories(zenith_test_vulkan_probe PUBLIC "${PROJECT_SOURCE_DIR}/tests")
target_link_libraries(zenith_test_vulkan_probe PRIVATE volk)
zenith_target_warnings(zenith_test_vulkan_probe)

add_library(zenith_test_main STATIC TestMain.cpp)
target_link_libraries(zenith_test_main PUBLIC doctest::doctest)
zenith_target_warnings(zenith_test_main)
