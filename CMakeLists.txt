cmake_minimum_required(VERSION 3.25)

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
# The dependency revision recorded in packages: the pinned third-party set.
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Dependencies.cmake" ZENITH_DEPENDENCY_REVISION)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

include(cmake/CompilerSettings.cmake)
include(cmake/WindowsExports.cmake)
include(cmake/Dependencies.cmake)

# The project's static and import libraries also share one directory
# (third-party ones stay in their _deps build directories).
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

# Engine, demo game, editor, and the executables that compose them; each
# folder lists its own sources.
add_subdirectory(src)

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

# The project a release package ships (its project file, scenes, game module,
# and assets). Build and Package (file.build_package) configures its release
# tree with the editor's open project.
set(ZENITH_PACKAGE_PROJECT "${CMAKE_CURRENT_SOURCE_DIR}/projects/demo.zenith-project" CACHE FILEPATH
    "The .zenith-project a release package ships")
# Files under assets/ that the runtime itself needs, whatever the project:
# none today (the game's UI font is built in and its shaders ship in
# shaders/), so a package's assets/ holds only the project's assets. A
# project asset at one of these paths fails the configure unless the path is
# in ZENITH_PACKAGE_ASSET_OVERRIDES.
set(ZENITH_ENGINE_RUNTIME_ASSETS "" CACHE STRING
    "Paths under assets/ every package ships for the runtime itself (a semicolon-separated list)")
set(ZENITH_PACKAGE_ASSET_OVERRIDES "" CACHE STRING
    "Engine runtime asset paths the packaged project may replace with its own files")
include(cmake/PackageProject.cmake)
zenith_read_package_project("${ZENITH_PACKAGE_PROJECT}")
cmake_path(COMPARE "${ZENITH_PACKAGE_ASSET_DIR}" NOT_EQUAL "${ZENITH_ASSET_SOURCE_DIR}" ZENITH_PACKAGE_HAS_OWN_ASSETS)
if(ZENITH_PACKAGE_HAS_OWN_ASSETS)
    zenith_check_package_asset_collisions("${ZENITH_ASSET_SOURCE_DIR}" "${ZENITH_ENGINE_RUNTIME_ASSETS}"
        "${ZENITH_PACKAGE_ASSET_DIR}" "${ZENITH_PACKAGE_ASSET_OVERRIDES}")
endif()

