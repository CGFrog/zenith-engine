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

# zenith_engine is a shared library so an executable and the game modules it
# loads share one copy of the engine and of its third-party state. Static
# third-party libraries linked into it must be position independent, and every
# runtime binary lands in one directory so executables, tests, and the loader
# find the engine and SDL libraries beside them (per configuration with
# multi-config generators).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
if(UNIX AND NOT APPLE)
    set(CMAKE_INSTALL_RPATH "$ORIGIN")
endif()

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
set(ZENITH_PIXEL_FONT_DIR "${CMAKE_CURRENT_BINARY_DIR}/_deps/pixel-fonts")
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

add_library(zenith_engine SHARED
    src/engine/diagnostics/Benchmark.cpp
    src/engine/diagnostics/FrameCaptureConfig.cpp
    src/engine/diagnostics/ImageComparison.cpp
    src/engine/diagnostics/Logger.cpp
    src/engine/diagnostics/RgbaImage.cpp
    src/engine/diagnostics/StbImageWrite.cpp
    src/engine/animation/AnimationSystem.cpp
    src/engine/audio/AudioSystem.cpp
    src/engine/audio/StbVorbis.cpp
    src/engine/Engine.cpp
    src/engine/application/Application.cpp
    src/engine/application/SimulationClock.cpp
    src/engine/assets/Assets.cpp
    src/engine/assets/ModelImporter.cpp
    src/engine/assets/MeshOptimization.cpp
    src/engine/assets/ImageDecoder.cpp
    src/engine/assets/ProductionAssetPipeline.cpp
    src/engine/assets/CompressedTexture.cpp
    src/engine/assets/TextureCooker.cpp
    src/engine/assets/AssetStreaming.cpp
    src/engine/assets/TextureMipStreaming.cpp
    src/engine/assets/PackedVertexStreams.cpp
    src/engine/assets/IrradianceVolumeAsset.cpp
    src/engine/assets/ColorLut.cpp
    src/engine/assets/ContentHash.cpp
    src/engine/assets/AssetGuid.cpp
    src/engine/assets/AssetMeta.cpp
    src/engine/assets/AssetDatabase.cpp
    src/engine/graphics/Renderer.cpp
    src/engine/graphics/GraphicsValidation.cpp
    src/engine/graphics/Lighting.cpp
    src/engine/graphics/DeferredUploadTracker.cpp
    src/engine/graphics/LocalShadows.cpp
    src/engine/graphics/ReflectionProbes.cpp
    src/engine/graphics/Decals.cpp
    src/engine/graphics/EnvironmentLighting.cpp
    src/engine/graphics/IrradianceVolume.cpp
    src/engine/graphics/RenderConfiguration.cpp
    src/engine/graphics/RenderSubmission.cpp
    src/engine/graphics/MeshLodSelection.cpp
    src/engine/graphics/MeshPassFramePlan.cpp
    src/engine/graphics/ViewShading.cpp
    src/engine/graphics/Rendering.cpp
    src/engine/graphics/DebugDraw.cpp
    src/engine/graphics/RenderPicking.cpp
    src/engine/graphics/RenderViewPlanning.cpp
    src/engine/graphics/GpuCullLayout.cpp
    src/engine/graphics/TemporalQuality.cpp
    src/engine/graphics/ToneMapping.cpp
    src/engine/graphics/AmbientOcclusion.cpp
    src/engine/graphics/VolumetricFog.cpp
    src/engine/graphics/MotionBlur.cpp
    src/engine/graphics/DepthOfField.cpp
    src/engine/graphics/ColorGrading.cpp
    src/engine/graphics/SpatialUpscale.cpp
    src/engine/graphics/Particles.cpp
    src/engine/graphics/MaterialRecord.cpp
    src/engine/graphics/graph/RenderGraph.cpp
    src/engine/graphics/passes/MeshPass.cpp
    src/engine/graphics/passes/GpuDrivenPass.cpp
    src/engine/graphics/passes/IdBufferPass.cpp
    src/engine/graphics/passes/DepthPyramid.cpp
    src/engine/graphics/passes/AmbientOcclusionPass.cpp
    src/engine/graphics/passes/ClusteredLightingPass.cpp
    src/engine/graphics/passes/LocalShadowPass.cpp
    src/engine/graphics/passes/ReflectionProbeClusterPass.cpp
    src/engine/graphics/passes/DecalClusterPass.cpp
    src/engine/graphics/passes/VolumetricFogPass.cpp
    src/engine/graphics/passes/MotionBlurPass.cpp
    src/engine/graphics/passes/ParticlePass.cpp
    src/engine/graphics/passes/EnvironmentPass.cpp
    src/engine/graphics/passes/IrradianceVolumePass.cpp
    src/engine/graphics/passes/PostProcessPass.cpp
    src/engine/graphics/passes/ScreenSpaceReflectionPass.cpp
    src/engine/graphics/passes/TemporalResolvePass.cpp
    src/engine/graphics/passes/DepthOfFieldPass.cpp
    src/engine/graphics/passes/SpatialUpscalePass.cpp
    src/engine/graphics/passes/TransmissionBackground.cpp
    src/engine/graphics/passes/BloomExposurePass.cpp
    src/engine/graphics/passes/ColorGradePass.cpp
    src/engine/graphics/passes/UiPass.cpp
    src/engine/graphics/passes/DebugDrawPass.cpp
    src/engine/graphics/pipeline/GraphicsPipeline.cpp
    src/engine/graphics/pipeline/ComputePipeline.cpp
    src/engine/graphics/resources/FrameResources.cpp
    src/engine/graphics/resources/UploadContext.cpp
    src/engine/graphics/resources/BindlessTextureTable.cpp
    src/engine/graphics/resources/GeometryArena.cpp
    src/engine/graphics/resources/IdReadback.cpp
    src/engine/graphics/resources/ImageReadback.cpp
    src/engine/graphics/resources/UiImageStore.cpp
    src/engine/graphics/resources/ViewportTarget.cpp
    src/engine/graphics/resources/MipChainBlit.cpp
    src/engine/graphics/resources/DescriptorPoolSizes.cpp
    src/engine/graphics/shader/ShaderModule.cpp
    src/engine/graphics/surface/Surface.cpp
    src/engine/graphics/swapchain/Swapchain.cpp
    src/engine/graphics/vulkan/VulkanContext.cpp
    src/engine/input/InputActions.cpp
    src/engine/input/InputManager.cpp
    src/engine/input/InputProfiles.cpp
    src/engine/jobs/JobSystem.cpp
    src/engine/jobs/FrameTaskGraph.cpp
    src/engine/module/GameModule.cpp
    src/engine/module/ModuleApi.cpp
    src/engine/module/ModuleRegistrar.cpp
    src/engine/module/ModuleShadowCopy.cpp
    src/engine/module/SharedLibrary.cpp
    src/engine/module/SystemScheduler.cpp
    src/engine/physics/PhysicsWorld.cpp
    src/engine/physics/JoltDebugDraw.cpp
    src/engine/physics/JoltRuntime.cpp
    src/engine/platform/FileWatcher.cpp
    src/engine/platform/ChildProcess.cpp
    src/engine/platform/FileContents.cpp
    src/engine/platform/PlatformServices.cpp
    src/engine/project/ProjectConfig.cpp
    src/engine/reflection/FieldJson.cpp
    src/engine/reflection/Reflection.cpp
    src/engine/resources/ResourceLocator.cpp
    src/engine/resources/ContentManifest.cpp
    src/engine/scene/Scene.cpp
    src/engine/scene/ParticleEmitter.cpp
    src/engine/scene/BuiltInComponentTypes.cpp
    src/engine/scene/CollectionComponentTypes.cpp
    src/engine/scene/Collections.cpp
    src/engine/scene/ComponentTypes.cpp
    src/engine/scene/SceneAssetReferences.cpp
    src/engine/scene/SceneDocument.cpp
    src/engine/scene/EntityReferenceRemap.cpp
    src/engine/scene/SceneVisibility.cpp
    src/engine/scene/AdditiveSceneManager.cpp
    src/engine/scene/Prefab.cpp
    src/engine/scene/PrefabComponentTypes.cpp
    src/engine/scene/PrefabSystem.cpp
    src/engine/ui/GameUi.cpp
    src/engine/ui/Accessibility.cpp
    src/engine/ui/Localization.cpp
    src/engine/window/Window.cpp
)

if(MSVC)
    set_source_files_properties(src/engine/audio/StbVorbis.cpp PROPERTIES COMPILE_OPTIONS /W0)
    set_source_files_properties(src/engine/diagnostics/StbImageWrite.cpp PROPERTIES COMPILE_OPTIONS /W0)
endif()

