cmake_minimum_required(VERSION 3.24)

# {{PROJECT_NAME}}: a Zenith C++ game project. The game module in src/ is a
# shared library the Zenith editor loads (and hot reloads) for this project.
project({{PROJECT_IDENTIFIER}} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# A module must be built with the engine's compiler and runtime, so a
# single-configuration generator defaults to the configuration the editor was
# built in. Multi-configuration generators choose it at build time.
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE {{BUILD_CONFIG}} CACHE STRING "Build type" FORCE)
endif()

# The Zenith engine to build against: a configured and built Zenith build
# directory (it holds ZenithEngineConfig.cmake), or a Zenith checkout whose
# build/ directory is one.
set(ZENITH_ROOT "{{ZENITH_ROOT}}" CACHE PATH "Zenith engine build directory (or checkout with a build/ directory)")
if(EXISTS "${ZENITH_ROOT}/ZenithEngineConfig.cmake")
    include("${ZENITH_ROOT}/ZenithEngineConfig.cmake")
elseif(EXISTS "${ZENITH_ROOT}/build/ZenithEngineConfig.cmake")
    include("${ZENITH_ROOT}/build/ZenithEngineConfig.cmake")
else()
    message(FATAL_ERROR
        "ZENITH_ROOT (${ZENITH_ROOT}) is not a Zenith build directory. Configure and build Zenith, then set "
        "ZENITH_ROOT to its build directory, for example: cmake --preset zenith -DZENITH_ROOT=C:/zenith/build")
endif()

add_subdirectory(src)
