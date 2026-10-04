# Third-party dependencies: FetchContent declarations, the project's wrapper
# targets for libraries without a usable CMake target, and the editor's pixel
# fonts. Included from the root CMakeLists.txt, so every variable it sets
# (<name>_SOURCE_DIR, ZENITH_PIXEL_FONT_DIR) is visible to the whole project.
include_guard(GLOBAL)

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
# SPIR-V optimization inside glslang is disabled; shaderc links the separately
# fetched SPIRV-Tools targets below for its runtime compilation pipeline.
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

# shaderc provides the in-process GLSL compiler used for shader hot reloading.
# Keep its SPIR-V dependencies on the same Vulkan SDK release as glslang.
set(SPIRV_HEADERS_SKIP_EXAMPLES ON CACHE BOOL "" FORCE)
set(SPIRV_HEADERS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    SPIRV-Headers
    GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Headers.git
    GIT_TAG vulkan-sdk-1.4.357.0
    GIT_SHALLOW TRUE
)

set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
set(SKIP_SPIRV_TOOLS_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    SPIRV-Tools
    GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Tools.git
    GIT_TAG vulkan-sdk-1.4.357.0
    GIT_SHALLOW TRUE
)

set(SHADERC_ENABLE_HLSL OFF CACHE BOOL "" FORCE)
# Match the DLL CRT used by Zenith and the other MSVC targets.
set(SHADERC_ENABLE_SHARED_CRT ON CACHE BOOL "" FORCE)
set(SHADERC_SKIP_INSTALL ON CACHE BOOL "" FORCE)
set(SHADERC_SKIP_TESTS ON CACHE BOOL "" FORCE)
set(SHADERC_SKIP_EXAMPLES ON CACHE BOOL "" FORCE)
set(SHADERC_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
set(SHADERC_SKIP_COPYRIGHT_CHECK ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    shaderc
    GIT_REPOSITORY https://github.com/google/shaderc.git
    GIT_TAG v2026.3
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

# SDL is shared because the engine library, executables, and tests all call it
# and must see one set of SDL globals (environment, video, errors).
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    SDL
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.10
    GIT_SHALLOW TRUE
)

# Dear ImGui supplies the immediate-mode authoring widgets used by engine tools.
# The docking branch is pinned because docking is a core level-editor workflow.
FetchContent_Declare(
    imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b-docking.tar.gz
    URL_HASH SHA256=466fdef9b18de15f0bb6e288e3d00ffa3d82200ec458ce5e4f724a161d9528a5
)

FetchContent_Declare(
    imguizmo
    URL https://github.com/CedricGuillemet/ImGuizmo/archive/refs/tags/1.10.tar.gz
    URL_HASH SHA256=a467155ee60a262430478a06c6534ab56fe62b053b896308581f5aa7057f5094
)

# Inter is the editor's fallback UI font (SIL Open Font License); only its TTF
# files are used.
FetchContent_Declare(
    inter
    URL https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip
    URL_HASH SHA256=9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e
)

# Press Start 2P (titles) and VT323 (body text) are the editor's pixel fonts
# (SIL Open Font License), pinned to one google/fonts revision.
set(ZENITH_PIXEL_FONT_REVISION 9710da1eacb3be272583c3224dcb70f9da6eadbb)
set(ZENITH_PIXEL_FONT_DIR "${PROJECT_BINARY_DIR}/_deps/pixel-fonts")
function(zenith_download_pixel_font source name hash)
    file(DOWNLOAD
        "https://raw.githubusercontent.com/google/fonts/${ZENITH_PIXEL_FONT_REVISION}/ofl/${source}"
        "${ZENITH_PIXEL_FONT_DIR}/${name}"
        EXPECTED_HASH SHA256=${hash}
        TLS_VERIFY ON)
endfunction()
zenith_download_pixel_font(pressstart2p/PressStart2P-Regular.ttf PressStart2P-Regular.ttf
    034c77f1f05ec89421e4a63f0e3a4ca1ecf852cc6d2bf611f126f275728e017d)
zenith_download_pixel_font(pressstart2p/OFL.txt PressStart2P-OFL.txt
    705960c3281a5765ecc0b59bd4ed7ca59eed165748076bc2fc3e8fdbfeb944b0)
zenith_download_pixel_font(vt323/VT323-Regular.ttf VT323-Regular.ttf
    cf4de751ada78ceac033dbe16a687742939995b77bc2a052ae17a4957958594d)
zenith_download_pixel_font(vt323/OFL.txt VT323-OFL.txt
    27d9af34210253e7ca1251fbace86c6f65b40031d6ce1a75493a1b2093631298)

FetchContent_Declare(
    cgltf
    GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
    GIT_TAG v1.15
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20
    GIT_SHALLOW TRUE
)

set(ENTT_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ENTT_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    EnTT
    GIT_REPOSITORY https://github.com/skypjack/entt.git
    GIT_TAG v3.13.2
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
)

# Jolt supplies the low-level collision detection and constraint solver. Zenith
# owns the ECS-facing API so gameplay code is not required to use Jolt types.
set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)
set(CPP_RTTI_ENABLED ON CACHE BOOL "" FORCE)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
set(JPH_USE_CPU_COMPUTE OFF CACHE BOOL "" FORCE)
set(ENABLE_OBJECT_STREAM OFF CACHE BOOL "" FORCE)
# PhysicsWorld::drawDebug (JoltDebugDraw) needs Jolt's debug renderer in every
# configuration. Jolt publishes the matching JPH_DEBUG_RENDERER definition to
# its users, so the library and the engine always agree on it.
set(DEBUG_RENDERER_IN_DISTRIBUTION ON CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    JoltPhysics
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG v5.6.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR Build
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

# meshoptimizer reorders imported geometry for the GPU and builds LOD chains.
FetchContent_Declare(
    meshoptimizer
    GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
    GIT_TAG v1.3
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(
    VulkanHeaders
    SPIRV-Headers
    SPIRV-Tools
    glslang
    shaderc
    volk
    glm
    SDL
    imgui
    imguizmo
    inter
    cgltf
    stb
    EnTT
    nlohmann_json
    JoltPhysics
    VulkanMemoryAllocator
)

# meshoptimizer runs per primitive on every model import, so an unoptimized
# Debug build of it dominates Debug startup (and every GPU test that launches
# the engine). It is third-party code nobody steps through, so its directory
# gets the RelWithDebInfo flags in Debug; the variable change is scoped to
# that subdirectory and restored for the rest of the project.
set(ZENITH_SAVED_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
set(ZENITH_SAVED_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_RELWITHDEBINFO}")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
FetchContent_MakeAvailable(meshoptimizer)
set(CMAKE_C_FLAGS_DEBUG "${ZENITH_SAVED_C_FLAGS_DEBUG}")
set(CMAKE_CXX_FLAGS_DEBUG "${ZENITH_SAVED_CXX_FLAGS_DEBUG}")

# Dear ImGui is compiled into zenith_engine (whose renderer owns the context
# and uses the engine's volk function table), and exported from it with
# IMGUI_API so the editor and its widgets share that one context.
add_library(imgui OBJECT
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_demo.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp"
)
target_include_directories(imgui PUBLIC
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends"
)
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK VK_NO_PROTOTYPES)
if(WIN32)
    target_compile_definitions(imgui PRIVATE "IMGUI_API=__declspec(dllexport)")
endif()
target_link_libraries(imgui PUBLIC Vulkan::Headers volk SDL3::SDL3)

# ImGuizmo declares its own functions with IMGUI_API, so it must be built where
# that macro means export: it is compiled into zenith_engine beside Dear ImGui.
# The dependency's static target is not built.
set_target_properties(imguizmo PROPERTIES EXCLUDE_FROM_ALL ON)
add_library(zenith_imguizmo OBJECT "${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp")
target_include_directories(zenith_imguizmo PUBLIC "${imguizmo_SOURCE_DIR}/src")
target_link_libraries(zenith_imguizmo PUBLIC imgui)
if(WIN32)
    target_compile_definitions(zenith_imguizmo PRIVATE "IMGUI_API=__declspec(dllexport)")
endif()

# cgltf is a single-header library and does not provide its own CMake target.
add_library(cgltf INTERFACE)
add_library(cgltf::cgltf ALIAS cgltf)
target_include_directories(cgltf INTERFACE "${cgltf_SOURCE_DIR}")

add_library(stb INTERFACE)
target_include_directories(stb INTERFACE "${stb_SOURCE_DIR}")

# bc7enc_rdo supplies the offline BC7 (bc7enc) and BC5 (rgbcx) block encoders
# and decoders used by texture cooking. Only the encoder sources are built; the
# repository's own CMake project (a command-line tool) is skipped.
FetchContent_Declare(
    bc7enc_rdo
    GIT_REPOSITORY https://github.com/richgel999/bc7enc_rdo.git
    GIT_TAG b9438627eef73a1157e84201b6fa6eb2ffd6d9f0
    SOURCE_SUBDIR zenith-sources-only
)
FetchContent_MakeAvailable(bc7enc_rdo)
add_library(zenith_bc7enc STATIC
    "${bc7enc_rdo_SOURCE_DIR}/bc7enc.cpp"
    "${bc7enc_rdo_SOURCE_DIR}/bc7decomp.cpp"
    "${bc7enc_rdo_SOURCE_DIR}/rgbcx.cpp"
)
target_include_directories(zenith_bc7enc SYSTEM PUBLIC "${bc7enc_rdo_SOURCE_DIR}")
target_compile_definitions(zenith_bc7enc PUBLIC RGBCX_USE_SMALLER_TABLES=1)
if(MSVC)
    target_compile_options(zenith_bc7enc PRIVATE /W0)
else()
    target_compile_options(zenith_bc7enc PRIVATE -w)
endif()
