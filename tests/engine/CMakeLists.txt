# Game modules GameModuleTests loads, and where it finds them.
zenith_add_test_module(zenith_test_game_module SOURCES modules/TestGameModule.cpp LIBS zenith_engine)
zenith_add_test_module(zenith_test_mismatched_module SOURCES modules/MismatchedApiModule.cpp
    INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/src")
zenith_add_test_module(zenith_test_not_a_module SOURCES modules/NotAModule.cpp
    INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/src")
set(game_module_test_definitions
    ZENITH_TEST_GAME_MODULE="$<TARGET_FILE:zenith_test_game_module>"
    ZENITH_TEST_MISMATCHED_MODULE="$<TARGET_FILE:zenith_test_mismatched_module>"
    ZENITH_TEST_NOT_A_MODULE="$<TARGET_FILE:zenith_test_not_a_module>")
if(MSVC)
    list(APPEND game_module_test_definitions
        ZENITH_TEST_GAME_MODULE_PDB="$<TARGET_PDB_FILE:zenith_test_game_module>")
endif()

# The engine's unit tests: doctest cases in one binary, zenith_engine_tests,
# each case its own CTest test "engine.<case>" (docs/TESTING.md). Files move
# here from the standalone registrations below as they are converted.
zenith_add_test_suite(engine
    SOURCES
        AmbientOcclusionTests.cpp
        AnimationTests.cpp
        AnimatorTests.cpp
        AssetCollectionTests.cpp
        AssetDatabaseTests.cpp
        AssetReloadTests.cpp
        AssetSceneTests.cpp
        AssetStreamingTests.cpp
        AudioSystemTests.cpp
        BehaviourTests.cpp
        CharacterControllerTests.cpp
        ChildProcessTests.cpp
        ClusteredLightingTests.cpp
        CollectionTests.cpp
        ColorGradingTests.cpp
        ComponentTypesTests.cpp
        CompressedTextureTests.cpp
        CookedMeshTests.cpp
        CrashHandlerTests.cpp
        DebugDrawTests.cpp
        DecalTests.cpp
        DeferredUploadTrackerTests.cpp
        DefinitionTableTests.cpp
        DepthOfFieldTests.cpp
        DepthPrepassTests.cpp
        EnvironmentLightingTests.cpp
        FileContentsTests.cpp
        FloorLifecycleTests.cpp
        FrameCaptureTests.cpp
        FrameTaskGraphTests.cpp
        GameMenusTests.cpp
        GameModuleTests.cpp
        GameServicesTests.cpp
        GameUiTests.cpp
        GpuCullLayoutTests.cpp
        GpuResourceTableTests.cpp
        GraphicsValidationTests.cpp
        InputActionTests.cpp
        InputActionsJsonTests.cpp
        IrradianceVolumeTests.cpp
        LocalShadowTests.cpp
        LoggerTests.cpp
        MaterialAssetTests.cpp
        MaterialExtensionTests.cpp
        MeshLodTests.cpp
        MeshPassFramePlanTests.cpp
        MotionBlurTests.cpp
        NavigationTests.cpp
        PackedVertexStreamTests.cpp
        ParticleTests.cpp
        PhysicsQueryTests.cpp
        PhysicsTests.cpp
        PrefabSpawnTests.cpp
        ProductionMilestonesTests.cpp
        ProjectConfigTests.cpp
        RandomTests.cpp
        ReflectionProbeTests.cpp
        ReflectionTests.cpp
        RenderGraphTests.cpp
        RenderPickingTests.cpp
        RenderRegressionTests.cpp
        RenderSubmissionTests.cpp
        RenderViewTests.cpp
        RuntimeContentTests.cpp
        RuntimeLimitTests.cpp
        SaveGameTests.cpp
        SceneDocumentTests.cpp
        SceneEntityIndexTests.cpp
        SceneHierarchyTests.cpp
        SceneTransformTests.cpp
        ScreenSpaceReflectionTests.cpp
        ShowcaseContentTests.cpp
        SpatialUpscaleTests.cpp
        SystemSchedulerTests.cpp
        TagsTests.cpp
        TemporalQualityTests.cpp
        ToneMappingTests.cpp
        TransientEntityTests.cpp
        VersionedJsonTests.cpp
        ViewShadingTests.cpp
        VolumetricFogTests.cpp
    LIBS
        zenith_engine
    DEFINITIONS
        ZENITH_TEST_CMAKE="${CMAKE_COMMAND}"
        ZENITH_PROJECT_DIR="${PROJECT_SOURCE_DIR}/projects"
        ${game_module_test_definitions}
    DEPENDS
        zenith_test_game_module
        zenith_test_mismatched_module
        zenith_test_not_a_module
    NEEDS_ASSETS)

# One CTest per backend: each half waits on real file-system latency, so
# they run in parallel rather than back to back.
zenith_add_test_executable(zenith_file_watcher_tests SOURCES FileWatcherTests.cpp LIBS zenith_engine zenith_test_support)
foreach(file_watcher_backend IN ITEMS native polling)
    zenith_add_test(zenith_file_watcher_${file_watcher_backend}
        EXECUTABLE zenith_file_watcher_tests COMMAND_ARGS ${file_watcher_backend} TIMEOUT 60)
endforeach()

# Reads the asset directory's path only, so it does not build zenith_assets first.
# Skips (77) where SDL's offscreen video driver cannot create a window.
zenith_add_test(zenith_input_manager SOURCES InputManagerTests.cpp LIBS zenith_engine zenith_test_support SKIP_RETURN_CODE 77)

# Game modules for zenith_game_module_tests: a working module, one built for
# another module API version, and a library without module entry points.
# zenith_test_game_module is also played by tests/game and tests/validation.


# Rendering, on the CPU.
# Per-frame capacity clamps and their once-per-episode reporting.
# Debug-draw recording, tessellation and label projection (CPU only).

# Engine-hosted runs on a Vulkan device; they skip (77) without one.
# Two created render views beside the default one
# (docs/engine/graphics-views.md, Render views).
zenith_add_test(zenith_multi_view_vulkan_smoke EXECUTABLE zenith_multi_view_smoke_tests
    SOURCES MultiViewSmokeTests.cpp LIBS zenith_engine zenith_test_support zenith_test_vulkan_probe DEPENDS zenith_assets
    ENVIRONMENT ZENITH_SMOKE_FRAMES=120
    LABELS vulkan smoke SKIP_RETURN_CODE 77 TIMEOUT 120 PROCESSORS 2)
# Secondary render scenes: a created view draws a second scene with preview
# lighting beside the application's scene.
zenith_add_test(zenith_two_scene_vulkan_smoke EXECUTABLE zenith_two_scene_smoke_tests
    SOURCES TwoSceneSmokeTests.cpp LIBS zenith_engine zenith_test_support zenith_test_vulkan_probe DEPENDS zenith_assets
    ENVIRONMENT ZENITH_SMOKE_FRAMES=90
    LABELS vulkan smoke SKIP_RETURN_CODE 77 TIMEOUT 120 PROCESSORS 2)
# Per-view shadow isolation: an isolated view's sun, point and spot shadows
# cast from its isolated objects only, beside a view of the whole scene.
zenith_add_test(zenith_isolated_shadow_vulkan_smoke EXECUTABLE zenith_isolated_shadow_smoke_tests
    SOURCES IsolatedShadowSmokeTests.cpp LIBS zenith_engine zenith_test_support zenith_test_vulkan_probe DEPENDS zenith_assets
    ENVIRONMENT ZENITH_SMOKE_FRAMES=80 ZENITH_DISABLE_SHADOW_CACHE=1 ZENITH_VALIDATION_STRICT=1
    LABELS vulkan smoke SKIP_RETURN_CODE 77 TIMEOUT 120 PROCESSORS 2)
# Debug lines in the default view and two created views under strict
# validation, plus the fixed primitive set the debug_draw reference set
# captures (tests/references/debug_draw).
zenith_add_test(zenith_debug_draw_vulkan_smoke EXECUTABLE zenith_debug_draw_smoke_tests
    SOURCES DebugDrawSmokeTests.cpp LIBS zenith_engine zenith_test_support zenith_test_vulkan_probe DEPENDS zenith_assets
    ENVIRONMENT ZENITH_SMOKE_FRAMES=60 ZENITH_VALIDATION_STRICT=1
    LABELS vulkan smoke SKIP_RETURN_CODE 77 TIMEOUT 120 PROCESSORS 2)
# GPU picking through the id buffer (docs/engine/graphics-views.md, Picking).
zenith_add_test(zenith_picking_vulkan_smoke EXECUTABLE zenith_picking_smoke_tests
    SOURCES PickingSmokeTests.cpp LIBS zenith_engine zenith_test_support zenith_test_vulkan_probe DEPENDS zenith_assets
    ENVIRONMENT ZENITH_SMOKE_FRAMES=80
    LABELS vulkan smoke SKIP_RETURN_CODE 77 TIMEOUT 120 PROCESSORS 2)
# More renderables and particle emitters than the per-frame capacities: the
# excess is clamped, counted and logged once instead of ending the run.
zenith_add_test(zenith_render_capacity_vulkan_smoke EXECUTABLE zenith_render_capacity_smoke_tests
    SOURCES RenderCapacitySmokeTests.cpp LIBS zenith_engine zenith_test_support zenith_test_vulkan_probe DEPENDS zenith_assets
    ENVIRONMENT ZENITH_SMOKE_FRAMES=20
    LABELS vulkan smoke SKIP_RETURN_CODE 77 TIMEOUT 180 PROCESSORS 2)
# A runtime-created mesh, material and texture drawn with a material override
# tint and a replacement material, checked in a captured frame; once on the
# CPU-submitted path and once on the GPU-driven one.
zenith_add_test(zenith_runtime_content_vulkan_smoke EXECUTABLE zenith_runtime_content_smoke_tests
    SOURCES RuntimeContentSmokeTests.cpp LIBS zenith_engine zenith_test_support zenith_test_vulkan_probe DEPENDS zenith_assets
    ENVIRONMENT ZENITH_CAPTURE_FRAME=12 ZENITH_WINDOW_SIZE=480x270
    "ZENITH_CAPTURE_PATH=${PROJECT_BINARY_DIR}/test-temp/runtime_content_cpu.png"
    LABELS vulkan smoke SKIP_RETURN_CODE 77 TIMEOUT 120 PROCESSORS 2)
zenith_add_test(zenith_runtime_content_gpu_driven_vulkan_smoke EXECUTABLE zenith_runtime_content_smoke_tests
    ENVIRONMENT ZENITH_CAPTURE_FRAME=12 ZENITH_WINDOW_SIZE=480x270 ZENITH_FORCE_GPU_DRIVEN=1
    "ZENITH_CAPTURE_PATH=${PROJECT_BINARY_DIR}/test-temp/runtime_content_gpu_driven.png"
    LABELS vulkan smoke SKIP_RETURN_CODE 77 TIMEOUT 120 PROCESSORS 2)

# The benchmarks are run by hand for numbers; CTest only checks that they
# complete and exit cleanly so they cannot silently rot.
zenith_add_test(zenith_render_packet_benchmark_run EXECUTABLE zenith_render_packet_benchmark
    SOURCES RenderPacketBenchmarkTests.cpp LIBS zenith_engine LABELS benchmark)
zenith_add_test(zenith_simulation_benchmark_run EXECUTABLE zenith_simulation_benchmark
    SOURCES SimulationBenchmark.cpp LIBS zenith_engine LABELS benchmark)
zenith_add_test(zenith_scene_scaling_benchmark_run EXECUTABLE zenith_scene_scaling_benchmark
    SOURCES SceneScalingBenchmark.cpp LIBS zenith_engine LABELS benchmark)
zenith_add_test(zenith_prefab_benchmark_run EXECUTABLE zenith_prefab_benchmark
    SOURCES PrefabBenchmark.cpp LIBS zenith_engine zenith_test_support LABELS benchmark TIMEOUT 600)
zenith_add_test(zenith_floor_benchmark_run EXECUTABLE zenith_floor_benchmark
    SOURCES FloorBenchmark.cpp LIBS zenith_engine zenith_test_support LABELS benchmark TIMEOUT 600)
# Secondary render scenes (the editor's prefab windows) per scene count; a
# Vulkan run, so it needs a GPU. CTest runs a few small frames to check it
# completes.
zenith_add_test(zenith_secondary_scene_benchmark_run EXECUTABLE zenith_secondary_scene_benchmark
    SOURCES SecondarySceneBenchmark.cpp LIBS zenith_engine DEPENDS zenith_assets
    ENVIRONMENT ZENITH_SECONDARY_BENCHMARK_OBJECTS=50 ZENITH_SECONDARY_BENCHMARK_WARMUP=2
        ZENITH_SECONDARY_BENCHMARK_FRAMES=3
    LABELS benchmark vulkan SKIP_RETURN_CODE 77 TIMEOUT 600 PROCESSORS 2)
zenith_add_test(zenith_benchmark SOURCES BenchmarkTests.cpp LIBS zenith_engine)
