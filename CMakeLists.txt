cmake_minimum_required(VERSION 3.24)

project(zenith VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# Fetch Vulkan's headers but do not link the platform Vulkan loader. Volk will
# load the Vulkan runtime dynamically when the renderer initializes it.
FetchContent_Declare(
    VulkanHeaders
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG vulkan-sdk-1.4.357.0
    GIT_SHALLOW TRUE
)

# Build Khronos's reference GLSL-to-SPIR-V compiler as part of the project.
# SPIR-V optimization is disabled so glslang remains a self-contained
# FetchContent dependency and does not require a separate SPIRV-Tools checkout.
set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    glslang
    GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
    GIT_TAG vulkan-sdk-1.4.357.0
    GIT_SHALLOW TRUE
)

set(VOLK_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG vulkan-sdk-1.4.357.0
    GIT_SHALLOW TRUE
)

set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.3
    GIT_SHALLOW TRUE
)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    SDL
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.10
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    cgltf
    GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
    GIT_TAG v1.15
    GIT_SHALLOW TRUE
)

set(VMA_BUILD_DOCUMENTATION OFF CACHE BOOL "" FORCE)
set(VMA_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(VMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    VulkanMemoryAllocator
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG v3.4.0
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(
    VulkanHeaders
    glslang
    volk
    glm
    SDL
    cgltf
    VulkanMemoryAllocator
)

# cgltf is a single-header library and does not provide its own CMake target.
add_library(cgltf INTERFACE)
add_library(cgltf::cgltf ALIAS cgltf)
target_include_directories(cgltf INTERFACE "${cgltf_SOURCE_DIR}")

add_executable(zenith
    src/main.cpp
)

# Compile each shader in shaders/ to a matching .spv file in the build tree.
# glslang infers the shader stage from the source extension.
set(ZENITH_SHADER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(ZENITH_SHADER_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
set(ZENITH_SHADER_EXTENSIONS
    vert tesc tese geom frag comp
    task mesh
    rgen rint rahit rchit rmiss rcall
)

set(ZENITH_SHADER_SOURCES)
foreach(shader_extension IN LISTS ZENITH_SHADER_EXTENSIONS)
    file(GLOB_RECURSE shader_sources CONFIGURE_DEPENDS
        "${ZENITH_SHADER_SOURCE_DIR}/*.${shader_extension}"
    )
    list(APPEND ZENITH_SHADER_SOURCES ${shader_sources})
endforeach()

set(ZENITH_SHADER_BINARIES)
foreach(shader_source IN LISTS ZENITH_SHADER_SOURCES)
    file(RELATIVE_PATH shader_relative_path
        "${ZENITH_SHADER_SOURCE_DIR}"
        "${shader_source}"
    )
    set(shader_binary "${ZENITH_SHADER_BINARY_DIR}/${shader_relative_path}.spv")
    get_filename_component(shader_binary_dir "${shader_binary}" DIRECTORY)

    add_custom_command(
        OUTPUT "${shader_binary}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${shader_binary_dir}"
        COMMAND $<TARGET_FILE:glslang-standalone>
            --target-env vulkan1.3
            -V "${shader_source}"
            -o "${shader_binary}"
        DEPENDS "${shader_source}" glslang-standalone
        COMMENT "Compiling Vulkan shader ${shader_relative_path}"
        VERBATIM
    )
    list(APPEND ZENITH_SHADER_BINARIES "${shader_binary}")
endforeach()

add_custom_target(zenith_shaders ALL DEPENDS ${ZENITH_SHADER_BINARIES})
add_dependencies(zenith zenith_shaders)

target_include_directories(zenith PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_definitions(zenith PRIVATE VK_NO_PROTOTYPES)

target_link_libraries(zenith PRIVATE
    Vulkan::Headers
    volk
    glm::glm
    SDL3::SDL3
    cgltf::cgltf
    GPUOpen::VulkanMemoryAllocator
)

if(MSVC)
    target_compile_options(zenith PRIVATE /W4 /permissive-)
else()
    target_compile_options(zenith PRIVATE -Wall -Wextra -Wpedantic)
endif()
