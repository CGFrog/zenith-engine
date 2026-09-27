cmake_minimum_required(VERSION 3.24)

project(zenith VERSION 0.1.0 LANGUAGES C CXX)

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
    cgltf
    stb
    EnTT
    nlohmann_json
    JoltPhysics
    VulkanMemoryAllocator
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

add_library(zenith_engine STATIC
    src/engine/audio/AudioSystem.cpp
    src/engine/audio/StbVorbis.cpp
    src/engine/Engine.cpp
    src/engine/application/Application.cpp
    src/engine/assets/Assets.cpp
    src/engine/assets/ModelImporter.cpp
    src/engine/assets/ImageDecoder.cpp
    src/engine/graphics/Renderer.cpp
    src/engine/graphics/RenderSubmission.cpp
    src/engine/graphics/passes/MeshPass.cpp
    src/engine/graphics/passes/PostProcessPass.cpp
    src/engine/graphics/passes/UiPass.cpp
    src/engine/graphics/pipeline/GraphicsPipeline.cpp
    src/engine/graphics/resources/FrameResources.cpp
    src/engine/graphics/resources/UploadContext.cpp
    src/engine/graphics/resources/ViewportTarget.cpp
    src/engine/graphics/shader/ShaderModule.cpp
    src/engine/graphics/surface/Surface.cpp
    src/engine/graphics/swapchain/Swapchain.cpp
    src/engine/graphics/vulkan/VulkanContext.cpp
    src/engine/input/InputManager.cpp
    src/engine/physics/PhysicsWorld.cpp
    src/engine/scene/Scene.cpp
    src/engine/scene/SceneDocument.cpp
    src/engine/window/Window.cpp
)

if(MSVC)
    set_source_files_properties(src/engine/audio/StbVorbis.cpp PROPERTIES COMPILE_OPTIONS /W0)
endif()

add_library(zenith_demo STATIC
    src/game/application/DemoApplication.cpp
    src/game/player/FirstPersonController.cpp
    src/game/scene/DemoScene.cpp
    src/game/scene/GameplayScene.cpp
)
target_link_libraries(zenith_demo PUBLIC zenith_engine)

add_executable(zenith src/main.cpp)
target_link_libraries(zenith PRIVATE zenith_demo)

add_library(zenith_editor_app STATIC
    src/editor/application/EditorApplication.cpp
    src/editor/application/EditorDiagnostics.cpp
    src/editor/application/EditorPlayMode.cpp
    src/editor/application/EditorShortcuts.cpp
    src/editor/application/EditorSession.cpp
    src/editor/document/CommandHistory.cpp
    src/editor/document/EditorDocument.cpp
    src/editor/document/EditorRecovery.cpp
    src/editor/document/SceneFragment.cpp
    src/editor/interaction/ComponentEditors.cpp
    src/editor/interaction/EditorCamera.cpp
    src/editor/project/AssetCatalog.cpp
    src/editor/project/ProjectConfig.cpp
    src/editor/project/ThumbnailCache.cpp
    src/editor/settings/EditorSettings.cpp
)
target_link_libraries(zenith_editor_app PUBLIC zenith_engine)
target_link_libraries(zenith_editor_app PUBLIC imguizmo::imguizmo)
target_compile_definitions(zenith_editor_app PRIVATE GLM_ENABLE_EXPERIMENTAL)

add_executable(zenith_editor src/editor/main.cpp)
target_link_libraries(zenith_editor PRIVATE zenith_editor_app zenith_demo)

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

target_include_directories(zenith_engine PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_definitions(zenith_engine PUBLIC VK_NO_PROTOTYPES
    ZENITH_SHADER_DIR="${ZENITH_SHADER_BINARY_DIR}"
    ZENITH_ASSET_DIR="${ZENITH_ASSET_BINARY_DIR}")

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

if(MSVC)
    target_compile_options(zenith_engine PRIVATE /W4 /permissive-)
    target_compile_options(zenith_demo PRIVATE /W4 /permissive-)
    target_compile_options(zenith_editor_app PRIVATE /W4 /permissive-)
else()
    target_compile_options(zenith_engine PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(zenith_demo PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(zenith_editor_app PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TESTING)
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

    add_executable(zenith_render_regression_tests tests/engine/RenderRegressionTests.cpp)
    add_test(NAME zenith_render_regression COMMAND zenith_render_regression_tests)

    add_executable(zenith_render_packet_benchmark tests/engine/RenderPacketBenchmarkTests.cpp)
    target_link_libraries(zenith_render_packet_benchmark PRIVATE zenith_engine)
    add_test(NAME zenith_render_packet_benchmark COMMAND zenith_render_packet_benchmark)

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

    add_executable(zenith_project_asset_tests
        tests/editor/ProjectAssetTests.cpp
        src/editor/project/AssetCatalog.cpp
        src/editor/project/ProjectConfig.cpp
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

    add_test(NAME zenith_editor_vulkan_smoke COMMAND zenith_editor)
    set_tests_properties(zenith_editor_vulkan_smoke PROPERTIES
        ENVIRONMENT "ZENITH_EDITOR_SMOKE=1;ZENITH_SMOKE_FRAMES=12"
        LABELS "vulkan;smoke"
        TIMEOUT 45
    )
endif()
