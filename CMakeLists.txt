cmake_minimum_required(VERSION 3.24)

project(zenith VERSION 0.1.0 LANGUAGES C CXX)

execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE ZENITH_SOURCE_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(ZENITH_SOURCE_REVISION STREQUAL "")
    set(ZENITH_SOURCE_REVISION "unknown")
endif()
execute_process(COMMAND git status --porcelain WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE ZENITH_WORKTREE_STATUS OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(ZENITH_WORKTREE_STATUS STREQUAL "")
    set(ZENITH_SOURCE_DIRTY "false")
else()
    set(ZENITH_SOURCE_DIRTY "true")
endif()
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt" ZENITH_DEPENDENCY_REVISION)

include(CTest)

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

# Inter is the editor UI font (SIL Open Font License); only its TTF files are used.
FetchContent_Declare(
    inter
    URL https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip
    URL_HASH SHA256=9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e
)

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
    meshoptimizer
)

add_library(imgui STATIC
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
target_link_libraries(imgui PUBLIC Vulkan::Headers volk SDL3::SDL3)
target_link_libraries(imguizmo PUBLIC imgui)

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

add_library(zenith_engine STATIC
    src/engine/diagnostics/Benchmark.cpp
    src/engine/animation/AnimationSystem.cpp
    src/engine/audio/AudioSystem.cpp
    src/engine/audio/StbVorbis.cpp
    src/engine/Engine.cpp
    src/engine/application/Application.cpp
    src/engine/assets/Assets.cpp
    src/engine/assets/ModelImporter.cpp
    src/engine/assets/MeshOptimization.cpp
    src/engine/assets/ImageDecoder.cpp
    src/engine/assets/ProductionAssetPipeline.cpp
    src/engine/assets/CompressedTexture.cpp
    src/engine/assets/TextureCooker.cpp
    src/engine/assets/AssetStreaming.cpp
    src/engine/assets/PackedVertexStreams.cpp
    src/engine/graphics/Renderer.cpp
    src/engine/graphics/Lighting.cpp
    src/engine/graphics/LocalShadows.cpp
    src/engine/graphics/EnvironmentLighting.cpp
    src/engine/graphics/RenderConfiguration.cpp
    src/engine/graphics/RenderSubmission.cpp
    src/engine/graphics/MeshLodSelection.cpp
    src/engine/graphics/TemporalQuality.cpp
    src/engine/graphics/ToneMapping.cpp
    src/engine/graphics/AmbientOcclusion.cpp
    src/engine/graphics/graph/RenderGraph.cpp
    src/engine/graphics/passes/MeshPass.cpp
    src/engine/graphics/passes/GpuDrivenPass.cpp
    src/engine/graphics/passes/DepthPyramid.cpp
    src/engine/graphics/passes/AmbientOcclusionPass.cpp
    src/engine/graphics/passes/ClusteredLightingPass.cpp
    src/engine/graphics/passes/LocalShadowPass.cpp
    src/engine/graphics/passes/EnvironmentPass.cpp
    src/engine/graphics/passes/PostProcessPass.cpp
    src/engine/graphics/passes/ScreenSpaceReflectionPass.cpp
    src/engine/graphics/passes/TemporalResolvePass.cpp
    src/engine/graphics/passes/BloomExposurePass.cpp
    src/engine/graphics/passes/UiPass.cpp
    src/engine/graphics/pipeline/GraphicsPipeline.cpp
    src/engine/graphics/pipeline/ComputePipeline.cpp
    src/engine/graphics/resources/FrameResources.cpp
    src/engine/graphics/resources/UploadContext.cpp
    src/engine/graphics/resources/BindlessTextureTable.cpp
    src/engine/graphics/resources/GeometryArena.cpp
    src/engine/graphics/resources/ViewportTarget.cpp
    src/engine/graphics/shader/ShaderModule.cpp
    src/engine/graphics/surface/Surface.cpp
    src/engine/graphics/swapchain/Swapchain.cpp
    src/engine/graphics/vulkan/VulkanContext.cpp
    src/engine/input/InputActions.cpp
    src/engine/input/InputManager.cpp
    src/engine/input/InputProfiles.cpp
    src/engine/jobs/JobSystem.cpp
    src/engine/jobs/FrameTaskGraph.cpp
    src/engine/physics/PhysicsWorld.cpp
    src/engine/platform/PlatformServices.cpp
    src/engine/project/ProjectConfig.cpp
    src/engine/resources/ResourceLocator.cpp
    src/engine/resources/ContentManifest.cpp
    src/engine/scene/Scene.cpp
    src/engine/scene/SceneDocument.cpp
    src/engine/scene/AdditiveSceneManager.cpp
    src/engine/scene/Prefab.cpp
    src/engine/ui/GameUi.cpp
    src/engine/ui/Accessibility.cpp
    src/engine/ui/Localization.cpp
    src/engine/window/Window.cpp
)

if(MSVC)
    set_source_files_properties(src/engine/audio/StbVorbis.cpp PROPERTIES COMPILE_OPTIONS /W0)
endif()

add_library(zenith_demo STATIC
    src/game/application/DemoApplication.cpp
    src/game/application/GameSettings.cpp
    src/game/input/DemoInput.cpp
    src/game/player/FirstPersonController.cpp
    src/game/scene/DemoScene.cpp
    src/game/scene/GameplayScene.cpp
)
target_link_libraries(zenith_demo PUBLIC zenith_engine)

add_executable(zenith src/main.cpp)
target_link_libraries(zenith PRIVATE zenith_demo)
if(APPLE)
    set_target_properties(zenith PROPERTIES MACOSX_BUNDLE TRUE
        OUTPUT_NAME "Zenith"
        MACOSX_BUNDLE_BUNDLE_NAME "Zenith"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.zenith.demo"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
endif()

add_library(zenith_editor_app STATIC
    src/editor/application/EditorApplication.cpp
    src/editor/application/EditorDiagnostics.cpp
    src/editor/application/EditorObjectCommands.cpp
    src/editor/application/EditorPlayMode.cpp
    src/editor/application/EditorShortcuts.cpp
    src/editor/application/EditorTheme.cpp
    src/editor/application/NativeFileDialog.cpp
    src/editor/application/EditorSession.cpp
    src/editor/document/CommandHistory.cpp
    src/editor/document/DefaultScene.cpp
    src/editor/document/EditorDocument.cpp
    src/editor/document/EditorRecovery.cpp
    src/editor/document/PrefabAuthoring.cpp
    src/editor/document/SceneFragment.cpp
    src/editor/interaction/ComponentEditors.cpp
    src/editor/interaction/EditorCamera.cpp
    src/editor/interaction/ModalTransform.cpp
    src/editor/interaction/ObjectKeymap.cpp
    src/editor/interaction/GraphicsSettingsEditor.cpp
    src/editor/project/AssetCatalog.cpp
    src/editor/project/DerivedAssetInspector.cpp
    src/editor/project/ReflectionProbeBaker.cpp
    src/editor/project/ThumbnailCache.cpp
    src/editor/settings/EditorRenderSettings.cpp
    src/editor/settings/EditorSettings.cpp
)
target_link_libraries(zenith_editor_app PUBLIC zenith_engine)
target_link_libraries(zenith_editor_app PUBLIC imguizmo::imguizmo)
target_compile_definitions(zenith_editor_app PRIVATE GLM_ENABLE_EXPERIMENTAL)

add_executable(zenith_editor src/editor/main.cpp)
target_link_libraries(zenith_editor PRIVATE zenith_editor_app)

# Editor-only runtime resources live beside assets/ under <resource root>/editor.
set(ZENITH_EDITOR_FONT_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/editor/fonts")
add_custom_target(zenith_editor_resources
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${ZENITH_EDITOR_FONT_BINARY_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${inter_SOURCE_DIR}/extras/ttf/Inter-Regular.ttf"
        "${inter_SOURCE_DIR}/LICENSE.txt"
        "${ZENITH_EDITOR_FONT_BINARY_DIR}"
    COMMENT "Copying Zenith editor resources"
)
add_dependencies(zenith_editor zenith_editor_resources)

add_executable(zenith_asset_build src/asset_build_main.cpp)
target_link_libraries(zenith_asset_build PRIVATE zenith_engine)

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
            --target-env vulkan1.4
            -V "${shader_source}"
            -o "${shader_binary}"
        DEPENDS "${shader_source}" glslang-standalone
        COMMENT "Compiling Vulkan shader ${shader_relative_path}"
        VERBATIM
    )
    list(APPEND ZENITH_SHADER_BINARIES "${shader_binary}")
endforeach()

add_custom_target(zenith_shaders ALL DEPENDS ${ZENITH_SHADER_BINARIES})
add_dependencies(zenith_engine zenith_shaders)

set(ZENITH_ASSET_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets")
set(ZENITH_ASSET_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/assets")
add_custom_target(zenith_assets ALL
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${ZENITH_ASSET_SOURCE_DIR}" "${ZENITH_ASSET_BINARY_DIR}"
    COMMENT "Copying Zenith assets"
)
add_dependencies(zenith_engine zenith_assets)

# Cook every shipped model's external textures to block-compressed KTX2 in the
# derived-data cache the executables read from <resource root>/derived. Entries
# are keyed by source bytes, so unchanged textures are skipped on rebuild.
set(ZENITH_DERIVED_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/derived")
file(GLOB_RECURSE ZENITH_COOKED_MODELS CONFIGURE_DEPENDS "${ZENITH_ASSET_SOURCE_DIR}/*.gltf")
file(GLOB_RECURSE ZENITH_COOKED_TEXTURE_SOURCES CONFIGURE_DEPENDS "${ZENITH_ASSET_SOURCE_DIR}/*.png"
    "${ZENITH_ASSET_SOURCE_DIR}/*.jpg")
set(ZENITH_TEXTURE_COOK_COMMANDS)
foreach(cooked_model IN LISTS ZENITH_COOKED_MODELS)
    list(APPEND ZENITH_TEXTURE_COOK_COMMANDS
        COMMAND $<TARGET_FILE:zenith_asset_build> cook-model-textures "${cooked_model}" "${ZENITH_DERIVED_BINARY_DIR}")
endforeach()
add_custom_command(
    OUTPUT "${ZENITH_DERIVED_BINARY_DIR}/cooked-textures.stamp"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${ZENITH_DERIVED_BINARY_DIR}"
    ${ZENITH_TEXTURE_COOK_COMMANDS}
    COMMAND "${CMAKE_COMMAND}" -E touch "${ZENITH_DERIVED_BINARY_DIR}/cooked-textures.stamp"
    DEPENDS zenith_asset_build ${ZENITH_COOKED_MODELS} ${ZENITH_COOKED_TEXTURE_SOURCES}
    COMMENT "Cooking block-compressed textures"
    VERBATIM
)
add_custom_target(zenith_cooked_textures ALL DEPENDS "${ZENITH_DERIVED_BINARY_DIR}/cooked-textures.stamp")

target_include_directories(zenith_engine PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_definitions(zenith_engine PUBLIC VK_NO_PROTOTYPES PRIVATE
    ZENITH_BENCHMARK_REVISION="${ZENITH_SOURCE_REVISION}"
    ZENITH_BENCHMARK_DIRTY="${ZENITH_SOURCE_DIRTY}"
    ZENITH_BENCHMARK_BUILD="$<CONFIG>"
    ZENITH_BENCHMARK_COMPILER="${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}"
    ZENITH_BENCHMARK_PLATFORM="${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}"
)

target_link_libraries(zenith_engine PUBLIC
    Vulkan::Headers
    volk
    glm::glm
    SDL3::SDL3
    cgltf::cgltf
    stb
    EnTT::EnTT
    nlohmann_json::nlohmann_json
    Jolt
    GPUOpen::VulkanMemoryAllocator
    shaderc
    imgui
)
target_link_libraries(zenith_engine PRIVATE zenith_bc7enc)
target_link_libraries(zenith_engine PRIVATE meshoptimizer)

if(MSVC)
    target_compile_options(zenith_engine PRIVATE /W4 /permissive- /FS)
    target_compile_options(zenith_demo PRIVATE /W4 /permissive-)
    target_compile_options(zenith_editor_app PRIVATE /W4 /permissive-)
else()
    target_compile_options(zenith_engine PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(zenith_demo PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(zenith_editor_app PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Relocatable release archives. Runtime code resolves these directories from
# the executable, so installed artifacts do not retain source/build paths.
install(TARGETS zenith zenith_asset_build
    RUNTIME_DEPENDENCIES
        PRE_EXCLUDE_REGEXES "api-ms-" "ext-ms-"
        POST_EXCLUDE_REGEXES ".*[Ss]ystem32/.*" ".*/lib(c|m|pthread|dl|rt)\\.so.*"
    RUNTIME DESTINATION .
    BUNDLE DESTINATION .
)
install(DIRECTORY "${ZENITH_ASSET_SOURCE_DIR}/" DESTINATION assets)
install(DIRECTORY "${ZENITH_SHADER_BINARY_DIR}/" DESTINATION shaders FILES_MATCHING PATTERN "*.spv")
install(DIRECTORY "${ZENITH_DERIVED_BINARY_DIR}/" DESTINATION derived FILES_MATCHING PATTERN "*.zderived")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/projects/demo.zenith-project" DESTINATION .)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/projects/scenes/demo.zenith-scene" DESTINATION scenes)
if(UNIX AND NOT APPLE)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/cmake/zenith.desktop" DESTINATION share/applications)
endif()
install(FILES "${sdl_SOURCE_DIR}/LICENSE.txt" DESTINATION licenses RENAME SDL.txt)
install(FILES "${volk_SOURCE_DIR}/LICENSE.md" DESTINATION licenses RENAME volk.md)
install(FILES "${glm_SOURCE_DIR}/copying.txt" DESTINATION licenses RENAME glm.txt)
install(FILES "${joltphysics_SOURCE_DIR}/LICENSE" DESTINATION licenses RENAME Jolt.txt)
install(FILES "${imgui_SOURCE_DIR}/LICENSE.txt" DESTINATION licenses RENAME Dear-ImGui.txt)
install(FILES "${cgltf_SOURCE_DIR}/LICENSE" DESTINATION licenses RENAME cgltf.txt)
install(FILES "${stb_SOURCE_DIR}/LICENSE" DESTINATION licenses RENAME stb.txt)
install(FILES "${entt_SOURCE_DIR}/LICENSE" DESTINATION licenses RENAME EnTT.txt)
install(FILES "${vulkanmemoryallocator_SOURCE_DIR}/LICENSE.txt" DESTINATION licenses
    RENAME Vulkan-Memory-Allocator.txt)
install(FILES "${shaderc_SOURCE_DIR}/LICENSE" DESTINATION licenses RENAME shaderc.txt)
install(FILES "${vulkanheaders_SOURCE_DIR}/LICENSE.md" DESTINATION licenses RENAME Vulkan-Headers.md)
install(FILES "${glslang_SOURCE_DIR}/LICENSE.txt" DESTINATION licenses RENAME glslang.txt)
install(FILES "${spirv-headers_SOURCE_DIR}/LICENSE" DESTINATION licenses RENAME SPIRV-Headers.txt)
install(FILES "${spirv-tools_SOURCE_DIR}/LICENSE" DESTINATION licenses RENAME SPIRV-Tools.txt)
install(FILES "${nlohmann_json_SOURCE_DIR}/LICENSE.MIT" DESTINATION licenses RENAME nlohmann-json.txt)
install(FILES "${imguizmo_SOURCE_DIR}/LICENSE" DESTINATION licenses RENAME ImGuizmo.txt)
install(FILES "${bc7enc_rdo_SOURCE_DIR}/LICENSE" DESTINATION licenses RENAME bc7enc_rdo.txt)
install(FILES "${meshoptimizer_SOURCE_DIR}/LICENSE.md" DESTINATION licenses RENAME meshoptimizer.md)
install(CODE "
    set(ZENITH_PACKAGE_ROOT \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}\")
    set(ZENITH_PACKAGE_VERSION \"${PROJECT_VERSION}\")
    set(ZENITH_SOURCE_REVISION \"${ZENITH_SOURCE_REVISION}\")
    set(ZENITH_DEPENDENCY_REVISION \"${ZENITH_DEPENDENCY_REVISION}\")
    set(ZENITH_TOOLCHAIN \"${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}\")
    set(ZENITH_TARGET_PLATFORM \"${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}\")
    include(\"${CMAKE_CURRENT_SOURCE_DIR}/cmake/ValidatePackage.cmake\")
")

set(CPACK_PACKAGE_NAME "Zenith")
set(CPACK_PACKAGE_VENDOR "Zenith")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "Zenith-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_POST_BUILD_SCRIPTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/SignPackage.cmake")
if(WIN32)
    option(ZENITH_WINDOWS_INSTALLER "Build a WiX MSI alongside the portable ZIP" OFF)
    if(ZENITH_WINDOWS_INSTALLER)
        set(CPACK_GENERATOR "ZIP;WIX")
        set(CPACK_WIX_UPGRADE_GUID "B37C201B-67EB-4F8C-9CC1-1CC87254C11A")
    else()
        set(CPACK_GENERATOR "ZIP")
    endif()
elseif(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
else()
    set(CPACK_GENERATOR "TGZ;DEB")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Zenith")
    set(CPACK_DEBIAN_PACKAGE_SECTION "games")
endif()
include(CPack)

if(BUILD_TESTING)
    add_executable(zenith_milestone_19_21_tests tests/engine/Milestone19And21Tests.cpp)
    target_compile_definitions(zenith_milestone_19_21_tests PRIVATE ZENITH_ASSET_DIR="${ZENITH_ASSET_BINARY_DIR}")
    add_dependencies(zenith_milestone_19_21_tests zenith_assets)
    target_link_libraries(zenith_milestone_19_21_tests PRIVATE zenith_engine zenith_demo zenith_editor_app)
    if(MSVC)
        target_compile_options(zenith_milestone_19_21_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_milestone_19_21_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_milestones_19_21 COMMAND zenith_milestone_19_21_tests)

    add_executable(zenith_production_milestone_tests tests/engine/ProductionMilestonesTests.cpp)
    target_link_libraries(zenith_production_milestone_tests PRIVATE zenith_engine)
    target_compile_definitions(zenith_production_milestone_tests PRIVATE
        ZENITH_ASSET_DIR="${ZENITH_ASSET_BINARY_DIR}")
    add_dependencies(zenith_production_milestone_tests zenith_assets)
    if(MSVC)
        target_compile_options(zenith_production_milestone_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_production_milestone_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_production_milestones COMMAND zenith_production_milestone_tests)

    add_executable(zenith_audio_tests tests/engine/AudioSystemTests.cpp)
    target_compile_definitions(zenith_audio_tests PRIVATE ZENITH_ASSET_DIR="${ZENITH_ASSET_BINARY_DIR}")
    add_dependencies(zenith_audio_tests zenith_assets)
    target_link_libraries(zenith_audio_tests PRIVATE zenith_engine)
    if(MSVC)
        target_compile_options(zenith_audio_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_audio_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_audio COMMAND zenith_audio_tests)

    add_executable(zenith_asset_tests
        tests/engine/AssetSceneTests.cpp
    )
    target_compile_definitions(zenith_asset_tests PRIVATE ZENITH_ASSET_DIR="${ZENITH_ASSET_BINARY_DIR}")
    add_dependencies(zenith_asset_tests zenith_assets)
    target_link_libraries(zenith_asset_tests PRIVATE zenith_engine)
    if(MSVC)
        target_compile_options(zenith_asset_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_asset_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_asset_scene COMMAND zenith_asset_tests)

    add_executable(zenith_compressed_texture_tests tests/engine/CompressedTextureTests.cpp)
    target_link_libraries(zenith_compressed_texture_tests PRIVATE zenith_engine)
    if(MSVC)
        target_compile_options(zenith_compressed_texture_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_compressed_texture_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_compressed_texture COMMAND zenith_compressed_texture_tests)

    add_executable(zenith_animation_tests tests/engine/AnimationTests.cpp)
    target_link_libraries(zenith_animation_tests PRIVATE zenith_engine)
    if(MSVC)
        target_compile_options(zenith_animation_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_animation_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_animation COMMAND zenith_animation_tests)

    add_executable(zenith_physics_tests tests/engine/PhysicsTests.cpp)
    target_compile_definitions(zenith_physics_tests PRIVATE ZENITH_ASSET_DIR="${ZENITH_ASSET_BINARY_DIR}")
    add_dependencies(zenith_physics_tests zenith_assets)
    target_link_libraries(zenith_physics_tests PRIVATE zenith_engine)
    if(MSVC)
        target_compile_options(zenith_physics_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_physics_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_physics COMMAND zenith_physics_tests)

    add_executable(zenith_application_tests tests/game/ApplicationLifecycleTests.cpp)
    target_compile_definitions(zenith_application_tests PRIVATE ZENITH_ASSET_DIR="${ZENITH_ASSET_BINARY_DIR}")
    add_dependencies(zenith_application_tests zenith_assets)
    target_link_libraries(zenith_application_tests PRIVATE zenith_demo)
    if(MSVC)
        target_compile_options(zenith_application_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_application_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_application_lifecycle COMMAND zenith_application_tests)

    add_executable(zenith_game_settings_tests tests/game/GameSettingsTests.cpp)
    target_link_libraries(zenith_game_settings_tests PRIVATE zenith_demo)
    if(MSVC)
        target_compile_options(zenith_game_settings_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_game_settings_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_game_settings COMMAND zenith_game_settings_tests)

    add_executable(zenith_editor_settings_tests
        tests/editor/EditorSettingsTests.cpp
        src/editor/settings/EditorSettings.cpp
    )
    target_include_directories(zenith_editor_settings_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    if(MSVC)
        target_compile_options(zenith_editor_settings_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_editor_settings_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_editor_settings COMMAND zenith_editor_settings_tests)

    add_executable(zenith_scene_document_tests tests/engine/SceneDocumentTests.cpp)
    target_compile_definitions(zenith_scene_document_tests PRIVATE
        ZENITH_ASSET_DIR="${ZENITH_ASSET_BINARY_DIR}"
    )
    add_dependencies(zenith_scene_document_tests zenith_assets)
    target_link_libraries(zenith_scene_document_tests PRIVATE zenith_engine)
    if(MSVC)
        target_compile_options(zenith_scene_document_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_scene_document_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_scene_document COMMAND zenith_scene_document_tests)

    add_executable(zenith_project_config_tests tests/engine/ProjectConfigTests.cpp)
    target_link_libraries(zenith_project_config_tests PRIVATE zenith_engine)
    if(MSVC)
        target_compile_options(zenith_project_config_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_project_config_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_project_config COMMAND zenith_project_config_tests)

    add_executable(zenith_input_action_tests tests/engine/InputActionTests.cpp)
    target_link_libraries(zenith_input_action_tests PRIVATE zenith_engine)
    if(MSVC)
        target_compile_options(zenith_input_action_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_input_action_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_input_actions COMMAND zenith_input_action_tests)

    add_executable(zenith_game_ui_tests tests/engine/GameUiTests.cpp)
    target_link_libraries(zenith_game_ui_tests PRIVATE zenith_engine)
    if(MSVC)
        target_compile_options(zenith_game_ui_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_game_ui_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_game_ui COMMAND zenith_game_ui_tests)

    add_executable(zenith_editor_document_tests
        tests/editor/EditorDocumentTests.cpp
        src/editor/document/EditorDocument.cpp
    )
    target_include_directories(zenith_editor_document_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(zenith_editor_document_tests PRIVATE zenith_engine)
    if(MSVC)
        target_compile_options(zenith_editor_document_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_editor_document_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_editor_document COMMAND zenith_editor_document_tests)

    add_executable(zenith_scene_hierarchy_tests tests/engine/SceneHierarchyTests.cpp)
    target_link_libraries(zenith_scene_hierarchy_tests PRIVATE zenith_engine)
    if(MSVC)
        target_compile_options(zenith_scene_hierarchy_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_scene_hierarchy_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_scene_hierarchy COMMAND zenith_scene_hierarchy_tests)

    add_executable(zenith_render_submission_tests tests/engine/RenderSubmissionTests.cpp)
    target_link_libraries(zenith_render_submission_tests PRIVATE zenith_engine)
    add_test(NAME zenith_render_submission COMMAND zenith_render_submission_tests)

    add_executable(zenith_gpu_resource_table_tests tests/engine/GpuResourceTableTests.cpp)
    target_link_libraries(zenith_gpu_resource_table_tests PRIVATE zenith_engine)
    add_test(NAME zenith_gpu_resource_table COMMAND zenith_gpu_resource_table_tests)

    add_executable(zenith_mesh_lod_tests tests/engine/MeshLodTests.cpp)
    target_link_libraries(zenith_mesh_lod_tests PRIVATE zenith_engine)
    add_test(NAME zenith_mesh_lod COMMAND zenith_mesh_lod_tests)

    add_executable(zenith_packed_vertex_stream_tests tests/engine/PackedVertexStreamTests.cpp)
    target_link_libraries(zenith_packed_vertex_stream_tests PRIVATE zenith_engine)
    add_test(NAME zenith_packed_vertex_streams COMMAND zenith_packed_vertex_stream_tests)

    add_executable(zenith_depth_prepass_tests tests/engine/DepthPrepassTests.cpp)
    target_link_libraries(zenith_depth_prepass_tests PRIVATE zenith_engine)
    add_test(NAME zenith_depth_prepass COMMAND zenith_depth_prepass_tests)

    add_executable(zenith_screen_space_reflection_tests tests/engine/ScreenSpaceReflectionTests.cpp)
    target_link_libraries(zenith_screen_space_reflection_tests PRIVATE zenith_engine)
    add_test(NAME zenith_screen_space_reflections COMMAND zenith_screen_space_reflection_tests)

    add_executable(zenith_ambient_occlusion_tests tests/engine/AmbientOcclusionTests.cpp)
    target_link_libraries(zenith_ambient_occlusion_tests PRIVATE zenith_engine)
    add_test(NAME zenith_ambient_occlusion COMMAND zenith_ambient_occlusion_tests)

    add_executable(zenith_clustered_lighting_tests tests/engine/ClusteredLightingTests.cpp)
    target_link_libraries(zenith_clustered_lighting_tests PRIVATE zenith_engine)
    add_test(NAME zenith_clustered_lighting COMMAND zenith_clustered_lighting_tests)

    add_executable(zenith_local_shadow_tests tests/engine/LocalShadowTests.cpp)
    target_link_libraries(zenith_local_shadow_tests PRIVATE zenith_engine)
    add_test(NAME zenith_local_shadows COMMAND zenith_local_shadow_tests)

    add_executable(zenith_frame_task_graph_tests tests/engine/FrameTaskGraphTests.cpp)
    target_link_libraries(zenith_frame_task_graph_tests PRIVATE zenith_engine)
    add_test(NAME zenith_frame_task_graph COMMAND zenith_frame_task_graph_tests)

    add_executable(zenith_render_graph_tests tests/engine/RenderGraphTests.cpp)
    target_link_libraries(zenith_render_graph_tests PRIVATE zenith_engine)
    add_test(NAME zenith_render_graph COMMAND zenith_render_graph_tests)

    add_executable(zenith_temporal_quality_tests tests/engine/TemporalQualityTests.cpp)
    target_link_libraries(zenith_temporal_quality_tests PRIVATE zenith_engine)
    add_test(NAME zenith_temporal_quality COMMAND zenith_temporal_quality_tests)

    add_executable(zenith_tone_mapping_tests tests/engine/ToneMappingTests.cpp)
    target_link_libraries(zenith_tone_mapping_tests PRIVATE zenith_engine)
    add_test(NAME zenith_tone_mapping COMMAND zenith_tone_mapping_tests)

    add_executable(zenith_environment_lighting_tests tests/engine/EnvironmentLightingTests.cpp)
    target_link_libraries(zenith_environment_lighting_tests PRIVATE zenith_engine)
    add_test(NAME zenith_environment_lighting COMMAND zenith_environment_lighting_tests)

    add_executable(zenith_asset_streaming_tests tests/engine/AssetStreamingTests.cpp)
    target_link_libraries(zenith_asset_streaming_tests PRIVATE zenith_engine)
    add_test(NAME zenith_asset_streaming COMMAND zenith_asset_streaming_tests)

    add_executable(zenith_render_regression_tests tests/engine/RenderRegressionTests.cpp)
    add_test(NAME zenith_render_regression COMMAND zenith_render_regression_tests)

    add_executable(zenith_render_packet_benchmark tests/engine/RenderPacketBenchmarkTests.cpp)
    target_link_libraries(zenith_render_packet_benchmark PRIVATE zenith_engine)

    add_executable(zenith_simulation_benchmark tests/engine/SimulationBenchmark.cpp)
    target_link_libraries(zenith_simulation_benchmark PRIVATE zenith_engine)

    add_executable(zenith_benchmark_tests tests/engine/BenchmarkTests.cpp)
    target_link_libraries(zenith_benchmark_tests PRIVATE zenith_engine)
    add_test(NAME zenith_benchmark COMMAND zenith_benchmark_tests)

    add_executable(zenith_component_editor_tests tests/editor/ComponentEditorTests.cpp)
    target_link_libraries(zenith_component_editor_tests PRIVATE zenith_editor_app)
    if(MSVC)
        target_compile_options(zenith_component_editor_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_component_editor_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_component_editor COMMAND zenith_component_editor_tests)

    add_executable(zenith_command_history_tests
        tests/editor/CommandHistoryTests.cpp
        src/editor/document/CommandHistory.cpp
        src/editor/document/PrefabAuthoring.cpp
    )
    target_include_directories(zenith_command_history_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(zenith_command_history_tests PRIVATE zenith_engine)
    if(MSVC)
        target_compile_options(zenith_command_history_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_command_history_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_command_history COMMAND zenith_command_history_tests)

    add_executable(zenith_editor_camera_tests tests/editor/EditorCameraTests.cpp)
    target_compile_definitions(zenith_editor_camera_tests PRIVATE ZENITH_ASSET_DIR="${ZENITH_ASSET_BINARY_DIR}")
    add_dependencies(zenith_editor_camera_tests zenith_assets)
    target_link_libraries(zenith_editor_camera_tests PRIVATE zenith_editor_app)
    if(MSVC)
        target_compile_options(zenith_editor_camera_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_editor_camera_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_editor_camera COMMAND zenith_editor_camera_tests)

    add_executable(zenith_project_asset_tests
        tests/editor/ProjectAssetTests.cpp
        src/editor/project/AssetCatalog.cpp
        src/editor/project/ReflectionProbeBaker.cpp
    )
    target_include_directories(zenith_project_asset_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(zenith_project_asset_tests PRIVATE zenith_engine)
    if(MSVC)
        target_compile_options(zenith_project_asset_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_project_asset_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_project_assets COMMAND zenith_project_asset_tests)

    add_executable(zenith_editor_play_mode_tests
        tests/editor/EditorPlayModeTests.cpp
        src/editor/application/EditorPlayMode.cpp
    )
    target_include_directories(zenith_editor_play_mode_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(zenith_editor_play_mode_tests PRIVATE zenith_engine)
    if(MSVC)
        target_compile_options(zenith_editor_play_mode_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_editor_play_mode_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_editor_play_mode COMMAND zenith_editor_play_mode_tests)

    add_executable(zenith_editor_hardening_tests tests/editor/EditorHardeningTests.cpp)
    target_link_libraries(zenith_editor_hardening_tests PRIVATE zenith_editor_app)
    if(MSVC)
        target_compile_options(zenith_editor_hardening_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_editor_hardening_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_editor_hardening COMMAND zenith_editor_hardening_tests)

    add_executable(zenith_editor_render_settings_tests tests/editor/EditorRenderSettingsTests.cpp)
    target_link_libraries(zenith_editor_render_settings_tests PRIVATE zenith_editor_app)
    if(MSVC)
        target_compile_options(zenith_editor_render_settings_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_editor_render_settings_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_editor_render_settings COMMAND zenith_editor_render_settings_tests)

    add_executable(zenith_native_file_dialog_tests tests/editor/NativeFileDialogTests.cpp)
    target_link_libraries(zenith_native_file_dialog_tests PRIVATE zenith_editor_app)
    if(MSVC)
        target_compile_options(zenith_native_file_dialog_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_native_file_dialog_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_native_file_dialog COMMAND zenith_native_file_dialog_tests)

    add_executable(zenith_default_scene_tests tests/editor/DefaultSceneTests.cpp)
    target_compile_definitions(zenith_default_scene_tests PRIVATE
        ZENITH_ASSET_DIR="${ZENITH_ASSET_BINARY_DIR}"
    )
    add_dependencies(zenith_default_scene_tests zenith_assets)
    target_link_libraries(zenith_default_scene_tests PRIVATE zenith_editor_app)
    if(MSVC)
        target_compile_options(zenith_default_scene_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_default_scene_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_default_scene COMMAND zenith_default_scene_tests)

    add_executable(zenith_modal_transform_tests tests/editor/ModalTransformTests.cpp)
    target_link_libraries(zenith_modal_transform_tests PRIVATE zenith_editor_app)
    if(MSVC)
        target_compile_options(zenith_modal_transform_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_modal_transform_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_modal_transform COMMAND zenith_modal_transform_tests)

    add_test(NAME zenith_editor_vulkan_smoke COMMAND zenith_editor)
    set_tests_properties(zenith_editor_vulkan_smoke PROPERTIES
        ENVIRONMENT "ZENITH_EDITOR_SMOKE=1;ZENITH_SMOKE_FRAMES=600"
        LABELS "vulkan;smoke"
        TIMEOUT 45
    )
endif()
