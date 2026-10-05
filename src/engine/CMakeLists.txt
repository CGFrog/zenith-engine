# zenith_engine: the reusable engine library every game, tool, and test links.
# The root CMakeLists.txt makes it depend on the shader and asset targets.

add_library(zenith_engine SHARED
    diagnostics/Benchmark.cpp
    diagnostics/FrameCaptureConfig.cpp
    diagnostics/ImageComparison.cpp
    diagnostics/Logger.cpp
    diagnostics/RgbaImage.cpp
    diagnostics/StbImageWrite.cpp
    animation/AnimationSystem.cpp
    animation/Animator.cpp
    audio/AudioSystem.cpp
    audio/StbVorbis.cpp
    Engine.cpp
    application/Application.cpp
    application/DeferredAssetCollection.cpp
    application/SimulationClock.cpp
    assets/Assets.cpp
    assets/ModelImporter.cpp
    assets/MeshOptimization.cpp
    assets/ImageDecoder.cpp
    assets/ProductionAssetPipeline.cpp
    assets/CompressedTexture.cpp
    assets/TextureCooker.cpp
    assets/AssetStreaming.cpp
    assets/TextureMipStreaming.cpp
    assets/PackedVertexStreams.cpp
    assets/IrradianceVolumeAsset.cpp
    assets/ColorLut.cpp
    assets/ContentHash.cpp
    assets/AssetGuid.cpp
    assets/AssetMeta.cpp
    assets/AssetDatabase.cpp
    graphics/Renderer.cpp
    graphics/GraphicsValidation.cpp
    graphics/Lighting.cpp
    graphics/DeferredUploadTracker.cpp
    graphics/LocalShadows.cpp
    graphics/ReflectionProbes.cpp
    graphics/Decals.cpp
    graphics/EnvironmentLighting.cpp
    graphics/IrradianceVolume.cpp
    graphics/RenderConfiguration.cpp
    graphics/RenderSubmission.cpp
    graphics/MeshLodSelection.cpp
    graphics/MeshPassFramePlan.cpp
    graphics/ViewShading.cpp
    graphics/Rendering.cpp
    graphics/DebugDraw.cpp
    graphics/RenderPicking.cpp
    graphics/RenderViewPlanning.cpp
    graphics/GpuCullLayout.cpp
    graphics/TemporalQuality.cpp
    graphics/ToneMapping.cpp
    graphics/AmbientOcclusion.cpp
    graphics/VolumetricFog.cpp
    graphics/MotionBlur.cpp
    graphics/DepthOfField.cpp
    graphics/ColorGrading.cpp
    graphics/SpatialUpscale.cpp
    graphics/Particles.cpp
    graphics/MaterialRecord.cpp
    graphics/graph/RenderGraph.cpp
    graphics/passes/MeshPass.cpp
    graphics/passes/MeshPassViews.cpp
    graphics/passes/MeshPassDescriptors.cpp
    graphics/passes/MeshPassPipelines.cpp
    graphics/passes/MeshPassResources.cpp
    graphics/passes/MeshPassTextureStreaming.cpp
    graphics/passes/MeshPassRenderItems.cpp
    graphics/passes/MeshPassShadows.cpp
    graphics/passes/MeshPassRecord.cpp
    graphics/passes/MeshPassRecordView.cpp
    graphics/passes/MeshPassSceneDraws.cpp
    graphics/passes/MeshPassGpuDriven.cpp
    graphics/passes/MeshPassEffects.cpp
    graphics/passes/MeshPassEditorOverlays.cpp
    graphics/passes/GpuDrivenPass.cpp
    graphics/passes/IdBufferPass.cpp
    graphics/passes/DepthPyramid.cpp
    graphics/passes/AmbientOcclusionPass.cpp
    graphics/passes/ClusteredLightingPass.cpp
    graphics/passes/LocalShadowPass.cpp
    graphics/passes/ReflectionProbeClusterPass.cpp
    graphics/passes/DecalClusterPass.cpp
    graphics/passes/VolumetricFogPass.cpp
    graphics/passes/MotionBlurPass.cpp
    graphics/passes/ParticlePass.cpp
    graphics/passes/EnvironmentPass.cpp
    graphics/passes/IrradianceVolumePass.cpp
    graphics/passes/PostProcessPass.cpp
    graphics/passes/ScreenSpaceReflectionPass.cpp
    graphics/passes/TemporalResolvePass.cpp
    graphics/passes/DepthOfFieldPass.cpp
    graphics/passes/SpatialUpscalePass.cpp
    graphics/passes/TransmissionBackground.cpp
    graphics/passes/BloomExposurePass.cpp
    graphics/passes/ColorGradePass.cpp
    graphics/passes/UiPass.cpp
    graphics/passes/DebugDrawPass.cpp
    graphics/pipeline/GraphicsPipeline.cpp
    graphics/pipeline/ComputePipeline.cpp
    graphics/resources/FrameResources.cpp
    graphics/resources/UploadContext.cpp
    graphics/resources/BindlessTextureTable.cpp
    graphics/resources/GeometryArena.cpp
    graphics/resources/IdReadback.cpp
    graphics/resources/ImageReadback.cpp
    graphics/resources/UiImageStore.cpp
    graphics/resources/ViewportTarget.cpp
    graphics/resources/MipChainBlit.cpp
    graphics/resources/DescriptorPoolSizes.cpp
    graphics/shader/ShaderModule.cpp
    graphics/surface/Surface.cpp
    graphics/swapchain/Swapchain.cpp
    graphics/vulkan/VulkanContext.cpp
    input/InputActions.cpp
    input/InputActionsJson.cpp
    input/InputManager.cpp
    input/InputProfiles.cpp
    jobs/JobSystem.cpp
    jobs/FrameTaskGraph.cpp
    behaviour/BehaviourRegistry.cpp
    behaviour/BehaviourScheduler.cpp
    gameplay/DefinitionTable.cpp
    gameplay/FirstPerson.cpp
    gameplay/Random.cpp
    module/BuiltInSystems.cpp
    module/Effects.cpp
    module/EntityCommandBuffer.cpp
    module/EventQueue.cpp
    module/GameModule.cpp
    module/GameServices.cpp
    module/Gameplay.cpp
    module/ModuleApi.cpp
    module/ModuleRegistrar.cpp
    module/ModuleShadowCopy.cpp
    module/SharedLibrary.cpp
    module/SystemScheduler.cpp
    module/Timers.cpp
    navigation/NavAgent.cpp
    navigation/NavigationCrowd.cpp
    navigation/NavigationWorld.cpp
    physics/PhysicsLayers.cpp
    physics/PhysicsWorld.cpp
    physics/JoltDebugDraw.cpp
    physics/JoltRuntime.cpp
    platform/FileWatcher.cpp
    platform/FileWatchBackends.cpp
    platform/FileWatchPolling.cpp
    platform/FileWatchWin32.cpp
    platform/FileWatchInotify.cpp
    platform/ChildProcess.cpp
    platform/CrashHandler.cpp
    platform/FileContents.cpp
    platform/PlatformServices.cpp
    platform/VersionedJson.cpp
    project/ProjectConfig.cpp
    reflection/FieldJson.cpp
    reflection/Reflection.cpp
    resources/ResourceLocator.cpp
    resources/ContentManifest.cpp
    scene/Scene.cpp
scene/SceneTransforms.cpp
scene/TransformTrs.cpp
    scene/ParticleEmitter.cpp
    scene/BuiltInComponentTypes.cpp
    scene/RenderComponentTypes.cpp
    scene/AnimationComponentTypes.cpp
    scene/LightComponentTypes.cpp
    scene/PhysicsComponentTypes.cpp
    scene/GameplayComponentTypes.cpp
    scene/NavigationComponentTypes.cpp
    scene/AudioComponentTypes.cpp
    scene/ComponentTypeHelpers.cpp
    scene/CollectionComponentTypes.cpp
    scene/Collections.cpp
    scene/ComponentTypes.cpp
    scene/SceneAssetReferences.cpp
    scene/SaveGame.cpp
    scene/SceneDocument.cpp
    scene/EntityReferenceRemap.cpp
    scene/SceneVisibility.cpp
    scene/Tags.cpp
    scene/AdditiveSceneManager.cpp
    scene/Prefab.cpp
    scene/PrefabComponentTypes.cpp
    scene/PrefabIdRemap.cpp
    scene/PrefabNesting.cpp
    scene/PrefabOverrides.cpp
    scene/PrefabQueries.cpp
    scene/PrefabSystem.cpp
    ui/GameMenus.cpp
    ui/GameUi.cpp
    ui/Accessibility.cpp
    ui/Localization.cpp
    window/Window.cpp
)

