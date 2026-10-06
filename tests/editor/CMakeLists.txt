# Tests that compile a few editor sources themselves rather than linking
# zenith_editor_app, so they cover those files without the rest of the editor.
zenith_add_test(zenith_editor_settings
    SOURCES
        EditorSettingsTests.cpp
        ${PROJECT_SOURCE_DIR}/src/editor/settings/EditorSettings.cpp
        ${PROJECT_SOURCE_DIR}/src/editor/theme/ThemeParameters.cpp
    INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/src")
zenith_add_test(zenith_copy_naming
    SOURCES
        CopyNamingTests.cpp
        ${PROJECT_SOURCE_DIR}/src/editor/document/CopyNaming.cpp
    INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/src")
zenith_add_test(zenith_editor_palette
    SOURCES
        EditorPaletteTests.cpp
        ${PROJECT_SOURCE_DIR}/src/editor/theme/EditorPalette.cpp
        ${PROJECT_SOURCE_DIR}/src/editor/theme/PixelIconBitmaps.cpp
        ${PROJECT_SOURCE_DIR}/src/editor/theme/ThemeParameters.cpp
    INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/src")
zenith_add_test(zenith_editor_document
    SOURCES
        EditorDocumentTests.cpp
        ${PROJECT_SOURCE_DIR}/src/editor/document/EditorDocument.cpp
        ${PROJECT_SOURCE_DIR}/src/editor/document/SceneEditorData.cpp
    INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/src"
    LIBS zenith_engine)
zenith_add_test(zenith_command_history
    SOURCES
        CommandHistoryTests.cpp
        ${PROJECT_SOURCE_DIR}/src/editor/document/CommandHistory.cpp
    INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/src"
    LIBS zenith_engine)
zenith_add_test(zenith_project_assets EXECUTABLE zenith_project_asset_tests
    SOURCES
        ProjectAssetTests.cpp
        ${PROJECT_SOURCE_DIR}/src/editor/project/AssetCatalog.cpp
        ${PROJECT_SOURCE_DIR}/src/editor/project/ReflectionProbeBaker.cpp
    INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/src"
    LIBS zenith_engine)
zenith_add_test(zenith_irradiance_volume_baker
    SOURCES
        IrradianceVolumeBakerTests.cpp
        ${PROJECT_SOURCE_DIR}/src/editor/project/IrradianceVolumeBaker.cpp
    INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/src"
    LIBS zenith_engine)
zenith_add_test(zenith_editor_play_mode
    SOURCES
        EditorPlayModeTests.cpp
        ${PROJECT_SOURCE_DIR}/src/editor/application/EditorPlayMode.cpp
    INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/src"
    LIBS zenith_engine)
# The smoke runner alone, with a step context of the test's own.
zenith_add_test(zenith_smoke_runner
    SOURCES
        SmokeRunnerTests.cpp
        ${PROJECT_SOURCE_DIR}/src/editor/smoke/SmokeRunner.cpp
    INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/src")

zenith_add_test(zenith_component_editor SOURCES ComponentEditorTests.cpp LIBS zenith_editor_app)
zenith_add_test(zenith_editor_camera SOURCES EditorCameraTests.cpp LIBS zenith_editor_app NEEDS_ASSETS)
zenith_add_test(zenith_viewport_chrome SOURCES ViewportChromeTests.cpp LIBS zenith_editor_app)
# Viewport scene gizmos recorded on the CPU (editor/viewport/SceneGizmoDraw).
zenith_add_test(zenith_scene_gizmo SOURCES SceneGizmoDrawTests.cpp LIBS zenith_editor_app DEPENDS zenith_assets)
# Scene gizmos in a rendered view under strict validation, plus the fixed
# scene the scene_gizmos reference set captures
# (tests/references/scene_gizmos).
zenith_add_test(zenith_scene_gizmo_vulkan_smoke EXECUTABLE zenith_scene_gizmo_smoke_tests
    SOURCES SceneGizmoCaptureTests.cpp LIBS zenith_editor_app DEPENDS zenith_assets
    ENVIRONMENT ZENITH_SMOKE_FRAMES=60 ZENITH_VALIDATION_STRICT=1
    LABELS vulkan smoke SKIP_RETURN_CODE 77 TIMEOUT 120 PROCESSORS 2)
zenith_add_test(zenith_editor_theme SOURCES EditorThemeTests.cpp LIBS zenith_editor_app
    DEFINITIONS ZENITH_EDITOR_FONT="${inter_SOURCE_DIR}/extras/ttf/Inter-Regular.ttf")
zenith_add_test(zenith_multi_viewport SOURCES MultiViewportTests.cpp LIBS zenith_editor_app)
zenith_add_test(zenith_viewport_picking SOURCES ViewportPickingTests.cpp LIBS zenith_editor_app NEEDS_ASSETS)
zenith_add_test(zenith_snapping_cursor SOURCES SnappingCursorTests.cpp LIBS zenith_editor_app NEEDS_ASSETS)
# Play targets: game views, input routing, mouse capture, the mode tint,
# and the standalone game with a fake process.
zenith_add_test(zenith_play_targets SOURCES PlayTargetsTests.cpp LIBS zenith_editor_app NEEDS_ASSETS)
zenith_add_test(zenith_editor_hardening SOURCES EditorHardeningTests.cpp LIBS zenith_editor_app)
zenith_add_test(zenith_editor_render_settings SOURCES EditorRenderSettingsTests.cpp LIBS zenith_editor_app)
zenith_add_test(zenith_native_file_dialog SOURCES NativeFileDialogTests.cpp LIBS zenith_editor_app)
zenith_add_test(zenith_default_scene SOURCES DefaultSceneTests.cpp LIBS zenith_editor_app NEEDS_ASSETS)
zenith_add_test(zenith_modal_transform SOURCES ModalTransformTests.cpp LIBS zenith_editor_app)
zenith_add_test(zenith_multi_gizmo SOURCES MultiGizmoTests.cpp LIBS zenith_editor_app)
zenith_add_test(zenith_placement SOURCES PlacementTests.cpp LIBS zenith_editor_app NEEDS_ASSETS)
zenith_add_test(zenith_selection_tools SOURCES SelectionToolsTests.cpp LIBS zenith_editor_app NEEDS_ASSETS)
zenith_add_test(zenith_pick_queries SOURCES PickQueriesTests.cpp LIBS zenith_editor_app)
zenith_add_test(zenith_selection SOURCES SelectionTests.cpp LIBS zenith_editor_app)
zenith_add_test(zenith_outliner SOURCES OutlinerTests.cpp LIBS zenith_editor_app)
# Run by hand for numbers; CTest checks that it completes and that unchanged
# frames rebuild nothing.
zenith_add_test(zenith_outliner_scale_benchmark_run EXECUTABLE zenith_outliner_scale_benchmark
    SOURCES OutlinerScaleBenchmark.cpp LIBS zenith_editor_app LABELS benchmark TIMEOUT 600)
zenith_add_test(zenith_scene_editing SOURCES SceneEditingTests.cpp LIBS zenith_editor_app)
zenith_add_test(zenith_scoped_capture SOURCES ScopedCaptureTests.cpp LIBS zenith_editor_app)
zenith_add_test(zenith_transient_editing SOURCES TransientEditingTests.cpp LIBS zenith_editor_app)
# Run by hand for numbers; CTest only checks that it completes.
zenith_add_test(zenith_scene_editing_benchmark_run EXECUTABLE zenith_scene_editing_benchmark
    SOURCES SceneEditingBenchmark.cpp LIBS zenith_editor_app LABELS benchmark TIMEOUT 600)
zenith_add_test(zenith_live_editing SOURCES LiveEditingTests.cpp LIBS zenith_editor_app)
zenith_add_test(zenith_reflected_field_editor SOURCES ReflectedFieldEditorTests.cpp LIBS zenith_editor_app)
zenith_add_test(zenith_properties_panel SOURCES PropertiesPanelTests.cpp LIBS zenith_editor_app)
zenith_add_test(zenith_entity_reference_field SOURCES EntityReferenceFieldTests.cpp LIBS zenith_editor_app)
zenith_add_test(zenith_asset_reference_field SOURCES AssetReferenceFieldTests.cpp LIBS zenith_editor_app NEEDS_ASSETS)
zenith_add_test(zenith_model_replace SOURCES ModelReplaceTests.cpp LIBS zenith_editor_app NEEDS_ASSETS)
zenith_add_test(zenith_mesh_assign SOURCES MeshAssignTests.cpp LIBS zenith_editor_app NEEDS_ASSETS)
# Collider shape switching, offsets, mesh choices, and viewport handles.
zenith_add_test(zenith_collider_tools SOURCES ColliderToolsTests.cpp LIBS zenith_editor_app DEPENDS zenith_assets)
zenith_add_test(zenith_component_operators EXECUTABLE zenith_component_operator_tests
    SOURCES ComponentOperatorTests.cpp LIBS zenith_editor_app)
zenith_add_test(zenith_build_workflow SOURCES BuildWorkflowTests.cpp LIBS zenith_editor_app
    DEFINITIONS ZENITH_TEST_CMAKE="${CMAKE_COMMAND}")

# The Project Hub, recent projects, and the C++ game template. The second
# test also configures and builds a generated project's game module
# against this build with CMake, which takes a while.
zenith_add_test(zenith_project_hub SOURCES ProjectHubTests.cpp LIBS zenith_editor_app NEEDS_ASSETS
    DEFINITIONS
        ZENITH_TEMPLATE_DIR="${PROJECT_SOURCE_DIR}/templates"
        ZENITH_ENGINE_BUILD_DIR="${PROJECT_BINARY_DIR}"
        ZENITH_EDITOR_CONFIGURATION="$<CONFIG>"
        ZENITH_TEST_CMAKE="${CMAKE_COMMAND}")
zenith_add_test(zenith_project_template_module_build EXECUTABLE zenith_project_hub_tests
    COMMAND_ARGS --build-generated-module LABELS slow TIMEOUT 600)
zenith_add_test(zenith_fps_template_module_build EXECUTABLE zenith_project_hub_tests
    COMMAND_ARGS --build-generated-module --template fps_game LABELS slow TIMEOUT 600)
# What iterating on gameplay code costs through the editor's build request;
# run by hand for numbers.
zenith_add_test(zenith_module_iteration_benchmark_run EXECUTABLE zenith_project_hub_tests
    COMMAND_ARGS --measure-iteration LABELS benchmark slow TIMEOUT 900)

# The FPS template's committed scene and prefab; with --write <folder> it
# rewrites them.
zenith_add_test(zenith_fps_template SOURCES FpsTemplateTests.cpp LIBS zenith_editor_app NEEDS_ASSETS
    DEFINITIONS
        ZENITH_TEMPLATE_DIR="${PROJECT_SOURCE_DIR}/templates"
        ZENITH_ENGINE_BUILD_DIR="${PROJECT_BINARY_DIR}")
zenith_add_test(zenith_code_generator SOURCES CodeGeneratorTests.cpp LIBS zenith_editor_app
    DEFINITIONS
        ZENITH_TEMPLATE_DIR="${PROJECT_SOURCE_DIR}/templates"
        ZENITH_TEST_CMAKE="${CMAKE_COMMAND}")

zenith_add_test(zenith_project_settings SOURCES ProjectSettingsTests.cpp LIBS zenith_editor_app)
zenith_add_test(zenith_debug_tools SOURCES DebugToolsTests.cpp LIBS zenith_editor_app
    DEFINITIONS ZENITH_TEST_CMAKE="${CMAKE_COMMAND}")

zenith_add_test(zenith_package_workflow SOURCES PackageWorkflowTests.cpp LIBS zenith_editor_app)
zenith_add_test(zenith_preferences SOURCES PreferencesTests.cpp LIBS zenith_editor_app
    DEFINITIONS ZENITH_TEST_CMAKE="${CMAKE_COMMAND}")
zenith_add_test(zenith_asset_browser SOURCES AssetBrowserTests.cpp LIBS zenith_editor_app)
zenith_add_test(zenith_asset_import SOURCES AssetImportTests.cpp LIBS zenith_editor_app)
zenith_add_test(zenith_asset_reference_workflow SOURCES AssetReferenceWorkflowTests.cpp LIBS zenith_editor_app
    NEEDS_ASSETS DEFINITIONS ZENITH_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
zenith_add_test(zenith_operators EXECUTABLE zenith_operator_tests SOURCES OperatorTests.cpp LIBS zenith_editor_app)
zenith_add_test(zenith_operator_families SOURCES OperatorFamilyTests.cpp LIBS zenith_editor_app)
zenith_add_test(zenith_menus EXECUTABLE zenith_menu_tests SOURCES MenuTests.cpp LIBS zenith_editor_app)
zenith_add_test(zenith_prefabs EXECUTABLE zenith_prefab_tests
    SOURCES PrefabTests.cpp LIBS zenith_editor_app NEEDS_ASSETS)
zenith_add_test(zenith_prefab_ui SOURCES PrefabUiTests.cpp LIBS zenith_editor_app)
zenith_add_test(zenith_prefab_writes SOURCES PrefabWriteTests.cpp LIBS zenith_editor_app)
# Prefab mode: the document stack headless, then the whole session headless.
zenith_add_test(zenith_prefab_edit_mode SOURCES PrefabEditModeTests.cpp LIBS zenith_editor_app NEEDS_ASSETS
    DEFINITIONS
        ZENITH_EDITOR_FONT="${inter_SOURCE_DIR}/extras/ttf/Inter-Regular.ttf"
        ZENITH_EDITOR_TEST_MODULE_V1="$<TARGET_FILE:zenith_editor_test_module>"
    DEPENDS zenith_editor_test_module)
zenith_add_test(zenith_workspaces EXECUTABLE zenith_workspace_tests SOURCES WorkspaceTests.cpp LIBS zenith_editor_app)
# The full editor session driven headlessly, as zenith_editor composes it.
zenith_add_test(zenith_editor_session SOURCES EditorSessionTests.cpp LIBS zenith_editor_app NEEDS_ASSETS
    DEFINITIONS
        ZENITH_TEMPLATE_DIR="${PROJECT_SOURCE_DIR}/templates"
        ZENITH_EDITOR_FONT="${inter_SOURCE_DIR}/extras/ttf/Inter-Regular.ttf"
        ZENITH_EDITOR_TEST_MODULE_V1="$<TARGET_FILE:zenith_editor_test_module>"
    DEPENDS zenith_editor_test_module)
zenith_add_test(zenith_editor_area_types SOURCES EditorAreaTypesTests.cpp LIBS zenith_editor_app)
zenith_add_test(zenith_info_panel SOURCES InfoPanelTests.cpp LIBS zenith_editor_app)
zenith_add_test(zenith_system_profiler SOURCES SystemProfilerTests.cpp LIBS zenith_editor_app
    DEFINITIONS ZENITH_EDITOR_TEST_MODULE_V1="$<TARGET_FILE:zenith_editor_test_module>"
    DEPENDS zenith_editor_test_module)

# Game modules with editor extensions for zenith_game_module_host_tests and
# the editor module smoke: one source built as two versions, so a test can
# hot reload one over the other. They link only the engine, as real game
# modules do.
zenith_add_test_module(zenith_editor_test_module SOURCES modules/EditorTestModule.cpp
    LIBS zenith_engine INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/src")
zenith_add_test_module(zenith_editor_test_module_v2 SOURCES modules/EditorTestModule.cpp
    LIBS zenith_engine INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/src"
    DEFINITIONS ZENITH_EDITOR_TEST_MODULE_V2)
zenith_add_test(zenith_game_module_host SOURCES GameModuleHostTests.cpp LIBS zenith_editor_app
    DEFINITIONS
        ZENITH_EDITOR_TEST_MODULE_V1="$<TARGET_FILE:zenith_editor_test_module>"
        ZENITH_EDITOR_TEST_MODULE_V2="$<TARGET_FILE:zenith_editor_test_module_v2>"
    DEPENDS zenith_editor_test_module zenith_editor_test_module_v2)

# Editor runs start from an empty ZENITH_EDITOR_TEST_SETTINGS_DIR (set in
# tests/CMakeLists.txt) each ctest run.
zenith_add_test(zenith_editor_test_settings_reset EXECUTABLE "${CMAKE_COMMAND}"
    COMMAND_ARGS -E rm -rf "${ZENITH_EDITOR_TEST_SETTINGS_DIR}"
    FIXTURES_SETUP zenith_editor_test_settings)

# The scenario quits when its last step finishes (editor/smoke/,
# docs/editor/smoke.md); ZENITH_SMOKE_FRAMES is only a safety cap above the
# sum of the step budgets (about 1,700 frames). About 5 s alone in Debug
# (8 s before the steps ran back to back); the 180 s timeout covers a full
# ctest -j 8 on a machine also running other build trees' suites.
zenith_add_test(zenith_editor_vulkan_smoke EXECUTABLE zenith_editor
    ENVIRONMENT ZENITH_EDITOR_SMOKE=1 ZENITH_SMOKE_FRAMES=3000
        ZENITH_EDITOR_SETTINGS_DIR=${ZENITH_EDITOR_TEST_SETTINGS_DIR}/vulkan_smoke
    LABELS vulkan smoke TIMEOUT 180 PROCESSORS 2
    FIXTURES_REQUIRED zenith_editor_test_settings)

# The editor smoke on the demo scene with a game module, which the smoke
# scenario also hot reloads once. Project paths are relative to the project
# file.
file(RELATIVE_PATH ZENITH_MODULE_SMOKE_ASSET_ROOT "${PROJECT_BINARY_DIR}" "${PROJECT_SOURCE_DIR}/assets")
file(RELATIVE_PATH ZENITH_MODULE_SMOKE_SCENE_ROOT "${PROJECT_BINARY_DIR}" "${PROJECT_SOURCE_DIR}/projects/scenes")
file(GENERATE OUTPUT "${PROJECT_BINARY_DIR}/editor-module-smoke-$<CONFIG>.zenith-project" CONTENT
"{
  \"version\": 4,
  \"displayName\": \"Zenith Editor Module Smoke\",
  \"assetRoot\": \"${ZENITH_MODULE_SMOKE_ASSET_ROOT}\",
  \"sceneRoot\": \"${ZENITH_MODULE_SMOKE_SCENE_ROOT}\",
  \"startupScene\": \"demo.zenith-scene\",
  \"gameModule\": \"$<PATH:RELATIVE_PATH,$<TARGET_FILE:zenith_editor_test_module>,${PROJECT_BINARY_DIR}>\",
  \"renderSettings\": ${ZENITH_DEMO_RENDER_SETTINGS}
}
")
# The same frame cap as above. About 9 s alone in Debug (12 s before the
# steps ran back to back; it once timed out at 60 s under load); the 180 s
# timeout covers other suites on the same machine.
zenith_add_test(zenith_editor_module_smoke EXECUTABLE zenith_editor
    COMMAND_ARGS "${PROJECT_BINARY_DIR}/editor-module-smoke-$<CONFIG>.zenith-project"
    ENVIRONMENT ZENITH_EDITOR_SMOKE=1 ZENITH_SMOKE_FRAMES=3000
        ZENITH_EDITOR_SETTINGS_DIR=${ZENITH_EDITOR_TEST_SETTINGS_DIR}/module_smoke
    LABELS vulkan smoke TIMEOUT 180 PROCESSORS 2
    FIXTURES_REQUIRED zenith_editor_test_settings)