# Cook every shipped model's external textures (the engine's assets and the
# packaged project's) to block-compressed KTX2 in the derived-data cache the
# executables read from <resource root>/derived. Entries are keyed by source
# bytes, so unchanged textures are skipped on rebuild.
set(ZENITH_DERIVED_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/derived")
set(ZENITH_COOKED_ASSET_DIRS "${ZENITH_ASSET_SOURCE_DIR}")
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

# Cook the same models' optimized meshes and LOD chains (CookedMesh.hpp) into
# that cache, so imports skip meshoptimizer. Entries are keyed by the model and
# buffer bytes; a model the build did not cook is cooked on first load.
set(ZENITH_COOKED_MODEL_BUFFERS)
foreach(cooked_asset_dir IN LISTS ZENITH_COOKED_ASSET_DIRS)
    file(GLOB_RECURSE cooked_buffers CONFIGURE_DEPENDS "${cooked_asset_dir}/*.bin")
    list(APPEND ZENITH_COOKED_MODEL_BUFFERS ${cooked_buffers})
endforeach()
set(ZENITH_MESH_COOK_COMMANDS)
foreach(cooked_model IN LISTS ZENITH_COOKED_MODELS)
    list(APPEND ZENITH_MESH_COOK_COMMANDS
        COMMAND $<TARGET_FILE:zenith_asset_build> cook-model-meshes "${cooked_model}" "${ZENITH_DERIVED_BINARY_DIR}")
endforeach()
add_custom_command(
    OUTPUT "${ZENITH_DERIVED_BINARY_DIR}/cooked-meshes.stamp"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${ZENITH_DERIVED_BINARY_DIR}"
    ${ZENITH_MESH_COOK_COMMANDS}
    COMMAND "${CMAKE_COMMAND}" -E touch "${ZENITH_DERIVED_BINARY_DIR}/cooked-meshes.stamp"
    DEPENDS zenith_asset_build ${ZENITH_COOKED_MODELS} ${ZENITH_COOKED_MODEL_BUFFERS}
    COMMENT "Cooking optimized meshes"
    VERBATIM
)
add_custom_target(zenith_cooked_meshes ALL DEPENDS "${ZENITH_DERIVED_BINARY_DIR}/cooked-meshes.stamp")

target_include_directories(zenith_engine PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

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
target_link_libraries(zenith_engine PRIVATE volk imgui zenith_imguizmo Jolt shaderc zenith_bc7enc meshoptimizer
    zenith_recast ${CMAKE_DL_LIBS})
foreach(header_dependency IN ITEMS volk imgui zenith_imguizmo)
    target_include_directories(zenith_engine PUBLIC
        $<TARGET_PROPERTY:${header_dependency},INTERFACE_INCLUDE_DIRECTORIES>)
    target_compile_definitions(zenith_engine PUBLIC
        $<TARGET_PROPERTY:${header_dependency},INTERFACE_COMPILE_DEFINITIONS>)
endforeach()

include(cmake/GameplayApi.cmake)
zenith_add_gameplay_api_check()

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
if(APPLE)
    # The application bundle carries the engine and SDL libraries in its
    # Frameworks folder, where the bundle's executable, the asset tool beside
    # the bundle, and the engine itself look for them. They are the only
    # non-system libraries the executables link, so no dependency scan runs.
    set(ZENITH_INSTALL_LIBRARY_DIR "Zenith.app/Contents/Frameworks")
    set_target_properties(zenith PROPERTIES INSTALL_RPATH "@executable_path/../Frameworks")
    set_target_properties(zenith_asset_build PROPERTIES INSTALL_RPATH "@executable_path/${ZENITH_INSTALL_LIBRARY_DIR}")
    set_target_properties(zenith_engine PROPERTIES INSTALL_RPATH "@loader_path")
    install(TARGETS zenith zenith_asset_build
        RUNTIME DESTINATION .
        BUNDLE DESTINATION .
    )
else()
    set(ZENITH_INSTALL_LIBRARY_DIR ".")
    install(TARGETS zenith zenith_asset_build
        RUNTIME_DEPENDENCIES
            PRE_EXCLUDE_REGEXES "api-ms-" "ext-ms-"
            POST_EXCLUDE_REGEXES ".*[Ss]ystem32/.*" ".*/lib(c|m|pthread|dl|rt)\\.so.*"
        RUNTIME DESTINATION .
    )
endif()
# The engine and SDL shared libraries sit beside the executables (in the
# bundle on macOS). Their import libraries and development symlinks are not
# part of the runtime package.
install(TARGETS zenith_engine SDL3-shared
    RUNTIME DESTINATION .
    LIBRARY DESTINATION "${ZENITH_INSTALL_LIBRARY_DIR}" NAMELINK_SKIP
    ARCHIVE DESTINATION lib COMPONENT development EXCLUDE_FROM_ALL
)
install(DIRECTORY "${ZENITH_SHADER_BINARY_DIR}/" DESTINATION shaders FILES_MATCHING PATTERN "*.spv")
install(DIRECTORY "${ZENITH_DERIVED_BINARY_DIR}/" DESTINATION derived FILES_MATCHING PATTERN "*.zderived")
# The runtime's own assets (ZENITH_ENGINE_RUNTIME_ASSETS, none by default),
# then the packaged project (ZENITH_PACKAGE_PROJECT): its assets in the same
# folder (its packaged assetRoot), the project file at the root, every scene
# under its sceneRoot, and its game module at the path the project file names.
# The engine's development content (the demo room, its audio, the showcase)
# ships only when it is the project's asset root, as for the demo project.
set(ZENITH_OVERRIDDEN_RUNTIME_ASSETS "")
if(ZENITH_PACKAGE_HAS_OWN_ASSETS)
    # Collisions that reach this point are overrides: the project's file ships
    # alone, so the archive never holds both spellings of one path.
    zenith_find_package_asset_collisions(ZENITH_OVERRIDDEN_RUNTIME_ASSETS "${ZENITH_ASSET_SOURCE_DIR}"
        "${ZENITH_ENGINE_RUNTIME_ASSETS}" "${ZENITH_PACKAGE_ASSET_DIR}")
    string(TOLOWER "${ZENITH_OVERRIDDEN_RUNTIME_ASSETS}" ZENITH_OVERRIDDEN_RUNTIME_ASSETS)
endif()
foreach(engine_runtime_asset IN LISTS ZENITH_ENGINE_RUNTIME_ASSETS)
    string(TOLOWER "${engine_runtime_asset}" folded_runtime_asset)
    if(folded_runtime_asset IN_LIST ZENITH_OVERRIDDEN_RUNTIME_ASSETS)
        continue()
    endif()
    cmake_path(GET engine_runtime_asset PARENT_PATH engine_runtime_asset_folder)
    install(FILES "${ZENITH_ASSET_SOURCE_DIR}/${engine_runtime_asset}"
        DESTINATION "assets/${engine_runtime_asset_folder}")
    if(EXISTS "${ZENITH_ASSET_SOURCE_DIR}/${engine_runtime_asset}.meta")
        install(FILES "${ZENITH_ASSET_SOURCE_DIR}/${engine_runtime_asset}.meta"
            DESTINATION "assets/${engine_runtime_asset_folder}")
    endif()
endforeach()
install(DIRECTORY "${ZENITH_PACKAGE_ASSET_DIR}/" DESTINATION assets)
install(FILES "${ZENITH_PACKAGE_PROJECT_INSTALL_FILE}" DESTINATION .)
foreach(package_scene IN LISTS ZENITH_PACKAGE_SCENES)
    cmake_path(GET package_scene PARENT_PATH package_scene_folder)
    cmake_path(APPEND ZENITH_PACKAGE_SCENE_ROOT "${package_scene_folder}" OUTPUT_VARIABLE package_scene_destination)
    cmake_path(NORMAL_PATH package_scene_destination)
    install(FILES "${ZENITH_PACKAGE_SCENE_DIR}/${package_scene}" DESTINATION "${package_scene_destination}")
    if(EXISTS "${ZENITH_PACKAGE_SCENE_DIR}/${package_scene}.meta")
        install(FILES "${ZENITH_PACKAGE_SCENE_DIR}/${package_scene}.meta" DESTINATION "${package_scene_destination}")
    endif()
endforeach()
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
install(FILES "${recastnavigation_SOURCE_DIR}/License.txt" DESTINATION licenses RENAME recastnavigation.txt)
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
    add_subdirectory(tests)
endif()