# The demo's gameplay: component types and systems registered through
# registerDemoGameplay. Linked into the zenith_game_demo module the editor
# loads and statically into the zenith executable, so both run the same
# registration.
add_library(zenith_demo_gameplay STATIC
    src/game/gameplay/DemoGameplay.cpp
    src/game/gameplay/ItemCollection.cpp
    src/game/input/DemoInput.cpp
    src/game/player/FirstPersonController.cpp
)
set_target_properties(zenith_demo_gameplay PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(zenith_demo_gameplay PUBLIC zenith_engine)

# The demo game module. Each build also copies it to
# projects/modules/zenith_game_demo.zmodule, the platform-independent,
# project-relative path projects/demo.zenith-project names, so the editor can
# open the checked-in project from any build directory. The copy is the most
# recently built configuration; a module built for another configuration is
# refused by its build hash. Release trees made by Build and Package turn the
# copy off, so they never replace the module the editor has loaded.
add_library(zenith_game_demo MODULE src/game/module/DemoGameModule.cpp)
target_link_libraries(zenith_game_demo PRIVATE zenith_demo_gameplay)
option(ZENITH_COPY_PROJECT_MODULE "Copy the built demo module beside projects/demo.zenith-project" ON)
if(ZENITH_COPY_PROJECT_MODULE)
    set(ZENITH_DEMO_PROJECT_MODULE "${CMAKE_CURRENT_SOURCE_DIR}/projects/modules/zenith_game_demo.zmodule")
    add_custom_command(TARGET zenith_game_demo POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/projects/modules"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:zenith_game_demo>" "${ZENITH_DEMO_PROJECT_MODULE}"
        VERBATIM)
endif()

# Application composition for the zenith executable: menus, settings, audio,
# and scene switching around the linked gameplay.
add_library(zenith_demo STATIC
    src/game/application/DemoApplication.cpp
    src/game/application/GameSettings.cpp
    src/game/application/LinkedGameplay.cpp
    src/game/scene/CaptureCamera.cpp
    src/game/scene/DemoScene.cpp
    src/game/scene/GameplayScene.cpp
)
target_link_libraries(zenith_demo PUBLIC zenith_demo_gameplay)

add_executable(zenith src/main.cpp)
target_link_libraries(zenith PRIVATE zenith_demo)
# The game links its gameplay statically, but the module is built with it so a
# build of the game also refreshes the module the demo project names.
add_dependencies(zenith zenith_game_demo)
if(APPLE)
    set_target_properties(zenith PROPERTIES MACOSX_BUNDLE TRUE
        OUTPUT_NAME "Zenith"
        MACOSX_BUNDLE_BUNDLE_NAME "Zenith"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.zenith.demo"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
endif()

add_library(zenith_editor_app STATIC
    src/editor/application/EditorApplication.cpp
    src/editor/application/EditorAreaTypes.cpp
    src/editor/application/EditorAreas.cpp
    src/editor/application/EditorDiagnostics.cpp
    src/editor/application/EditorModuleCommands.cpp
    src/editor/application/EditorObjectCommands.cpp
    src/editor/application/EditorPickingSmoke.cpp
    src/editor/application/EditorViewports.cpp
    src/editor/application/EditorPlayMode.cpp
    src/editor/application/EditorPreferences.cpp
    src/editor/application/NativeFileDialog.cpp
    src/editor/application/EditorSession.cpp
    src/editor/application/EntityCommands.cpp
    src/editor/application/SceneEditing.cpp
    src/editor/application/SystemClipboard.cpp
    src/editor/areas/AreaRegistry.cpp
    src/editor/areas/AreaWindows.cpp
    src/editor/areas/DefaultWorkspaces.cpp
    src/editor/areas/Workspace.cpp
    src/editor/areas/WorkspaceFile.cpp
    src/editor/document/CommandHistory.cpp
    src/editor/document/DefaultScene.cpp
    src/editor/document/EditorDocument.cpp
    src/editor/document/EditorRecovery.cpp
    src/editor/document/PrefabAuthoring.cpp
    src/editor/document/SceneEditorData.cpp
    src/editor/document/SceneFragment.cpp
    src/editor/interaction/ComponentEditors.cpp
    src/editor/interaction/EditorCamera.cpp
    src/editor/interaction/EntityEyedropper.cpp
    src/editor/interaction/EntityPropertiesView.cpp
    src/editor/interaction/EntityReferenceField.cpp
    src/editor/interaction/ModalTransform.cpp
    src/editor/interaction/PickQueries.cpp
    src/editor/interaction/GraphicsSettingsEditor.cpp
    src/editor/interaction/ReflectedFieldEditor.cpp
    src/editor/interaction/Selection.cpp
    src/editor/menus/AddComponentMenu.cpp
    src/editor/menus/DefaultMenus.cpp
    src/editor/menus/EditorMenus.cpp
    src/editor/menus/MenuPopup.cpp
    src/editor/menus/MenuRegistry.cpp
    src/editor/menus/PieMenu.cpp
    src/editor/modules/GameModuleHost.cpp
    src/editor/operators/ComponentOperators.cpp
    src/editor/operators/CollectionOperators.cpp
    src/editor/operators/DefaultKeymap.cpp
    src/editor/operators/Keymap.cpp
    src/editor/operators/KeymapDispatch.cpp
    src/editor/operators/KeymapEditor.cpp
    src/editor/operators/LastOperatorPanel.cpp
    src/editor/operators/EyedropperOperators.cpp
    src/editor/operators/ObjectOperators.cpp
    src/editor/operators/PrefabOperators.cpp
    src/editor/operators/Operator.cpp
    src/editor/operators/OperatorHistory.cpp
    src/editor/operators/OperatorRegistry.cpp
    src/editor/operators/OperatorRunner.cpp
    src/editor/operators/OperatorSearch.cpp
    src/editor/operators/PlayOperators.cpp
    src/editor/operators/OutlinerOperators.cpp
    src/editor/operators/PackageOperators.cpp
    src/editor/operators/PreferencesOperators.cpp
    src/editor/operators/VisibilityOperators.cpp
    src/editor/panels/AssetBrowserPanel.cpp
    src/editor/panels/BuildPanel.cpp
    src/editor/panels/DiagnosticsPanel.cpp
    src/editor/panels/EditorContext.cpp
    src/editor/panels/GameModulePanel.cpp
    src/editor/panels/GraphicsPanel.cpp
    src/editor/panels/HierarchyPanel.cpp
    src/editor/panels/InfoPanel.cpp
    src/editor/panels/InspectorPanel.cpp
    src/editor/panels/PreferencesWindow.cpp
    src/editor/panels/PropertiesPanel.cpp
    src/editor/panels/RenderSettingsControls.cpp
    src/editor/panels/RenderStatisticsPanel.cpp
    src/editor/panels/SystemProfilerPanel.cpp
    src/editor/panels/ViewportChrome.cpp
    src/editor/panels/ViewportOperators.cpp
    src/editor/panels/ViewportPanel.cpp
    src/editor/panels/ViewportRegistry.cpp
    src/editor/panels/outliner/OrphanData.cpp
    src/editor/panels/outliner/OutlinerRowWidgets.cpp
    src/editor/panels/outliner/OutlinerTree.cpp
    src/editor/project/AssetCatalog.cpp
    src/editor/project/AssetRelocation.cpp
    src/editor/project/BuildRunner.cpp
    src/editor/project/CompilerOutputParser.cpp
    src/editor/project/DerivedAssetInspector.cpp
    src/editor/project/ExternalEditor.cpp
    src/editor/project/IrradianceVolumeBaker.cpp
    src/editor/project/PackageRunner.cpp
    src/editor/project/ProjectAssets.cpp
    src/editor/project/ProjectFileOperations.cpp
    src/editor/project/ProjectHub.cpp
    src/editor/project/ProjectTemplate.cpp
    src/editor/project/RecentProjects.cpp
    src/editor/project/ReflectionProbeBaker.cpp
    src/editor/project/ThumbnailCache.cpp
    src/editor/settings/AssetBrowserSettings.cpp
    src/editor/settings/ComponentInspectorLayout.cpp
    src/editor/settings/EditorRenderSettings.cpp
    src/editor/settings/EditorSettings.cpp
    src/editor/settings/RenderSettingsController.cpp
    src/editor/settings/SimulateSystemSettings.cpp
    src/editor/theme/EditorPalette.cpp
    src/editor/theme/EditorTheme.cpp
    src/editor/theme/EditorWidgets.cpp
    src/editor/theme/EntityIcon.cpp
    src/editor/theme/GlassBackdrop.cpp
    src/editor/theme/PixelIconBitmaps.cpp
    src/editor/theme/PixelIcons.cpp
    src/editor/theme/PixelPainter.cpp
    src/editor/theme/PixelWallpaper.cpp
    src/editor/theme/ThemeChrome.cpp
    src/editor/viewport/SceneGizmoDraw.cpp
    src/editor/viewport/BoxSelect.cpp
    src/editor/viewport/Cursor3D.cpp
    src/editor/viewport/LassoSelect.cpp
    src/editor/viewport/RegionSelect.cpp
    src/editor/viewport/SnapQueries.cpp
    src/editor/viewport/ViewFraming.cpp
    src/editor/viewport/ViewportPickProvider.cpp
    src/editor/viewport/ViewportPicker.cpp
    src/editor/viewport/ViewportSelectOperators.cpp
    src/editor/theme/ThemeParameters.cpp
)
target_link_libraries(zenith_editor_app PUBLIC zenith_engine)
if(WIN32)
    # Shell file operations (Recycle Bin, Explorer) in ProjectFileOperations.
    target_link_libraries(zenith_editor_app PRIVATE ole32 shell32)
endif()
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
        "${ZENITH_PIXEL_FONT_DIR}/PressStart2P-Regular.ttf"
        "${ZENITH_PIXEL_FONT_DIR}/PressStart2P-OFL.txt"
        "${ZENITH_PIXEL_FONT_DIR}/VT323-Regular.ttf"
        "${ZENITH_PIXEL_FONT_DIR}/VT323-OFL.txt"
        "${ZENITH_EDITOR_FONT_BINARY_DIR}"
    COMMENT "Copying Zenith editor resources"
)
add_dependencies(zenith_editor zenith_editor_resources)

# Project templates for the editor's Project Hub (New from Template). The
# editor looks in <resource root>/templates, where the build copies them, and
# then in the source tree's templates/. Generated projects build their game
# module against this build directory (ZenithEngineConfig.cmake below) in the
# editor's own configuration.
set(ZENITH_TEMPLATE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/templates")
add_custom_target(zenith_editor_templates
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${ZENITH_TEMPLATE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/templates"
    COMMENT "Copying Zenith project templates"
)
add_dependencies(zenith_editor zenith_editor_templates)
target_compile_definitions(zenith_editor PRIVATE
    ZENITH_SOURCE_TEMPLATE_DIR="${ZENITH_TEMPLATE_SOURCE_DIR}"
    ZENITH_ENGINE_BUILD_DIR="${CMAKE_CURRENT_BINARY_DIR}"
    ZENITH_EDITOR_CONFIGURATION="$<CONFIG>"
)

# The engine as an imported target for game modules built outside this tree
# (a project generated from templates/cpp_game includes
# <build>/ZenithEngineConfig.cmake and links Zenith::Engine): the engine's
# usage requirements (include directories and definitions, evaluated
# transitively) and, per configuration, the engine and SDL libraries.
# zenith-engine.json records the generator and compiler, which a generated
# project's CMakePresets.json repeats so its module's build hash matches.
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ZenithEngineConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/ZenithEngineConfig.cmake.genex" @ONLY)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ZenithEngineConfig.cmake"
    INPUT "${CMAKE_CURRENT_BINARY_DIR}/ZenithEngineConfig.cmake.genex" TARGET zenith_engine)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ZenithEngineTargets-$<CONFIG>.cmake"
    INPUT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ZenithEngineTargets.cmake.in" TARGET zenith_engine)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/zenith-engine.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/zenith-engine.json" @ONLY)

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

# The project a release package ships (its project file, startup scene, game
# module, and assets). Build and Package (file.build_package) configures its
# release tree with the editor's open project.
set(ZENITH_PACKAGE_PROJECT "${CMAKE_CURRENT_SOURCE_DIR}/projects/demo.zenith-project" CACHE FILEPATH
    "The .zenith-project a release package ships")
include(cmake/PackageProject.cmake)
zenith_read_package_project("${ZENITH_PACKAGE_PROJECT}")

# Cook every shipped model's external textures (the engine's assets and the
# packaged project's) to block-compressed KTX2 in the derived-data cache the
# executables read from <resource root>/derived. Entries are keyed by source
# bytes, so unchanged textures are skipped on rebuild.
set(ZENITH_DERIVED_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/derived")
set(ZENITH_COOKED_ASSET_DIRS "${ZENITH_ASSET_SOURCE_DIR}")
cmake_path(COMPARE "${ZENITH_PACKAGE_ASSET_DIR}" NOT_EQUAL "${ZENITH_ASSET_SOURCE_DIR}" ZENITH_PACKAGE_HAS_OWN_ASSETS)
if(ZENITH_PACKAGE_HAS_OWN_ASSETS)
    list(APPEND ZENITH_COOKED_ASSET_DIRS "${ZENITH_PACKAGE_ASSET_DIR}")
endif()
set(ZENITH_COOKED_MODELS)
set(ZENITH_COOKED_TEXTURE_SOURCES)
foreach(cooked_asset_dir IN LISTS ZENITH_COOKED_ASSET_DIRS)
    file(GLOB_RECURSE cooked_models CONFIGURE_DEPENDS "${cooked_asset_dir}/*.gltf")
    file(GLOB_RECURSE cooked_textures CONFIGURE_DEPENDS "${cooked_asset_dir}/*.png" "${cooked_asset_dir}/*.jpg")
    list(APPEND ZENITH_COOKED_MODELS ${cooked_models})
    list(APPEND ZENITH_COOKED_TEXTURE_SOURCES ${cooked_textures})
endforeach()
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

set_target_properties(zenith_engine PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
target_compile_definitions(zenith_engine PRIVATE ZENITH_ENGINE_BUILDING)
if(WIN32)
    target_compile_definitions(zenith_engine
        PRIVATE "IMGUI_API=__declspec(dllexport)"
        INTERFACE "IMGUI_API=__declspec(dllimport)")
endif()

target_compile_definitions(zenith_engine PUBLIC VK_NO_PROTOTYPES PRIVATE
    ZENITH_BENCHMARK_REVISION="${ZENITH_SOURCE_REVISION}"
    ZENITH_BENCHMARK_DIRTY="${ZENITH_SOURCE_DIRTY}"
    ZENITH_BENCHMARK_BUILD="$<CONFIG>"
    ZENITH_BENCHMARK_COMPILER="${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}"
    ZENITH_BENCHMARK_PLATFORM="${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}"
)

# Header-only and shared dependencies are public. Static libraries are linked
# privately so their code and globals exist once, inside the engine library;
# consumers receive only the headers of those that engine headers include.
target_link_libraries(zenith_engine PUBLIC
    Vulkan::Headers
    glm::glm
    SDL3::SDL3
    cgltf::cgltf
    stb
    EnTT::EnTT
    nlohmann_json::nlohmann_json
    GPUOpen::VulkanMemoryAllocator
)
target_link_libraries(zenith_engine PRIVATE volk imgui zenith_imguizmo Jolt shaderc zenith_bc7enc meshoptimizer ${CMAKE_DL_LIBS})
foreach(header_dependency IN ITEMS volk imgui zenith_imguizmo)
    target_include_directories(zenith_engine PUBLIC
        $<TARGET_PROPERTY:${header_dependency},INTERFACE_INCLUDE_DIRECTORIES>)
    target_compile_definitions(zenith_engine PUBLIC
        $<TARGET_PROPERTY:${header_dependency},INTERFACE_COMPILE_DEFINITIONS>)
endforeach()

if(MSVC)
    target_compile_options(zenith_engine PRIVATE /W4 /permissive- /FS)
    target_compile_options(zenith_demo PRIVATE /W4 /permissive-)
    target_compile_options(zenith_demo_gameplay PRIVATE /W4 /permissive-)
    target_compile_options(zenith_game_demo PRIVATE /W4 /permissive-)
    target_compile_options(zenith_editor_app PRIVATE /W4 /permissive-)
else()
    target_compile_options(zenith_engine PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(zenith_demo PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(zenith_demo_gameplay PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(zenith_game_demo PRIVATE -Wall -Wextra -Wpedantic)
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
# The engine and SDL shared libraries sit beside the executables. Their import
# libraries and development symlinks are not part of the runtime package.
install(TARGETS zenith_engine SDL3-shared
    RUNTIME DESTINATION .
    LIBRARY DESTINATION . NAMELINK_SKIP
    ARCHIVE DESTINATION lib COMPONENT development EXCLUDE_FROM_ALL
)
install(DIRECTORY "${ZENITH_SHADER_BINARY_DIR}/" DESTINATION shaders FILES_MATCHING PATTERN "*.spv")
install(DIRECTORY "${ZENITH_DERIVED_BINARY_DIR}/" DESTINATION derived FILES_MATCHING PATTERN "*.zderived")
# The runtime's own assets, then the packaged project (ZENITH_PACKAGE_PROJECT):
# its assets merged into the same folder (its packaged assetRoot), the project
# file at the root, and its startup scene and game module at the paths the
# project file names.
install(DIRECTORY "${ZENITH_ASSET_SOURCE_DIR}/" DESTINATION assets)
if(ZENITH_PACKAGE_HAS_OWN_ASSETS)
    install(DIRECTORY "${ZENITH_PACKAGE_ASSET_DIR}/" DESTINATION assets)
endif()
install(FILES "${ZENITH_PACKAGE_PROJECT_INSTALL_FILE}" DESTINATION .)
install(FILES "${ZENITH_PACKAGE_STARTUP_SCENE}" DESTINATION "${ZENITH_PACKAGE_SCENE_DESTINATION}")
if(ZENITH_PACKAGE_MODULE_TARGET)
    install(FILES "$<TARGET_FILE:${ZENITH_PACKAGE_MODULE_TARGET}>" DESTINATION "${ZENITH_PACKAGE_MODULE_DESTINATION}"
        RENAME "${ZENITH_PACKAGE_MODULE_NAME}")
elseif(ZENITH_PACKAGE_MODULE)
    install(FILES "${ZENITH_PACKAGE_MODULE}" DESTINATION "${ZENITH_PACKAGE_MODULE_DESTINATION}"
        RENAME "${ZENITH_PACKAGE_MODULE_NAME}")
endif()
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
    set(ZENITH_PACKAGE_PROJECT_PATHS \"${ZENITH_PACKAGE_PROJECT_PATHS}\")
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
    # meshoptimizer is private to the engine library; this test calls it directly.
    target_link_libraries(zenith_milestone_19_21_tests PRIVATE zenith_engine zenith_demo zenith_editor_app meshoptimizer)
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

    add_executable(zenith_file_watcher_tests tests/engine/FileWatcherTests.cpp)
    target_link_libraries(zenith_file_watcher_tests PRIVATE zenith_engine)
    if(MSVC)
        target_compile_options(zenith_file_watcher_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_file_watcher_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    # One CTest per backend: each half waits on real file-system latency, so
    # they run in parallel rather than back to back.
    foreach(file_watcher_backend IN ITEMS native polling)
        add_test(NAME zenith_file_watcher_${file_watcher_backend}
            COMMAND zenith_file_watcher_tests ${file_watcher_backend})
    endforeach()

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

    add_executable(zenith_asset_database_tests tests/engine/AssetDatabaseTests.cpp)
    target_link_libraries(zenith_asset_database_tests PRIVATE zenith_engine)
    if(MSVC)
        target_compile_options(zenith_asset_database_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_asset_database_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_asset_database COMMAND zenith_asset_database_tests)

    add_executable(zenith_showcase_content_tests tests/engine/ShowcaseContentTests.cpp)
    target_compile_definitions(zenith_showcase_content_tests PRIVATE
        ZENITH_ASSET_DIR="${ZENITH_ASSET_BINARY_DIR}"
        ZENITH_PROJECT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/projects")
    add_dependencies(zenith_showcase_content_tests zenith_assets)
    target_link_libraries(zenith_showcase_content_tests PRIVATE zenith_engine)
    if(MSVC)
        target_compile_options(zenith_showcase_content_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_showcase_content_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_showcase_content COMMAND zenith_showcase_content_tests)

    add_executable(zenith_material_extension_tests tests/engine/MaterialExtensionTests.cpp)
    target_compile_definitions(zenith_material_extension_tests PRIVATE ZENITH_ASSET_DIR="${ZENITH_ASSET_BINARY_DIR}")
    add_dependencies(zenith_material_extension_tests zenith_assets)
    target_link_libraries(zenith_material_extension_tests PRIVATE zenith_engine)
    if(MSVC)
        target_compile_options(zenith_material_extension_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_material_extension_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_material_extensions COMMAND zenith_material_extension_tests)

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

    add_executable(zenith_system_scheduler_tests tests/engine/SystemSchedulerTests.cpp)
    target_link_libraries(zenith_system_scheduler_tests PRIVATE zenith_engine)
    if(MSVC)
        target_compile_options(zenith_system_scheduler_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_system_scheduler_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_system_scheduler COMMAND zenith_system_scheduler_tests)

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

    add_executable(zenith_demo_game_module_tests tests/game/DemoGameModuleTests.cpp)
    target_link_libraries(zenith_demo_game_module_tests PRIVATE zenith_demo)
    target_compile_definitions(zenith_demo_game_module_tests PRIVATE
        ZENITH_DEMO_GAME_MODULE="$<TARGET_FILE:zenith_game_demo>"
        ZENITH_PROJECT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/projects")
    add_dependencies(zenith_demo_game_module_tests zenith_game_demo)
    if(MSVC)
        target_compile_options(zenith_demo_game_module_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_demo_game_module_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_demo_game_module COMMAND zenith_demo_game_module_tests)

    add_executable(zenith_editor_settings_tests
        tests/editor/EditorSettingsTests.cpp
        src/editor/settings/EditorSettings.cpp
        src/editor/theme/ThemeParameters.cpp
    )
    target_include_directories(zenith_editor_settings_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    if(MSVC)
        target_compile_options(zenith_editor_settings_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_editor_settings_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_editor_settings COMMAND zenith_editor_settings_tests)

    add_executable(zenith_editor_palette_tests
        tests/editor/EditorPaletteTests.cpp
        src/editor/theme/EditorPalette.cpp
        src/editor/theme/PixelIconBitmaps.cpp
        src/editor/theme/PixelWallpaper.cpp
        src/editor/theme/ThemeParameters.cpp
    )
    target_include_directories(zenith_editor_palette_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    if(MSVC)
        target_compile_options(zenith_editor_palette_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_editor_palette_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_editor_palette COMMAND zenith_editor_palette_tests)

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

    add_executable(zenith_collection_tests tests/engine/CollectionTests.cpp)
    target_compile_definitions(zenith_collection_tests PRIVATE
        ZENITH_ASSET_DIR="${ZENITH_ASSET_BINARY_DIR}"
    )
    target_link_libraries(zenith_collection_tests PRIVATE zenith_engine)
    if(MSVC)
        target_compile_options(zenith_collection_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_collection_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_collections COMMAND zenith_collection_tests)

    add_executable(zenith_component_types_tests tests/engine/ComponentTypesTests.cpp)
    target_link_libraries(zenith_component_types_tests PRIVATE zenith_engine)
    if(MSVC)
        target_compile_options(zenith_component_types_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_component_types_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_component_types COMMAND zenith_component_types_tests)

    add_executable(zenith_reflection_tests tests/engine/ReflectionTests.cpp)
    target_link_libraries(zenith_reflection_tests PRIVATE zenith_engine)
    if(MSVC)
        target_compile_options(zenith_reflection_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_reflection_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_reflection COMMAND zenith_reflection_tests)

    add_executable(zenith_file_contents_tests tests/engine/FileContentsTests.cpp)
    target_link_libraries(zenith_file_contents_tests PRIVATE zenith_engine)
    if(MSVC)
        target_compile_options(zenith_file_contents_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_file_contents_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_file_contents COMMAND zenith_file_contents_tests)

    add_executable(zenith_input_manager_tests tests/engine/InputManagerTests.cpp)
    target_link_libraries(zenith_input_manager_tests PRIVATE zenith_engine)
    if(MSVC)
        target_compile_options(zenith_input_manager_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_input_manager_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_input_manager COMMAND zenith_input_manager_tests)
    # Skips (77) where SDL's offscreen video driver cannot create a window.
    set_tests_properties(zenith_input_manager PROPERTIES SKIP_RETURN_CODE 77)

    # Game modules for zenith_game_module_tests: a working module, one built for
    # another module API version, and a library without module entry points.
    add_library(zenith_test_game_module MODULE tests/engine/modules/TestGameModule.cpp)
    target_link_libraries(zenith_test_game_module PRIVATE zenith_engine)
    add_library(zenith_test_mismatched_module MODULE tests/engine/modules/MismatchedApiModule.cpp)
    target_include_directories(zenith_test_mismatched_module PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_library(zenith_test_not_a_module MODULE tests/engine/modules/NotAModule.cpp)
    target_include_directories(zenith_test_not_a_module PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_executable(zenith_game_module_tests tests/engine/GameModuleTests.cpp)
    target_link_libraries(zenith_game_module_tests PRIVATE zenith_engine)
    target_compile_definitions(zenith_game_module_tests PRIVATE
        ZENITH_TEST_GAME_MODULE="$<TARGET_FILE:zenith_test_game_module>"
        ZENITH_TEST_MISMATCHED_MODULE="$<TARGET_FILE:zenith_test_mismatched_module>"
        ZENITH_TEST_NOT_A_MODULE="$<TARGET_FILE:zenith_test_not_a_module>")
    if(MSVC)
        target_compile_definitions(zenith_game_module_tests PRIVATE
            ZENITH_TEST_GAME_MODULE_PDB="$<TARGET_PDB_FILE:zenith_test_game_module>")
    endif()
    add_dependencies(zenith_game_module_tests
        zenith_test_game_module zenith_test_mismatched_module zenith_test_not_a_module)
    foreach(module_test_target IN ITEMS zenith_test_game_module zenith_test_mismatched_module
            zenith_test_not_a_module zenith_game_module_tests)
        if(MSVC)
            target_compile_options(${module_test_target} PRIVATE /W4 /permissive-)
        else()
            target_compile_options(${module_test_target} PRIVATE -Wall -Wextra -Wpedantic)
        endif()
    endforeach()
    add_test(NAME zenith_game_module COMMAND zenith_game_module_tests)

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
        src/editor/document/SceneEditorData.cpp
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

    add_executable(zenith_mesh_pass_frame_plan_tests tests/engine/MeshPassFramePlanTests.cpp)
    target_link_libraries(zenith_mesh_pass_frame_plan_tests PRIVATE zenith_engine)
    add_test(NAME zenith_mesh_pass_frame_plan COMMAND zenith_mesh_pass_frame_plan_tests)

    add_executable(zenith_view_shading_tests tests/engine/ViewShadingTests.cpp)
    target_link_libraries(zenith_view_shading_tests PRIVATE zenith_engine)
    add_test(NAME zenith_view_shading COMMAND zenith_view_shading_tests)

    add_executable(zenith_render_view_tests tests/engine/RenderViewTests.cpp)
    target_link_libraries(zenith_render_view_tests PRIVATE zenith_engine)
    add_test(NAME zenith_render_views COMMAND zenith_render_view_tests)

    # Two created render views beside the default one (docs/ARCHITECTURE.md,
    # Render views); needs a Vulkan device like the editor smoke test.
    add_executable(zenith_multi_view_smoke_tests tests/engine/MultiViewSmokeTests.cpp)
    target_link_libraries(zenith_multi_view_smoke_tests PRIVATE zenith_engine)
    add_dependencies(zenith_multi_view_smoke_tests zenith_assets)
    add_test(NAME zenith_multi_view_vulkan_smoke COMMAND zenith_multi_view_smoke_tests)
    set_tests_properties(zenith_multi_view_vulkan_smoke PROPERTIES
        ENVIRONMENT "ZENITH_SMOKE_FRAMES=120"
        LABELS "vulkan;smoke"
        SKIP_RETURN_CODE 77
        TIMEOUT 120
    )

    # Per-view shadow isolation: an isolated view's sun, point and spot shadows
    # cast from its isolated objects only, beside a view of the whole scene.
    add_executable(zenith_isolated_shadow_smoke_tests tests/engine/IsolatedShadowSmokeTests.cpp)
    target_link_libraries(zenith_isolated_shadow_smoke_tests PRIVATE zenith_engine)
    if(MSVC)
        target_compile_options(zenith_isolated_shadow_smoke_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_isolated_shadow_smoke_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_dependencies(zenith_isolated_shadow_smoke_tests zenith_assets)
    add_test(NAME zenith_isolated_shadow_vulkan_smoke COMMAND zenith_isolated_shadow_smoke_tests)
    set_tests_properties(zenith_isolated_shadow_vulkan_smoke PROPERTIES
        ENVIRONMENT "ZENITH_SMOKE_FRAMES=80;ZENITH_DISABLE_SHADOW_CACHE=1;ZENITH_VALIDATION_STRICT=1"
        LABELS "vulkan;smoke"
        SKIP_RETURN_CODE 77
        TIMEOUT 120
        PROCESSORS 2
    )
    # Debug-draw recording, tessellation and label projection (CPU only).
    add_executable(zenith_debug_draw_tests tests/engine/DebugDrawTests.cpp)
    target_link_libraries(zenith_debug_draw_tests PRIVATE zenith_engine)
    if(MSVC)
        target_compile_options(zenith_debug_draw_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_debug_draw_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_debug_draw COMMAND zenith_debug_draw_tests)

    # Debug lines in the default view and two created views under strict
    # validation, plus the fixed primitive set the debug_draw reference set
    # captures (tests/references/debug_draw).
    add_executable(zenith_debug_draw_smoke_tests tests/engine/DebugDrawSmokeTests.cpp)
    target_link_libraries(zenith_debug_draw_smoke_tests PRIVATE zenith_engine)
    add_dependencies(zenith_debug_draw_smoke_tests zenith_assets)
    add_test(NAME zenith_debug_draw_vulkan_smoke COMMAND zenith_debug_draw_smoke_tests)
    set_tests_properties(zenith_debug_draw_vulkan_smoke PROPERTIES
        ENVIRONMENT "ZENITH_SMOKE_FRAMES=60;ZENITH_VALIDATION_STRICT=1"
        LABELS "vulkan;smoke"
        SKIP_RETURN_CODE 77
        TIMEOUT 120
        PROCESSORS 2
    )

    add_executable(zenith_render_picking_tests tests/engine/RenderPickingTests.cpp)
    target_link_libraries(zenith_render_picking_tests PRIVATE zenith_engine)
    if(MSVC)
        target_compile_options(zenith_render_picking_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_render_picking_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_render_picking COMMAND zenith_render_picking_tests)

    # GPU picking through the id buffer (docs/ARCHITECTURE.md, Picking).
    add_executable(zenith_picking_smoke_tests tests/engine/PickingSmokeTests.cpp)
    target_link_libraries(zenith_picking_smoke_tests PRIVATE zenith_engine)
    if(MSVC)
        target_compile_options(zenith_picking_smoke_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_picking_smoke_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_dependencies(zenith_picking_smoke_tests zenith_assets)
    add_test(NAME zenith_picking_vulkan_smoke COMMAND zenith_picking_smoke_tests)
    set_tests_properties(zenith_picking_vulkan_smoke PROPERTIES
        ENVIRONMENT "ZENITH_SMOKE_FRAMES=80"
        LABELS "vulkan;smoke"
        SKIP_RETURN_CODE 77
        TIMEOUT 120
        PROCESSORS 2
    )

    add_executable(zenith_deferred_upload_tracker_tests tests/engine/DeferredUploadTrackerTests.cpp)
    target_link_libraries(zenith_deferred_upload_tracker_tests PRIVATE zenith_engine)
    add_test(NAME zenith_deferred_upload_tracker COMMAND zenith_deferred_upload_tracker_tests)

    add_executable(zenith_gpu_cull_layout_tests tests/engine/GpuCullLayoutTests.cpp)
    target_link_libraries(zenith_gpu_cull_layout_tests PRIVATE zenith_engine)
    add_test(NAME zenith_gpu_cull_layout COMMAND zenith_gpu_cull_layout_tests)

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
    add_executable(zenith_volumetric_fog_tests tests/engine/VolumetricFogTests.cpp)
    target_link_libraries(zenith_volumetric_fog_tests PRIVATE zenith_engine)
    add_test(NAME zenith_volumetric_fog COMMAND zenith_volumetric_fog_tests)

    add_executable(zenith_motion_blur_tests tests/engine/MotionBlurTests.cpp)
    target_link_libraries(zenith_motion_blur_tests PRIVATE zenith_engine)
    add_test(NAME zenith_motion_blur COMMAND zenith_motion_blur_tests)
    add_executable(zenith_depth_of_field_tests tests/engine/DepthOfFieldTests.cpp)
    target_link_libraries(zenith_depth_of_field_tests PRIVATE zenith_engine)
    add_test(NAME zenith_depth_of_field COMMAND zenith_depth_of_field_tests)
    add_executable(zenith_color_grading_tests tests/engine/ColorGradingTests.cpp)
    target_compile_definitions(zenith_color_grading_tests PRIVATE ZENITH_ASSET_DIR="${ZENITH_ASSET_BINARY_DIR}")
    add_dependencies(zenith_color_grading_tests zenith_assets)
    target_link_libraries(zenith_color_grading_tests PRIVATE zenith_engine)
    add_test(NAME zenith_color_grading COMMAND zenith_color_grading_tests)
    add_executable(zenith_particle_tests tests/engine/ParticleTests.cpp)
    target_compile_definitions(zenith_particle_tests PRIVATE ZENITH_ASSET_DIR="${ZENITH_ASSET_BINARY_DIR}")
    add_dependencies(zenith_particle_tests zenith_assets)
    target_link_libraries(zenith_particle_tests PRIVATE zenith_engine)
    add_test(NAME zenith_particles COMMAND zenith_particle_tests)

    add_executable(zenith_frame_task_graph_tests tests/engine/FrameTaskGraphTests.cpp)
    target_link_libraries(zenith_frame_task_graph_tests PRIVATE zenith_engine)
    add_test(NAME zenith_frame_task_graph COMMAND zenith_frame_task_graph_tests)

    add_executable(zenith_render_graph_tests tests/engine/RenderGraphTests.cpp)
    target_link_libraries(zenith_render_graph_tests PRIVATE zenith_engine)
    add_test(NAME zenith_render_graph COMMAND zenith_render_graph_tests)

    add_executable(zenith_temporal_quality_tests tests/engine/TemporalQualityTests.cpp)
    target_link_libraries(zenith_temporal_quality_tests PRIVATE zenith_engine)
    add_test(NAME zenith_temporal_quality COMMAND zenith_temporal_quality_tests)

    add_executable(zenith_spatial_upscale_tests tests/engine/SpatialUpscaleTests.cpp)
    target_link_libraries(zenith_spatial_upscale_tests PRIVATE zenith_engine)
    add_test(NAME zenith_spatial_upscale COMMAND zenith_spatial_upscale_tests)

    add_executable(zenith_tone_mapping_tests tests/engine/ToneMappingTests.cpp)
    target_link_libraries(zenith_tone_mapping_tests PRIVATE zenith_engine)
    add_test(NAME zenith_tone_mapping COMMAND zenith_tone_mapping_tests)

    add_executable(zenith_reflection_probe_tests tests/engine/ReflectionProbeTests.cpp)
    target_link_libraries(zenith_reflection_probe_tests PRIVATE zenith_engine)
    add_test(NAME zenith_reflection_probes COMMAND zenith_reflection_probe_tests)

    add_executable(zenith_decal_tests tests/engine/DecalTests.cpp)
    target_compile_definitions(zenith_decal_tests PRIVATE
        ZENITH_ASSET_DIR="${ZENITH_ASSET_BINARY_DIR}"
    )
    add_dependencies(zenith_decal_tests zenith_assets)
    target_link_libraries(zenith_decal_tests PRIVATE zenith_engine)
    add_test(NAME zenith_decals COMMAND zenith_decal_tests)

    add_executable(zenith_environment_lighting_tests tests/engine/EnvironmentLightingTests.cpp)
    target_link_libraries(zenith_environment_lighting_tests PRIVATE zenith_engine)
    add_test(NAME zenith_environment_lighting COMMAND zenith_environment_lighting_tests)

    add_executable(zenith_irradiance_volume_tests tests/engine/IrradianceVolumeTests.cpp)
    target_link_libraries(zenith_irradiance_volume_tests PRIVATE zenith_engine)
    if(MSVC)
        target_compile_options(zenith_irradiance_volume_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_irradiance_volume_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_irradiance_volume COMMAND zenith_irradiance_volume_tests)

    add_executable(zenith_asset_streaming_tests tests/engine/AssetStreamingTests.cpp)
    target_link_libraries(zenith_asset_streaming_tests PRIVATE zenith_engine)
    add_test(NAME zenith_asset_streaming COMMAND zenith_asset_streaming_tests)

    add_executable(zenith_render_regression_tests tests/engine/RenderRegressionTests.cpp)
    add_test(NAME zenith_render_regression COMMAND zenith_render_regression_tests)

    add_executable(zenith_frame_capture_tests tests/engine/FrameCaptureTests.cpp)
    target_link_libraries(zenith_frame_capture_tests PRIVATE zenith_engine)
    add_test(NAME zenith_frame_capture COMMAND zenith_frame_capture_tests)

    add_executable(zenith_capture_camera_tests tests/game/CaptureCameraTests.cpp)
    target_link_libraries(zenith_capture_camera_tests PRIVATE zenith_demo)
    add_test(NAME zenith_capture_camera COMMAND zenith_capture_camera_tests)

    # Renders each reference set's views and compares them with
    # tests/references/<set> (docs/RENDERER_VALIDATION.md). Skips without a
    # hardware Vulkan device; zenith_update_<set>_references regenerates one
    # set and zenith_update_references regenerates all of them.
    add_executable(zenith_reference_capture tests/capture/ReferenceCaptureDriver.cpp)
    # The driver loads Vulkan through its own volk instance; the engine's is private.
    target_link_libraries(zenith_reference_capture PRIVATE zenith_engine volk)
    # Captures sample the cooked BC7/BC5 textures, so they must be current.
    add_dependencies(zenith_reference_capture zenith zenith_cooked_textures zenith_debug_draw_smoke_tests
        zenith_scene_gizmo_smoke_tests)
    add_custom_target(zenith_update_references)
    # Each set renders with the game, except debug_draw and scene_gizmos,
    # whose fixed scenes are drawn by Engine-hosted smoke test executables.
    foreach(reference_set IN ITEMS showcase irradiance_room debug_draw scene_gizmos)
        set(capture_executable zenith)
        if(reference_set STREQUAL "debug_draw")
            set(capture_executable zenith_debug_draw_smoke_tests)
        elseif(reference_set STREQUAL "scene_gizmos")
            set(capture_executable zenith_scene_gizmo_smoke_tests)
        endif()
        set(capture_manifest "${CMAKE_CURRENT_SOURCE_DIR}/tests/references/${reference_set}/captures.json")
        set(capture_arguments
            --executable "$<TARGET_FILE:${capture_executable}>"
            --manifest "${capture_manifest}"
            --output "${CMAKE_CURRENT_BINARY_DIR}/captures/${reference_set}")
        # One CTest per manifest view, so views capture in parallel and a
        # failure names its view. Editing the manifest re-runs configure.
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${capture_manifest}")
        file(READ "${capture_manifest}" capture_manifest_json)
        string(JSON capture_view_count LENGTH "${capture_manifest_json}" views)
        math(EXPR capture_last_view "${capture_view_count} - 1")
        foreach(capture_view_index RANGE ${capture_last_view})
            string(JSON capture_view GET "${capture_manifest_json}" views ${capture_view_index} name)
            add_test(NAME zenith_${reference_set}_capture_${capture_view}
                COMMAND zenith_reference_capture ${capture_arguments} --view ${capture_view})
            set_tests_properties(zenith_${reference_set}_capture_${capture_view} PROPERTIES
                LABELS "vulkan;capture"
                SKIP_RETURN_CODE 77
                TIMEOUT 120
                PROCESSORS 2
            )
        endforeach()
        add_custom_target(zenith_update_${reference_set}_references
            COMMAND zenith_reference_capture ${capture_arguments} --update
            DEPENDS zenith_reference_capture ${capture_executable} zenith_cooked_textures
            COMMENT "Regenerating ${reference_set} reference captures"
            VERBATIM
        )
        add_dependencies(zenith_update_references zenith_update_${reference_set}_references)
    endforeach()

    add_executable(zenith_render_packet_benchmark tests/engine/RenderPacketBenchmarkTests.cpp)
    target_link_libraries(zenith_render_packet_benchmark PRIVATE zenith_engine)

    add_executable(zenith_simulation_benchmark tests/engine/SimulationBenchmark.cpp)
    target_link_libraries(zenith_simulation_benchmark PRIVATE zenith_engine)

    # The benchmarks are run by hand for numbers; CTest only checks that they
    # complete and exit cleanly so they cannot silently rot.
    add_test(NAME zenith_render_packet_benchmark_run COMMAND zenith_render_packet_benchmark)
    add_test(NAME zenith_simulation_benchmark_run COMMAND zenith_simulation_benchmark)
    set_tests_properties(zenith_render_packet_benchmark_run zenith_simulation_benchmark_run PROPERTIES
        LABELS "benchmark"
    )

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

    add_executable(zenith_viewport_chrome_tests tests/editor/ViewportChromeTests.cpp)
    target_link_libraries(zenith_viewport_chrome_tests PRIVATE zenith_editor_app)
    if(MSVC)
        target_compile_options(zenith_viewport_chrome_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_viewport_chrome_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_viewport_chrome COMMAND zenith_viewport_chrome_tests)

    # Viewport scene gizmos recorded on the CPU (editor/viewport/SceneGizmoDraw).
    add_executable(zenith_scene_gizmo_tests tests/editor/SceneGizmoDrawTests.cpp)
    target_link_libraries(zenith_scene_gizmo_tests PRIVATE zenith_editor_app)
    add_dependencies(zenith_scene_gizmo_tests zenith_assets)
    if(MSVC)
        target_compile_options(zenith_scene_gizmo_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_scene_gizmo_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_scene_gizmo COMMAND zenith_scene_gizmo_tests)

    # Scene gizmos in a rendered view under strict validation, plus the fixed
    # scene the scene_gizmos reference set captures
    # (tests/references/scene_gizmos).
    add_executable(zenith_scene_gizmo_smoke_tests tests/editor/SceneGizmoCaptureTests.cpp)
    target_link_libraries(zenith_scene_gizmo_smoke_tests PRIVATE zenith_editor_app)
    add_dependencies(zenith_scene_gizmo_smoke_tests zenith_assets)
    if(MSVC)
        target_compile_options(zenith_scene_gizmo_smoke_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_scene_gizmo_smoke_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_scene_gizmo_vulkan_smoke COMMAND zenith_scene_gizmo_smoke_tests)
    set_tests_properties(zenith_scene_gizmo_vulkan_smoke PROPERTIES
        ENVIRONMENT "ZENITH_SMOKE_FRAMES=60;ZENITH_VALIDATION_STRICT=1"
        LABELS "vulkan;smoke"
        SKIP_RETURN_CODE 77
        TIMEOUT 120
        PROCESSORS 2
    )

    add_executable(zenith_editor_theme_tests tests/editor/EditorThemeTests.cpp)
    target_link_libraries(zenith_editor_theme_tests PRIVATE zenith_editor_app)
    if(MSVC)
        target_compile_options(zenith_editor_theme_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_editor_theme_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_editor_theme COMMAND zenith_editor_theme_tests)

    add_executable(zenith_multi_viewport_tests tests/editor/MultiViewportTests.cpp)
    target_link_libraries(zenith_multi_viewport_tests PRIVATE zenith_editor_app)
    if(MSVC)
        target_compile_options(zenith_multi_viewport_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_multi_viewport_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_multi_viewport COMMAND zenith_multi_viewport_tests)

    add_executable(zenith_viewport_picking_tests tests/editor/ViewportPickingTests.cpp)
    target_compile_definitions(zenith_viewport_picking_tests PRIVATE ZENITH_ASSET_DIR="${ZENITH_ASSET_BINARY_DIR}")
    add_dependencies(zenith_viewport_picking_tests zenith_assets)
    target_link_libraries(zenith_viewport_picking_tests PRIVATE zenith_editor_app)
    if(MSVC)
        target_compile_options(zenith_viewport_picking_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_viewport_picking_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_viewport_picking COMMAND zenith_viewport_picking_tests)

    add_executable(zenith_snapping_cursor_tests tests/editor/SnappingCursorTests.cpp)
    target_compile_definitions(zenith_snapping_cursor_tests PRIVATE ZENITH_ASSET_DIR="${ZENITH_ASSET_BINARY_DIR}")
    add_dependencies(zenith_snapping_cursor_tests zenith_assets)
    target_link_libraries(zenith_snapping_cursor_tests PRIVATE zenith_editor_app)
    if(MSVC)
        target_compile_options(zenith_snapping_cursor_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_snapping_cursor_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_snapping_cursor COMMAND zenith_snapping_cursor_tests)
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

    add_executable(zenith_irradiance_volume_baker_tests
        tests/editor/IrradianceVolumeBakerTests.cpp
        src/editor/project/IrradianceVolumeBaker.cpp
    )
    target_include_directories(zenith_irradiance_volume_baker_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(zenith_irradiance_volume_baker_tests PRIVATE zenith_engine)
    if(MSVC)
        target_compile_options(zenith_irradiance_volume_baker_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_irradiance_volume_baker_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_irradiance_volume_baker COMMAND zenith_irradiance_volume_baker_tests)

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

    add_executable(zenith_pick_queries_tests tests/editor/PickQueriesTests.cpp)
    target_link_libraries(zenith_pick_queries_tests PRIVATE zenith_editor_app)
    if(MSVC)
        target_compile_options(zenith_pick_queries_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_pick_queries_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_pick_queries COMMAND zenith_pick_queries_tests)

    add_executable(zenith_selection_tests tests/editor/SelectionTests.cpp)
    target_link_libraries(zenith_selection_tests PRIVATE zenith_editor_app)
    if(MSVC)
        target_compile_options(zenith_selection_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_selection_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_selection COMMAND zenith_selection_tests)

    add_executable(zenith_outliner_tests tests/editor/OutlinerTests.cpp)
    target_link_libraries(zenith_outliner_tests PRIVATE zenith_editor_app)
    if(MSVC)
        target_compile_options(zenith_outliner_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_outliner_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_outliner COMMAND zenith_outliner_tests)

    add_executable(zenith_scene_editing_tests tests/editor/SceneEditingTests.cpp)
    target_link_libraries(zenith_scene_editing_tests PRIVATE zenith_editor_app)
    if(MSVC)
        target_compile_options(zenith_scene_editing_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_scene_editing_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_scene_editing COMMAND zenith_scene_editing_tests)

    add_executable(zenith_reflected_field_editor_tests tests/editor/ReflectedFieldEditorTests.cpp)
    target_link_libraries(zenith_reflected_field_editor_tests PRIVATE zenith_editor_app)
    if(MSVC)
        target_compile_options(zenith_reflected_field_editor_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_reflected_field_editor_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_reflected_field_editor COMMAND zenith_reflected_field_editor_tests)

    add_executable(zenith_properties_panel_tests tests/editor/PropertiesPanelTests.cpp)
    target_link_libraries(zenith_properties_panel_tests PRIVATE zenith_editor_app)
    if(MSVC)
        target_compile_options(zenith_properties_panel_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_properties_panel_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_properties_panel COMMAND zenith_properties_panel_tests)

    add_executable(zenith_entity_reference_field_tests tests/editor/EntityReferenceFieldTests.cpp)
    target_link_libraries(zenith_entity_reference_field_tests PRIVATE zenith_editor_app)
    if(MSVC)
        target_compile_options(zenith_entity_reference_field_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_entity_reference_field_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_entity_reference_field COMMAND zenith_entity_reference_field_tests)


    add_executable(zenith_component_operator_tests tests/editor/ComponentOperatorTests.cpp)
    target_link_libraries(zenith_component_operator_tests PRIVATE zenith_editor_app)
    if(MSVC)
        target_compile_options(zenith_component_operator_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_component_operator_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_component_operators COMMAND zenith_component_operator_tests)

    add_executable(zenith_logger_tests tests/engine/LoggerTests.cpp)
    target_link_libraries(zenith_logger_tests PRIVATE zenith_engine)
    if(MSVC)
        target_compile_options(zenith_logger_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_logger_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_logger COMMAND zenith_logger_tests)

    add_executable(zenith_child_process_tests tests/engine/ChildProcessTests.cpp)
    target_link_libraries(zenith_child_process_tests PRIVATE zenith_engine)
    target_compile_definitions(zenith_child_process_tests PRIVATE ZENITH_TEST_CMAKE="${CMAKE_COMMAND}")
    if(MSVC)
        target_compile_options(zenith_child_process_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_child_process_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_child_process COMMAND zenith_child_process_tests)

    add_executable(zenith_build_workflow_tests tests/editor/BuildWorkflowTests.cpp)
    target_link_libraries(zenith_build_workflow_tests PRIVATE zenith_editor_app)
    target_compile_definitions(zenith_build_workflow_tests PRIVATE ZENITH_TEST_CMAKE="${CMAKE_COMMAND}")
    if(MSVC)
        target_compile_options(zenith_build_workflow_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_build_workflow_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_build_workflow COMMAND zenith_build_workflow_tests)

    # The Project Hub, recent projects, and the C++ game template. The second
    # test also configures and builds a generated project's game module
    # against this build with CMake, which takes a while.
    add_executable(zenith_project_hub_tests tests/editor/ProjectHubTests.cpp)
    target_link_libraries(zenith_project_hub_tests PRIVATE zenith_editor_app)
    target_compile_definitions(zenith_project_hub_tests PRIVATE
        ZENITH_ASSET_DIR="${ZENITH_ASSET_BINARY_DIR}"
        ZENITH_TEMPLATE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/templates"
        ZENITH_ENGINE_BUILD_DIR="${CMAKE_CURRENT_BINARY_DIR}"
        ZENITH_EDITOR_CONFIGURATION="$<CONFIG>"
        ZENITH_TEST_CMAKE="${CMAKE_COMMAND}")
    add_dependencies(zenith_project_hub_tests zenith_assets)
    if(MSVC)
        target_compile_options(zenith_project_hub_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_project_hub_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_project_hub COMMAND zenith_project_hub_tests)
    add_test(NAME zenith_project_template_module_build COMMAND zenith_project_hub_tests --build-generated-module)
    set_tests_properties(zenith_project_template_module_build PROPERTIES LABELS "slow" TIMEOUT 600)


    add_executable(zenith_package_workflow_tests tests/editor/PackageWorkflowTests.cpp)
    target_link_libraries(zenith_package_workflow_tests PRIVATE zenith_editor_app)
    if(MSVC)
        target_compile_options(zenith_package_workflow_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_package_workflow_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_package_workflow COMMAND zenith_package_workflow_tests)

    add_executable(zenith_preferences_tests tests/editor/PreferencesTests.cpp)
    target_link_libraries(zenith_preferences_tests PRIVATE zenith_editor_app)
    target_compile_definitions(zenith_preferences_tests PRIVATE ZENITH_TEST_CMAKE="${CMAKE_COMMAND}")
    if(MSVC)
        target_compile_options(zenith_preferences_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_preferences_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_preferences COMMAND zenith_preferences_tests)

    add_executable(zenith_asset_browser_tests tests/editor/AssetBrowserTests.cpp)
    target_link_libraries(zenith_asset_browser_tests PRIVATE zenith_editor_app)
    if(MSVC)
        target_compile_options(zenith_asset_browser_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_asset_browser_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_asset_browser COMMAND zenith_asset_browser_tests)

    add_executable(zenith_asset_reference_workflow_tests tests/editor/AssetReferenceWorkflowTests.cpp)
    target_compile_definitions(zenith_asset_reference_workflow_tests PRIVATE
        ZENITH_ASSET_DIR="${ZENITH_ASSET_BINARY_DIR}"
        ZENITH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_dependencies(zenith_asset_reference_workflow_tests zenith_assets)
    target_link_libraries(zenith_asset_reference_workflow_tests PRIVATE zenith_editor_app)
    if(MSVC)
        target_compile_options(zenith_asset_reference_workflow_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_asset_reference_workflow_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_asset_reference_workflow COMMAND zenith_asset_reference_workflow_tests)

    add_executable(zenith_operator_tests tests/editor/OperatorTests.cpp)
    target_link_libraries(zenith_operator_tests PRIVATE zenith_editor_app)
    if(MSVC)
        target_compile_options(zenith_operator_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_operator_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_operators COMMAND zenith_operator_tests)

    add_executable(zenith_menu_tests tests/editor/MenuTests.cpp)
    target_link_libraries(zenith_menu_tests PRIVATE zenith_editor_app)
    if(MSVC)
        target_compile_options(zenith_menu_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_menu_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_menus COMMAND zenith_menu_tests)


    add_executable(zenith_prefab_tests tests/editor/PrefabTests.cpp)
    target_compile_definitions(zenith_prefab_tests PRIVATE ZENITH_ASSET_DIR="${ZENITH_ASSET_BINARY_DIR}")
    add_dependencies(zenith_prefab_tests zenith_assets)
    target_link_libraries(zenith_prefab_tests PRIVATE zenith_editor_app)
    if(MSVC)
        target_compile_options(zenith_prefab_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_prefab_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_prefabs COMMAND zenith_prefab_tests)

    add_executable(zenith_prefab_ui_tests tests/editor/PrefabUiTests.cpp)
    target_link_libraries(zenith_prefab_ui_tests PRIVATE zenith_editor_app)
    if(MSVC)
        target_compile_options(zenith_prefab_ui_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_prefab_ui_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_prefab_ui COMMAND zenith_prefab_ui_tests)

    add_executable(zenith_workspace_tests tests/editor/WorkspaceTests.cpp)
    target_link_libraries(zenith_workspace_tests PRIVATE zenith_editor_app)
    if(MSVC)
        target_compile_options(zenith_workspace_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_workspace_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_workspaces COMMAND zenith_workspace_tests)

    # The full editor session driven headlessly, as zenith_editor composes it.
    add_executable(zenith_editor_session_tests tests/editor/EditorSessionTests.cpp)
    target_link_libraries(zenith_editor_session_tests PRIVATE zenith_editor_app)
    target_compile_definitions(zenith_editor_session_tests PRIVATE
        ZENITH_ASSET_DIR="${ZENITH_ASSET_BINARY_DIR}"
        ZENITH_TEMPLATE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/templates"
        ZENITH_EDITOR_FONT="${inter_SOURCE_DIR}/extras/ttf/Inter-Regular.ttf")
    add_dependencies(zenith_editor_session_tests zenith_assets)
    if(MSVC)
        target_compile_options(zenith_editor_session_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_editor_session_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_editor_session COMMAND zenith_editor_session_tests)

    add_executable(zenith_editor_area_types_tests tests/editor/EditorAreaTypesTests.cpp)
    target_link_libraries(zenith_editor_area_types_tests PRIVATE zenith_editor_app)
    if(MSVC)
        target_compile_options(zenith_editor_area_types_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_editor_area_types_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_editor_area_types COMMAND zenith_editor_area_types_tests)

    add_executable(zenith_info_panel_tests tests/editor/InfoPanelTests.cpp)
    target_link_libraries(zenith_info_panel_tests PRIVATE zenith_editor_app)
    if(MSVC)
        target_compile_options(zenith_info_panel_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_info_panel_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_info_panel COMMAND zenith_info_panel_tests)

    add_executable(zenith_system_profiler_tests tests/editor/SystemProfilerTests.cpp)
    target_link_libraries(zenith_system_profiler_tests PRIVATE zenith_editor_app)
    target_compile_definitions(zenith_system_profiler_tests PRIVATE
        ZENITH_EDITOR_TEST_MODULE_V1="$<TARGET_FILE:zenith_editor_test_module>")
    add_dependencies(zenith_system_profiler_tests zenith_editor_test_module)
    if(MSVC)
        target_compile_options(zenith_system_profiler_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(zenith_system_profiler_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME zenith_system_profiler COMMAND zenith_system_profiler_tests)

    # Game modules with editor extensions for zenith_game_module_host_tests and
    # the editor module smoke: one source built as two versions, so a test can
    # hot reload one over the other. They link only the engine, as real game
    # modules do.
    add_library(zenith_editor_test_module MODULE tests/editor/modules/EditorTestModule.cpp)
    add_library(zenith_editor_test_module_v2 MODULE tests/editor/modules/EditorTestModule.cpp)
    target_compile_definitions(zenith_editor_test_module_v2 PRIVATE ZENITH_EDITOR_TEST_MODULE_V2)
    add_executable(zenith_game_module_host_tests tests/editor/GameModuleHostTests.cpp)
    target_link_libraries(zenith_game_module_host_tests PRIVATE zenith_editor_app)
    target_compile_definitions(zenith_game_module_host_tests PRIVATE
        ZENITH_EDITOR_TEST_MODULE_V1="$<TARGET_FILE:zenith_editor_test_module>"
        ZENITH_EDITOR_TEST_MODULE_V2="$<TARGET_FILE:zenith_editor_test_module_v2>")
    add_dependencies(zenith_game_module_host_tests zenith_editor_test_module zenith_editor_test_module_v2)
    foreach(editor_module_target IN ITEMS zenith_editor_test_module zenith_editor_test_module_v2
            zenith_game_module_host_tests)
        if(NOT editor_module_target STREQUAL "zenith_game_module_host_tests")
            target_link_libraries(${editor_module_target} PRIVATE zenith_engine)
            target_include_directories(${editor_module_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        endif()
        if(MSVC)
            target_compile_options(${editor_module_target} PRIVATE /W4 /permissive-)
        else()
            target_compile_options(${editor_module_target} PRIVATE -Wall -Wextra -Wpedantic)
        endif()
    endforeach()
    add_test(NAME zenith_game_module_host COMMAND zenith_game_module_host_tests)

    # Editor runs keep their settings (workspaces.json, the smoke scene, ...)
    # in their own directory under the build tree, never the user's, starting
    # from an empty one each ctest run so they open the default workspaces.
    set(ZENITH_EDITOR_TEST_SETTINGS_DIR "${CMAKE_CURRENT_BINARY_DIR}/editor-test-settings")
    add_test(NAME zenith_editor_test_settings_reset
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${ZENITH_EDITOR_TEST_SETTINGS_DIR}")
    set_tests_properties(zenith_editor_test_settings_reset PROPERTIES FIXTURES_SETUP zenith_editor_test_settings)

    add_test(NAME zenith_editor_vulkan_smoke COMMAND zenith_editor)
    set_tests_properties(zenith_editor_vulkan_smoke PROPERTIES
        ENVIRONMENT
            "ZENITH_EDITOR_SMOKE=1;ZENITH_SMOKE_FRAMES=900;ZENITH_EDITOR_SETTINGS_DIR=${ZENITH_EDITOR_TEST_SETTINGS_DIR}/vulkan_smoke"
        LABELS "vulkan;smoke"
        TIMEOUT 45
        FIXTURES_REQUIRED zenith_editor_test_settings
    )

    # The editor smoke on the demo scene with a game module, which the smoke
    # scenario also hot reloads once.
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/projects/demo.zenith-project" ZENITH_DEMO_PROJECT_JSON)
    string(JSON ZENITH_DEMO_RENDER_SETTINGS GET "${ZENITH_DEMO_PROJECT_JSON}" renderSettings)
    # Project paths are relative to the project file.
    file(RELATIVE_PATH ZENITH_MODULE_SMOKE_ASSET_ROOT "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/assets")
    file(RELATIVE_PATH ZENITH_MODULE_SMOKE_SCENE_ROOT "${CMAKE_CURRENT_BINARY_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/projects/scenes")
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/editor-module-smoke-$<CONFIG>.zenith-project" CONTENT
"{
  \"version\": 4,
  \"displayName\": \"Zenith Editor Module Smoke\",
  \"assetRoot\": \"${ZENITH_MODULE_SMOKE_ASSET_ROOT}\",
  \"sceneRoot\": \"${ZENITH_MODULE_SMOKE_SCENE_ROOT}\",
  \"startupScene\": \"demo.zenith-scene\",
  \"gameModule\": \"$<PATH:RELATIVE_PATH,$<TARGET_FILE:zenith_editor_test_module>,${CMAKE_CURRENT_BINARY_DIR}>\",
  \"renderSettings\": ${ZENITH_DEMO_RENDER_SETTINGS}
}
")
    add_test(NAME zenith_editor_module_smoke
        COMMAND zenith_editor "${CMAKE_CURRENT_BINARY_DIR}/editor-module-smoke-$<CONFIG>.zenith-project")
    set_tests_properties(zenith_editor_module_smoke PROPERTIES
        ENVIRONMENT
            "ZENITH_EDITOR_SMOKE=1;ZENITH_SMOKE_FRAMES=900;ZENITH_EDITOR_SETTINGS_DIR=${ZENITH_EDITOR_TEST_SETTINGS_DIR}/module_smoke"
        LABELS "vulkan;smoke"
        TIMEOUT 60
        FIXTURES_REQUIRED zenith_editor_test_settings
    )

    add_executable(zenith_graphics_validation_tests tests/engine/GraphicsValidationTests.cpp)
    target_link_libraries(zenith_graphics_validation_tests PRIVATE zenith_engine)
    add_test(NAME zenith_graphics_validation COMMAND zenith_graphics_validation_tests)

    # Strict validation smoke matrix (docs/RENDERER_VALIDATION.md). Any counted
    # validation or synchronization message fails the run; exit code 77 marks it
    # skipped when VK_LAYER_KHRONOS_validation is not installed.
    function(zenith_add_validation_smoke test_name)
        cmake_parse_arguments(SMOKE "" "" "COMMAND;ENVIRONMENT" ${ARGN})
        add_test(NAME ${test_name} COMMAND ${SMOKE_COMMAND})
        set_tests_properties(${test_name} PROPERTIES
            ENVIRONMENT "ZENITH_VALIDATION_STRICT=1;${SMOKE_ENVIRONMENT}"
            LABELS "vulkan;validation"
            SKIP_RETURN_CODE 77
            TIMEOUT 300
            # Counts double against ctest -j so at most half the slots run an
            # engine instance on the shared GPU.
            PROCESSORS 2
        )
    endfunction()

    set(ZENITH_VALIDATION_SCENES demo showcase)
    set(ZENITH_VALIDATION_COMMAND_demo zenith)
    set(ZENITH_VALIDATION_COMMAND_showcase zenith --project "${CMAKE_CURRENT_SOURCE_DIR}/projects/showcase.zenith-project")
    set(ZENITH_VALIDATION_SUBMISSIONS cpu gpu_driven)
    set(ZENITH_VALIDATION_ENVIRONMENT_cpu "")
    set(ZENITH_VALIDATION_ENVIRONMENT_gpu_driven "ZENITH_FORCE_GPU_DRIVEN=1")
    set(ZENITH_VALIDATION_QUALITIES default high_dynamic fsr1_dynamic device_fallbacks)
    set(ZENITH_VALIDATION_ENVIRONMENT_default "")
    set(ZENITH_VALIDATION_ENVIRONMENT_high_dynamic "ZENITH_FORCE_HIGH_QUALITY=1;ZENITH_FORCE_DYNAMIC_RESOLUTION=1")
    # FSR 1 after a render-resolution TAA resolve, under dynamic resolution.
    set(ZENITH_VALIDATION_ENVIRONMENT_fsr1_dynamic
        "ZENITH_FORCE_HIGH_QUALITY=1;ZENITH_FORCE_DYNAMIC_RESOLUTION=1;ZENITH_FORCE_UPSCALER=fsr1;ZENITH_FORCE_UPSCALER_QUALITY=performance")
    # Paths other vendors' devices take: RGBA8 instead of BC textures,
    # update-after-bind bindless descriptors, the per-level depth pyramid, and
    # FSR 1 requested without support (it falls back to TAA upsampling).
    set(ZENITH_VALIDATION_ENVIRONMENT_device_fallbacks
        "ZENITH_FORCE_HIGH_QUALITY=1;ZENITH_DISABLE_BLOCK_COMPRESSION=1;ZENITH_FORCE_UPDATE_AFTER_BIND=1;ZENITH_DEPTH_PYRAMID_PER_LEVEL=1;ZENITH_DISABLE_SPATIAL_UPSCALING=1;ZENITH_FORCE_UPSCALER=fsr1")
    foreach(scene IN LISTS ZENITH_VALIDATION_SCENES)
        foreach(submission IN LISTS ZENITH_VALIDATION_SUBMISSIONS)
            foreach(quality IN LISTS ZENITH_VALIDATION_QUALITIES)
                zenith_add_validation_smoke(zenith_validation_${scene}_${submission}_${quality}
                    COMMAND ${ZENITH_VALIDATION_COMMAND_${scene}}
                    ENVIRONMENT ZENITH_SMOKE_FRAMES=120
                        ${ZENITH_VALIDATION_ENVIRONMENT_${submission}}
                        ${ZENITH_VALIDATION_ENVIRONMENT_${quality}}
                )
            endforeach()
        endforeach()
    endforeach()
    # View shading modes (ZENITH_FORCE_VIEW_SHADING) on both submission paths,
    # with every feature requested so each mode strips the most, plus
    # Wireframe on a device without fillModeNonSolid (the Solid fallback).
    set(ZENITH_VALIDATION_VIEW_SHADINGS material_preview solid wireframe)
    foreach(scene IN LISTS ZENITH_VALIDATION_SCENES)
        foreach(submission IN LISTS ZENITH_VALIDATION_SUBMISSIONS)
            foreach(shading IN LISTS ZENITH_VALIDATION_VIEW_SHADINGS)
                zenith_add_validation_smoke(zenith_validation_${scene}_${submission}_shading_${shading}
                    COMMAND ${ZENITH_VALIDATION_COMMAND_${scene}}
                    ENVIRONMENT ZENITH_SMOKE_FRAMES=120 ZENITH_FORCE_HIGH_QUALITY=1
                        ZENITH_FORCE_VIEW_SHADING=${shading}
                        ${ZENITH_VALIDATION_ENVIRONMENT_${submission}}
                )
            endforeach()
        endforeach()
    endforeach()
    zenith_add_validation_smoke(zenith_validation_demo_cpu_shading_wireframe_fallback
        COMMAND zenith
        ENVIRONMENT ZENITH_SMOKE_FRAMES=120 ZENITH_FORCE_VIEW_SHADING=wireframe ZENITH_DISABLE_WIREFRAME=1
    )
    zenith_add_validation_smoke(zenith_validation_editor
        COMMAND zenith_editor
        ENVIRONMENT ZENITH_EDITOR_SMOKE=1 ZENITH_SMOKE_FRAMES=900
            ZENITH_EDITOR_SETTINGS_DIR=${ZENITH_EDITOR_TEST_SETTINGS_DIR}/validation_editor
    )
    set_tests_properties(zenith_validation_editor PROPERTIES FIXTURES_REQUIRED zenith_editor_test_settings)
    zenith_add_validation_smoke(zenith_validation_multi_view
        COMMAND zenith_multi_view_smoke_tests
        ENVIRONMENT ZENITH_SMOKE_FRAMES=120
    )
    # The default view picks through both submission paths' visible lists.
    zenith_add_validation_smoke(zenith_validation_picking
        COMMAND zenith_picking_smoke_tests
        ENVIRONMENT ZENITH_SMOKE_FRAMES=80
    )
    zenith_add_validation_smoke(zenith_validation_picking_gpu_driven
        COMMAND zenith_picking_smoke_tests
        ENVIRONMENT ZENITH_SMOKE_FRAMES=80 ZENITH_FORCE_GPU_DRIVEN=1
    )
    set_tests_properties(zenith_multi_view_vulkan_smoke zenith_editor_vulkan_smoke zenith_editor_module_smoke
        PROPERTIES PROCESSORS 2)

    # Test targets declared without the project's test warning level.
    foreach(test_target IN ITEMS
            zenith_game_module_tests zenith_render_submission_tests zenith_gpu_resource_table_tests
            zenith_mesh_lod_tests zenith_mesh_pass_frame_plan_tests zenith_view_shading_tests
            zenith_render_view_tests zenith_multi_view_smoke_tests zenith_deferred_upload_tracker_tests
            zenith_gpu_cull_layout_tests zenith_packed_vertex_stream_tests zenith_depth_prepass_tests
            zenith_screen_space_reflection_tests zenith_ambient_occlusion_tests zenith_clustered_lighting_tests
            zenith_local_shadow_tests zenith_volumetric_fog_tests zenith_motion_blur_tests
            zenith_depth_of_field_tests zenith_color_grading_tests zenith_particle_tests
            zenith_frame_task_graph_tests zenith_render_graph_tests zenith_temporal_quality_tests
            zenith_spatial_upscale_tests zenith_tone_mapping_tests zenith_reflection_probe_tests
            zenith_decal_tests zenith_environment_lighting_tests zenith_asset_streaming_tests
            zenith_render_regression_tests zenith_frame_capture_tests zenith_capture_camera_tests
            zenith_reference_capture zenith_render_packet_benchmark zenith_simulation_benchmark
            zenith_benchmark_tests zenith_game_module_host_tests zenith_graphics_validation_tests)
        if(MSVC)
            target_compile_options(${test_target} PRIVATE /W4 /permissive-)
        else()
            target_compile_options(${test_target} PRIVATE -Wall -Wextra -Wpedantic)
        endif()
    endforeach()
endif()
