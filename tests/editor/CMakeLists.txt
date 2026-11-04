# Scene gizmos in a rendered view under strict validation, plus the fixed
# scene the scene_gizmos reference set captures
# (tests/references/scene_gizmos).
zenith_add_test(zenith_scene_gizmo_vulkan_smoke EXECUTABLE zenith_scene_gizmo_smoke_tests
    SOURCES SceneGizmoSmokeTests.cpp LIBS zenith_editor_app zenith_test_support zenith_test_vulkan_probe
    DEPENDS zenith_assets
    ENVIRONMENT ZENITH_SMOKE_FRAMES=60 ZENITH_VALIDATION_STRICT=1
    LABELS vulkan smoke SKIP_RETURN_CODE 77 TIMEOUT 120 PROCESSORS 2)
# Run by hand for numbers; CTest checks that it completes and that unchanged
# frames rebuild nothing.
zenith_add_test(zenith_outliner_scale_benchmark_run EXECUTABLE zenith_outliner_scale_benchmark
    SOURCES OutlinerScaleBenchmark.cpp LIBS zenith_editor_app zenith_test_support LABELS benchmark TIMEOUT 600)
# Run by hand for numbers; CTest only checks that it completes.
zenith_add_test(zenith_scene_editing_benchmark_run EXECUTABLE zenith_scene_editing_benchmark
    SOURCES SceneEditingBenchmark.cpp LIBS zenith_editor_app zenith_test_support LABELS benchmark TIMEOUT 600)

# A generated project's game module, end to end (GeneratedModuleRuns.cpp):
# configured and built against this build with CMake, which takes a while,
# and for the FPS template played with the built module.
zenith_add_test_executable(zenith_generated_module_runs SOURCES GeneratedModuleRuns.cpp
    LIBS zenith_editor_app zenith_test_support NEEDS_ASSETS
    DEFINITIONS
        ZENITH_TEMPLATE_DIR="${PROJECT_SOURCE_DIR}/templates"
        ZENITH_ENGINE_BUILD_DIR="${PROJECT_BINARY_DIR}"
        ZENITH_EDITOR_CONFIGURATION="$<CONFIG>"
        ZENITH_TEST_CMAKE="${CMAKE_COMMAND}")
zenith_add_test(zenith_project_template_module_build EXECUTABLE zenith_generated_module_runs
    COMMAND_ARGS --build-generated-module LABELS slow TIMEOUT 600)
zenith_add_test(zenith_fps_template_module_build EXECUTABLE zenith_generated_module_runs
    COMMAND_ARGS --build-generated-module --template fps_game LABELS slow TIMEOUT 600)
# What iterating on gameplay code costs through the editor's build request;
# run by hand for numbers.
zenith_add_test(zenith_module_iteration_benchmark_run EXECUTABLE zenith_generated_module_runs
    COMMAND_ARGS --measure-iteration LABELS benchmark TIMEOUT 900)

# Writes the FPS template's scene and prefab (not a test; FpsTemplateTests.cpp
# checks what it wrote): zenith_fps_template_writer templates/fps_game.
zenith_add_test_executable(zenith_fps_template_writer SOURCES FpsTemplateWriter.cpp
    LIBS zenith_editor_app zenith_test_support NEEDS_ASSETS)

# Game modules with editor extensions for the GameModuleHost cases and
# the editor module smoke: one source built as two versions, so a test can
# hot reload one over the other. They link only the engine, as real game
# modules do.
zenith_add_test_module(zenith_editor_test_module SOURCES modules/EditorTestModule.cpp
    LIBS zenith_engine INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/src")
zenith_add_test_module(zenith_editor_test_module_v2 SOURCES modules/EditorTestModule.cpp
    LIBS zenith_engine INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/src"
    DEFINITIONS ZENITH_EDITOR_TEST_MODULE_V2)

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

# The editor's unit tests: doctest cases in one binary, zenith_editor_tests,
# each case its own CTest test "editor.<case>" (docs/TESTING.md). Declared
# last, after the test modules and paths its cases use.
zenith_add_test_suite(editor
    SOURCES
        AnimatorEditorTests.cpp
        AssetBrowserTests.cpp
        AssetImportTests.cpp
        AssetReferenceFieldTests.cpp
        AssetReferenceWorkflowTests.cpp
        BuildWorkflowTests.cpp
        CodeGeneratorTests.cpp
        ColliderToolsTests.cpp
        CommandHistoryTests.cpp
        ComponentEditorTests.cpp
        ComponentOperatorTests.cpp
        CopyNamingTests.cpp
        DebugToolsTests.cpp
        DefaultSceneTests.cpp
        EditorAreaTypesTests.cpp
        EditorCameraTests.cpp
        EditorDocumentTests.cpp
        EditorHardeningTests.cpp
        EditorPaletteTests.cpp
        EditorPlayModeTests.cpp
        EditorRenderSettingsTests.cpp
        EditorSessionTests.cpp
        EditorSettingsTests.cpp
        EditorThemeTests.cpp
        EntityReferenceFieldTests.cpp
        FpsTemplateTests.cpp
        GameModuleHostTests.cpp
        InfoPanelTests.cpp
        IrradianceVolumeBakerTests.cpp
        LiveEditingTests.cpp
        MenuTests.cpp
        MeshAssignTests.cpp
        Milestone19And21Tests.cpp
        ModalTransformTests.cpp
        ModelReplaceTests.cpp
        MultiGizmoTests.cpp
        MultiViewportTests.cpp
        NativeFileDialogTests.cpp
        OperatorFamilyTests.cpp
        OperatorTests.cpp
        OutlinerTests.cpp
        PackageWorkflowTests.cpp
        PickQueriesTests.cpp
        PlacementTests.cpp
        PlayTargetsTests.cpp
        PrefabTests.cpp
        PrefabUiTests.cpp
        PrefabWindowTests.cpp
        PrefabWriteTests.cpp
        PreferencesTests.cpp
        ProjectAssetTests.cpp
        ProjectHubTests.cpp
        ProjectSettingsTests.cpp
        PropertiesPanelTests.cpp
        ReflectedFieldEditorTests.cpp
        SceneEditingTests.cpp
        SceneGizmoDrawTests.cpp
        ScopedCaptureTests.cpp
        SelectionTests.cpp
        SelectionToolsTests.cpp
        SmokeRunnerTests.cpp
        SnappingCursorTests.cpp
        SystemProfilerTests.cpp
        ThumbnailCacheTests.cpp
        TransientEditingTests.cpp
        ViewportChromeTests.cpp
        ViewportPickingTests.cpp
        WorkspaceTests.cpp
    LIBS
        zenith_editor_app
    DEFINITIONS
        ZENITH_TEST_CMAKE="${CMAKE_COMMAND}"
        ZENITH_TEMPLATE_DIR="${PROJECT_SOURCE_DIR}/templates"
        ZENITH_ENGINE_BUILD_DIR="${PROJECT_BINARY_DIR}"
        ZENITH_EDITOR_CONFIGURATION="$<CONFIG>"
        ZENITH_EDITOR_FONT="${inter_SOURCE_DIR}/extras/ttf/Inter-Regular.ttf"
        ZENITH_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        ZENITH_EDITOR_TEST_MODULE_V1="$<TARGET_FILE:zenith_editor_test_module>"
        ZENITH_EDITOR_TEST_MODULE_V2="$<TARGET_FILE:zenith_editor_test_module_v2>"
    DEPENDS
        zenith_assets
        zenith_editor_test_module
        zenith_editor_test_module_v2
    NEEDS_ASSETS)
