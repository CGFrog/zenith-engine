# meshoptimizer is private to the engine library; this test calls it directly.
zenith_add_test(zenith_milestones_19_21 EXECUTABLE zenith_milestone_19_21_tests
    SOURCES Milestone19And21Tests.cpp
    LIBS zenith_engine zenith_demo zenith_editor_app meshoptimizer NEEDS_ASSETS)
zenith_add_test(zenith_production_milestones EXECUTABLE zenith_production_milestone_tests
    SOURCES ProductionMilestonesTests.cpp LIBS zenith_engine NEEDS_ASSETS)
zenith_add_test(zenith_audio SOURCES AudioSystemTests.cpp LIBS zenith_engine NEEDS_ASSETS)

# One CTest per backend: each half waits on real file-system latency, so
# they run in parallel rather than back to back.
zenith_add_test_executable(zenith_file_watcher_tests SOURCES FileWatcherTests.cpp LIBS zenith_engine)
foreach(file_watcher_backend IN ITEMS native polling)
    zenith_add_test(zenith_file_watcher_${file_watcher_backend}
        EXECUTABLE zenith_file_watcher_tests COMMAND_ARGS ${file_watcher_backend})
endforeach()

zenith_add_test(zenith_asset_scene EXECUTABLE zenith_asset_tests
    SOURCES AssetSceneTests.cpp LIBS zenith_engine NEEDS_ASSETS)
zenith_add_test(zenith_asset_database SOURCES AssetDatabaseTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_showcase_content SOURCES ShowcaseContentTests.cpp LIBS zenith_engine
    NEEDS_ASSETS DEFINITIONS ZENITH_PROJECT_DIR="${PROJECT_SOURCE_DIR}/projects")
zenith_add_test(zenith_material_extensions EXECUTABLE zenith_material_extension_tests
    SOURCES MaterialExtensionTests.cpp LIBS zenith_engine NEEDS_ASSETS)
zenith_add_test(zenith_compressed_texture SOURCES CompressedTextureTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_animation SOURCES AnimationTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_physics SOURCES PhysicsTests.cpp LIBS zenith_engine NEEDS_ASSETS)
zenith_add_test(zenith_system_scheduler SOURCES SystemSchedulerTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_game_services SOURCES GameServicesTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_scene_document SOURCES SceneDocumentTests.cpp LIBS zenith_engine NEEDS_ASSETS)
# Reads the asset directory's path only, so it does not build zenith_assets first.
zenith_add_test(zenith_collections EXECUTABLE zenith_collection_tests
    SOURCES CollectionTests.cpp LIBS zenith_engine
    DEFINITIONS ZENITH_ASSET_DIR="${ZENITH_ASSET_BINARY_DIR}")
zenith_add_test(zenith_component_types SOURCES ComponentTypesTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_reflection SOURCES ReflectionTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_file_contents SOURCES FileContentsTests.cpp LIBS zenith_engine)
# Skips (77) where SDL's offscreen video driver cannot create a window.
zenith_add_test(zenith_input_manager SOURCES InputManagerTests.cpp LIBS zenith_engine SKIP_RETURN_CODE 77)

# Game modules for zenith_game_module_tests: a working module, one built for
# another module API version, and a library without module entry points.
# zenith_test_game_module is also played by tests/game and tests/validation.
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
zenith_add_test(zenith_game_module SOURCES GameModuleTests.cpp LIBS zenith_engine
    DEFINITIONS ${game_module_test_definitions}
    DEPENDS zenith_test_game_module zenith_test_mismatched_module zenith_test_not_a_module)

zenith_add_test(zenith_project_config SOURCES ProjectConfigTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_input_actions EXECUTABLE zenith_input_action_tests
    SOURCES InputActionTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_game_ui SOURCES GameUiTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_scene_hierarchy SOURCES SceneHierarchyTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_scene_entity_index SOURCES SceneEntityIndexTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_logger SOURCES LoggerTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_child_process SOURCES ChildProcessTests.cpp LIBS zenith_engine
    DEFINITIONS ZENITH_TEST_CMAKE="${CMAKE_COMMAND}")

# Rendering, on the CPU.
zenith_add_test(zenith_render_submission SOURCES RenderSubmissionTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_gpu_resource_table SOURCES GpuResourceTableTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_mesh_lod SOURCES MeshLodTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_mesh_pass_frame_plan SOURCES MeshPassFramePlanTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_view_shading SOURCES ViewShadingTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_render_views EXECUTABLE zenith_render_view_tests
    SOURCES RenderViewTests.cpp LIBS zenith_engine)