# The stb implementation files compile third-party C code without warnings,
# and without the precompiled header, whose warning state would replace /W0.
set_source_files_properties(audio/StbVorbis.cpp diagnostics/StbImageWrite.cpp
    PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
if(MSVC)
    set_source_files_properties(audio/StbVorbis.cpp PROPERTIES COMPILE_OPTIONS /W0)
    set_source_files_properties(diagnostics/StbImageWrite.cpp PROPERTIES COMPILE_OPTIONS /W0)
endif()

target_include_directories(zenith_engine PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
)

zenith_export_defined_symbols(zenith_engine)
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
if(WIN32)
    # MiniDumpWriteDump, for the crash handler.
    target_link_libraries(zenith_engine PRIVATE dbghelp)
endif()
foreach(header_dependency IN ITEMS volk imgui zenith_imguizmo)
    target_include_directories(zenith_engine PUBLIC
        $<TARGET_PROPERTY:${header_dependency},INTERFACE_INCLUDE_DIRECTORIES>)
    target_compile_definitions(zenith_engine PUBLIC
        $<TARGET_PROPERTY:${header_dependency},INTERFACE_COMPILE_DEFINITIONS>)
endforeach()

zenith_target_warnings(zenith_engine)
zenith_target_precompile_headers(zenith_engine ZenithEnginePch.hpp)
if(MSVC)
    target_compile_options(zenith_engine PRIVATE /FS)
endif()
