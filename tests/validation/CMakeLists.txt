# Strict validation smoke matrix (docs/RENDERER_VALIDATION.md): the game, the
# editor, and Engine-hosted smoke executables declared in tests/engine. Any
# counted validation or synchronization message fails the run; exit code 77
# marks it skipped when VK_LAYER_KHRONOS_validation is not installed.

# zenith_add_validation_smoke(<name> COMMAND <program> <arg>...
#     [ENVIRONMENT <VAR=value>...] [FIXTURES_REQUIRED <fixture>...])
function(zenith_add_validation_smoke test_name)
    cmake_parse_arguments(PARSE_ARGV 1 SMOKE "" "" "COMMAND;ENVIRONMENT;FIXTURES_REQUIRED")
    list(POP_FRONT SMOKE_COMMAND smoke_executable)
    set(fixture_arguments)
    if(SMOKE_FIXTURES_REQUIRED)
        set(fixture_arguments FIXTURES_REQUIRED ${SMOKE_FIXTURES_REQUIRED})
    endif()
    zenith_add_test(${test_name} EXECUTABLE ${smoke_executable} COMMAND_ARGS ${SMOKE_COMMAND}
        ENVIRONMENT ZENITH_VALIDATION_STRICT=1 ${SMOKE_ENVIRONMENT}
        LABELS vulkan validation
        SKIP_RETURN_CODE 77
        TIMEOUT 300
        # Counts double against ctest -j so at most half the slots run an
        # engine instance on the shared GPU.
        PROCESSORS 2
        ${fixture_arguments})
endfunction()

set(ZENITH_VALIDATION_SCENES demo showcase)
set(ZENITH_VALIDATION_COMMAND_demo zenith)
set(ZENITH_VALIDATION_COMMAND_showcase zenith --project "${PROJECT_SOURCE_DIR}/projects/showcase.zenith-project")
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

# The game with a project's game module (docs/RELEASING.md): the demo
# project through this build's zenith_game_demo module, also with --scene
# choosing another scene, and the project-agnostic runtime playing the
# engine's test module. Project paths are relative to the project file.
file(RELATIVE_PATH ZENITH_GAME_PROJECT_ASSET_ROOT "${PROJECT_BINARY_DIR}" "${PROJECT_SOURCE_DIR}/assets")
file(RELATIVE_PATH ZENITH_GAME_PROJECT_SCENE_ROOT "${PROJECT_BINARY_DIR}" "${PROJECT_SOURCE_DIR}/projects/scenes")
foreach(game_project IN ITEMS demo_module test_module)
    if(game_project STREQUAL "demo_module")
        set(game_project_module zenith_game_demo)
        set(game_project_scene demo.zenith-scene)
    else()
        set(game_project_module zenith_test_game_module)
        set(game_project_scene irradiance_room.zenith-scene)
    endif()
    file(GENERATE OUTPUT "${PROJECT_BINARY_DIR}/game-${game_project}-$<CONFIG>.zenith-project" CONTENT
"{
  \"version\": 4,
  \"displayName\": \"Zenith Game ${game_project}\",
  \"assetRoot\": \"${ZENITH_GAME_PROJECT_ASSET_ROOT}\",
  \"sceneRoot\": \"${ZENITH_GAME_PROJECT_SCENE_ROOT}\",
  \"startupScene\": \"${game_project_scene}\",
  \"gameModule\": \"$<PATH:RELATIVE_PATH,$<TARGET_FILE:${game_project_module}>,${PROJECT_BINARY_DIR}>\",
  \"renderSettings\": ${ZENITH_DEMO_RENDER_SETTINGS}
}
")
endforeach()
zenith_add_validation_smoke(zenith_validation_demo_module
    COMMAND zenith --project "${PROJECT_BINARY_DIR}/game-demo_module-$<CONFIG>.zenith-project"
    ENVIRONMENT ZENITH_SMOKE_FRAMES=120
)
zenith_add_validation_smoke(zenith_validation_demo_module_scene
    COMMAND zenith --project "${PROJECT_BINARY_DIR}/game-demo_module-$<CONFIG>.zenith-project"
        --scene "${PROJECT_SOURCE_DIR}/projects/scenes/irradiance_room.zenith-scene"
    ENVIRONMENT ZENITH_SMOKE_FRAMES=120
)
zenith_add_validation_smoke(zenith_validation_project_runtime
    COMMAND zenith --project "${PROJECT_BINARY_DIR}/game-test_module-$<CONFIG>.zenith-project"
    ENVIRONMENT ZENITH_SMOKE_FRAMES=120
)
zenith_add_validation_smoke(zenith_validation_editor
    COMMAND zenith_editor
    ENVIRONMENT ZENITH_EDITOR_SMOKE=1 ZENITH_SMOKE_FRAMES=900
        ZENITH_EDITOR_SETTINGS_DIR=${ZENITH_EDITOR_TEST_SETTINGS_DIR}/validation_editor
    FIXTURES_REQUIRED zenith_editor_test_settings
)
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
