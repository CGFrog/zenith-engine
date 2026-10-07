# Shared test support (docs/TESTING.md): helpers every test may use, and the
# main of the doctest test binaries.
add_library(zenith_test_support STATIC Files.cpp TempDirectory.cpp)
target_include_directories(zenith_test_support PUBLIC "${PROJECT_SOURCE_DIR}/tests")
target_link_libraries(zenith_test_support PUBLIC doctest::doctest glm::glm)
target_compile_definitions(zenith_test_support PRIVATE ZENITH_TEST_TEMP_DIR="${ZENITH_TEST_TEMP_DIR}")
zenith_target_warnings(zenith_test_support)

add_library(zenith_test_main STATIC TestMain.cpp)
target_link_libraries(zenith_test_main PUBLIC doctest::doctest)
zenith_target_warnings(zenith_test_main)