# Debug-draw recording, tessellation and label projection (CPU only).
zenith_add_test(zenith_debug_draw SOURCES DebugDrawTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_render_picking SOURCES RenderPickingTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_deferred_upload_tracker SOURCES DeferredUploadTrackerTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_gpu_cull_layout SOURCES GpuCullLayoutTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_packed_vertex_streams EXECUTABLE zenith_packed_vertex_stream_tests
    SOURCES PackedVertexStreamTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_depth_prepass SOURCES DepthPrepassTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_screen_space_reflections EXECUTABLE zenith_screen_space_reflection_tests
    SOURCES ScreenSpaceReflectionTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_ambient_occlusion SOURCES AmbientOcclusionTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_clustered_lighting SOURCES ClusteredLightingTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_local_shadows EXECUTABLE zenith_local_shadow_tests
    SOURCES LocalShadowTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_volumetric_fog SOURCES VolumetricFogTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_motion_blur SOURCES MotionBlurTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_depth_of_field SOURCES DepthOfFieldTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_color_grading SOURCES ColorGradingTests.cpp LIBS zenith_engine NEEDS_ASSETS)
zenith_add_test(zenith_particles EXECUTABLE zenith_particle_tests
    SOURCES ParticleTests.cpp LIBS zenith_engine NEEDS_ASSETS)
zenith_add_test(zenith_frame_task_graph SOURCES FrameTaskGraphTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_render_graph SOURCES RenderGraphTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_temporal_quality SOURCES TemporalQualityTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_spatial_upscale SOURCES SpatialUpscaleTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_tone_mapping SOURCES ToneMappingTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_reflection_probes EXECUTABLE zenith_reflection_probe_tests
    SOURCES ReflectionProbeTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_decals EXECUTABLE zenith_decal_tests SOURCES DecalTests.cpp LIBS zenith_engine NEEDS_ASSETS)
zenith_add_test(zenith_environment_lighting SOURCES EnvironmentLightingTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_irradiance_volume SOURCES IrradianceVolumeTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_asset_streaming SOURCES AssetStreamingTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_render_regression SOURCES RenderRegressionTests.cpp)
zenith_add_test(zenith_frame_capture SOURCES FrameCaptureTests.cpp LIBS zenith_engine)
zenith_add_test(zenith_graphics_validation SOURCES GraphicsValidationTests.cpp LIBS zenith_engine)

# Engine-hosted runs on a Vulkan device; they skip (77) without one.
# Two created render views beside the default one (docs/ARCHITECTURE.md,
# Render views).
zenith_add_test(zenith_multi_view_vulkan_smoke EXECUTABLE zenith_multi_view_smoke_tests
    SOURCES MultiViewSmokeTests.cpp LIBS zenith_engine DEPENDS zenith_assets
    ENVIRONMENT ZENITH_SMOKE_FRAMES=120
    LABELS vulkan smoke SKIP_RETURN_CODE 77 TIMEOUT 120 PROCESSORS 2)
# Per-view shadow isolation: an isolated view's sun, point and spot shadows
# cast from its isolated objects only, beside a view of the whole scene.
zenith_add_test(zenith_isolated_shadow_vulkan_smoke EXECUTABLE zenith_isolated_shadow_smoke_tests
    SOURCES IsolatedShadowSmokeTests.cpp LIBS zenith_engine DEPENDS zenith_assets
    ENVIRONMENT ZENITH_SMOKE_FRAMES=80 ZENITH_DISABLE_SHADOW_CACHE=1 ZENITH_VALIDATION_STRICT=1
    LABELS vulkan smoke SKIP_RETURN_CODE 77 TIMEOUT 120 PROCESSORS 2)
# Debug lines in the default view and two created views under strict
# validation, plus the fixed primitive set the debug_draw reference set
# captures (tests/references/debug_draw).
zenith_add_test(zenith_debug_draw_vulkan_smoke EXECUTABLE zenith_debug_draw_smoke_tests
    SOURCES DebugDrawSmokeTests.cpp LIBS zenith_engine DEPENDS zenith_assets
    ENVIRONMENT ZENITH_SMOKE_FRAMES=60 ZENITH_VALIDATION_STRICT=1
    LABELS vulkan smoke SKIP_RETURN_CODE 77 TIMEOUT 120 PROCESSORS 2)
# GPU picking through the id buffer (docs/ARCHITECTURE.md, Picking).
zenith_add_test(zenith_picking_vulkan_smoke EXECUTABLE zenith_picking_smoke_tests
    SOURCES PickingSmokeTests.cpp LIBS zenith_engine DEPENDS zenith_assets
    ENVIRONMENT ZENITH_SMOKE_FRAMES=80
    LABELS vulkan smoke SKIP_RETURN_CODE 77 TIMEOUT 120 PROCESSORS 2)

# The benchmarks are run by hand for numbers; CTest only checks that they
# complete and exit cleanly so they cannot silently rot.
zenith_add_test(zenith_render_packet_benchmark_run EXECUTABLE zenith_render_packet_benchmark
    SOURCES RenderPacketBenchmarkTests.cpp LIBS zenith_engine LABELS benchmark)
zenith_add_test(zenith_simulation_benchmark_run EXECUTABLE zenith_simulation_benchmark
    SOURCES SimulationBenchmark.cpp LIBS zenith_engine LABELS benchmark)
zenith_add_test(zenith_scene_scaling_benchmark_run EXECUTABLE zenith_scene_scaling_benchmark
    SOURCES SceneScalingBenchmark.cpp LIBS zenith_engine LABELS benchmark)
zenith_add_test(zenith_benchmark SOURCES BenchmarkTests.cpp LIBS zenith_engine)
