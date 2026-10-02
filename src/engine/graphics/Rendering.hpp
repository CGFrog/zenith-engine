#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <glm/glm.hpp>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

using RenderObjectId = uint32_t;

struct UiTextureHandle {
    uint64_t value{0};

    explicit operator bool() const {
        return value != 0;
    }
};

enum class AntialiasingMode : uint8_t {
    Off,
    Fxaa,
    Taa,
};

enum class AmbientOcclusionQuality : uint8_t {
    Off,
    HalfResolution,
    FullResolution,
};

enum class ReflectionQuality : uint8_t {
    ProbesOnly,
    ScreenSpaceHalfResolution,
    ScreenSpaceFullResolution,
};

// How reflection probes reach shading. CameraOnly blends the dominant probe at
// the camera over the whole frame (the fallback); PerPixel assigns resident
// probes to the light-cluster grid and blends them per pixel with box
// projection. Serialized by value.
enum class ReflectionProbeBlending : uint8_t {
    CameraOnly,
    PerPixel,
};

enum class ShadowQuality : uint8_t {
    Off,
    Single,
    CascadedBalanced,
    CascadedHigh,
};

enum class LightingQuality : uint8_t {
    Fast,
    PhysicallyBased,
};

// Downsampled is the compute bloom chain (13-tap downsample, tent upsample).
enum class BloomQuality : uint8_t {
    Off,
    Downsampled,
};

enum class TonemapOperator : uint8_t {
    Aces,
    AgX,
    AgXPunchy,
};

enum class ExposureMode : uint8_t {
    Auto,
    Manual,
};

// Luminance values are in log2 units (EV) of linear scene luminance. Exposure is
// a multiplier on scene radiance: Manual uses 2^(manualEv + compensationEv); Auto
// maps the histogram's percentile-averaged luminance to middle grey and then
// applies compensationEv. Adaptation speeds are rates per second in log2 space;
// "up" applies while exposure rises (the scene got darker).
struct ExposureSettings {
    ExposureMode mode{ExposureMode::Auto};
    float compensationEv{0.0f};
    float manualEv{0.0f};
    float minimumLogLuminance{-10.0f};
    float maximumLogLuminance{6.0f};
    float adaptationSpeedUp{1.5f};
    float adaptationSpeedDown{3.0f};
    float lowPercentile{0.5f};
    float highPercentile{0.95f};
};

enum class EnvironmentQuality : uint8_t {
    Hemisphere,
    ImageBasedLow,
    ImageBasedHigh,
};

// Serialized by value. Older documents stored 1 and 2 for the retired 32- and
// 64-light tiled modes; both load as Clustered.
enum class LocalLightingQuality : uint8_t {
    Disabled,
    Clustered,
};

// Shadows of point and spot lights that opt in (castsShadows), rendered into
// one depth atlas. Higher levels raise the atlas size, the tile resolution,
// the number of shadowed lights, and the tiles re-rendered per frame; see
// localShadowProfile in LocalShadows.hpp. Off leaves local lights unshadowed.
enum class LocalShadowQuality : uint8_t {
    Off,
    Low,
    Medium,
    High,
};

// Froxel volumetric fog (VolumetricFogPass): Low 80x45x48 froxels sampled at
// their centers, Medium 160x90x72 and High 160x90x96 with jittered samples and
// temporal reprojection (see volumetric_fog::froxelSubdivision). The medium
// itself is authored per scene with a VolumetricFogComponent; Off composites
// nothing and records no fog work.
enum class VolumetricFogQuality : uint8_t {
    Off,
    Low,
    Medium,
    High,
};

// GPU particles (ParticlePass): the total particle budget shared by every
// emitter in the scene, Low 16384 and High 131072 (see
// particles::particleBudget). Off records no particle work.
enum class ParticleQuality : uint8_t {
    Off,
    Low,
    High,
};

// Source of indirect diffuse light. Environment (the Off fallback) evaluates
// the environment's spherical harmonics everywhere; ProbeVolume uses a baked
// irradiance volume inside its box and the environment elsewhere.
enum class IndirectDiffuseMode : uint8_t {
    Environment,
    ProbeVolume,
};

// Camera and object motion blur (MotionBlurPass) reconstructed from the
// motion vectors on the HDR scene after the temporal resolve: Low gathers 8
// samples per blurred pixel, High 16. Off records no work.
enum class MotionBlurQuality : uint8_t {
    Off,
    Low,
    High,
};

// Thin-lens depth of field on the HDR scene (DepthOfFieldPass), after the
// temporal resolve and before exposure, bloom, and tone mapping. Both qualities
// gather at half resolution and composite at full resolution; Low spaces its
// gather rings twice as far apart as High (depth_of_field::gatherRingSpacing).
// Off records no work.
enum class DepthOfFieldQuality : uint8_t {
    Off,
    Low,
    High,
};

// Physical camera that shapes the circle of confusion. DepthOfField.hpp holds
// the math and the accepted ranges.
struct DepthOfFieldSettings {
    DepthOfFieldQuality quality{DepthOfFieldQuality::Off};
    // Distance in meters, along the view axis, of the plane in perfect focus.
    float focusDistance{5.0f};
    // Aperture as an f-number (focal length / aperture diameter); smaller
    // values blur more.
    float fStop{2.8f};
    // Film-back (sensor) height in millimeters. With the camera's vertical
    // field of view it sets the focal length, so larger sensors blur more at
    // equal framing.
    float sensorHeightMillimeters{24.0f};
    // Largest circle-of-confusion radius, in pixels of the blurred image
    // (output resolution with TAA, render resolution otherwise). Bounds the
    // gather cost.
    float maximumCocRadiusPixels{12.0f};
};

// Display-referred finishing effects applied after tone mapping by
// ColorGradePass, in this order: the LUT grade, the vignette, then film grain.
// Each is selected independently and Off by default; the pass records no work
// while all of them are off. ColorGrading.hpp holds the CPU reference math.
struct ColorLutSettings {
    bool enabled{false};
    // A .cube file (see ColorLut.hpp), relative to the render asset root
    // (RenderRuntime::setAssetRoot) or absolute. Empty applies no LUT; a file
    // that cannot be loaded is reported once and also applies no LUT.
    std::string path;
    // Mix of the graded colour over the ungraded one, 0..1.
    float strength{1.0f};
};

struct VignetteSettings {
    bool enabled{false};
    // Darkening at the frame corners, 0..1: corners keep (1 - intensity).
    float intensity{0.3f};
    // Width of the falloff toward the corners, as a fraction of the
    // centre-to-corner distance, 0.01..1.
    float smoothness{0.5f};
    // 1 keeps the vignette circular on screen; 0 stretches it to the frame's
    // aspect ratio.
    float roundness{1.0f};
};

struct FilmGrainSettings {
    bool enabled{false};
    // Peak grain amplitude in display-encoded (sRGB) units, 0..0.5.
    float intensity{0.05f};
    // Grain cell edge in output pixels, 1..8. The pattern changes every frame,
    // seeded by the frame serial, so deterministic runs repeat it exactly.
    float size{1.5f};
};

// Clustered decals (DecalComponent): On assigns the scene's decals to the
// light-cluster grid and blends them into opaque and masked surfaces before
// lighting; Off (the fallback) skips both. Serialized by value.
enum class DecalQuality : uint8_t {
    Off,
    On,
};

struct RenderPassBudget {
    double cpuMilliseconds{0.0};
    double gpuMilliseconds{0.0};
};

struct RenderPassBudgets {
    RenderPassBudget shadow;
    RenderPassBudget opaque;
    RenderPassBudget blended;
    RenderPassBudget bloom;
    RenderPassBudget ui;
    RenderPassBudget upload;
    RenderPassBudget temporalResolve;
    RenderPassBudget ambientOcclusion;
    RenderPassBudget reflections;
    RenderPassBudget exposure;
    RenderPassBudget depthPrepass;
    RenderPassBudget depthPyramid;
    RenderPassBudget lightClusters;
    // Environment precompute (only when its inputs change) plus the skybox.
    RenderPassBudget environment;
    // GPU culling of the redrawn shadow cascades (GPU-driven path).
    RenderPassBudget shadowCulling;
    // Local-light shadow atlas tiles re-rendered this frame.
    RenderPassBudget localShadows;
    // Per-pixel reflection probe upload and cluster assignment.
    RenderPassBudget reflectionProbeClusters;
    RenderPassBudget volumetricFog;
    // Irradiance-volume probe upload (sampling is part of opaque shading).
    RenderPassBudget irradianceVolume;
    // Velocity tiles and the reconstruction gather.
    RenderPassBudget motionBlur;
    RenderPassBudget depthOfField;
    // Display-referred LUT grade, vignette and film grain.
    RenderPassBudget colorGrade;
    // FSR 1 EASU upsample plus RCAS sharpen.
    RenderPassBudget spatialUpscale;
    // Decal cluster assignment (blending is part of opaque shading).
    RenderPassBudget decalClusters;
    // Particle emission, simulation, lighting, and sorting.
    RenderPassBudget particleSimulation;
    // Particle billboards (within the blended scene time).
    RenderPassBudget particleDraw;
};

struct DynamicResolutionSettings {
    bool enabled{false};
    float minimumScale{0.67f};
    float maximumScale{1.0f};
    float targetGpuMilliseconds{14.0f};
    float adjustmentRate{0.025f};
    uint32_t quantization{16};
};

// How the frame reaches output resolution when it renders below it. Off keeps
// the historical behavior: the internal resolution follows dynamicResolution
// alone (1.0 when it is disabled), and TAA upsamples whatever it renders.
// Taau renders at the preset's scale and reconstructs with temporal
// anti-aliasing (which it requires). Fsr1 renders at the preset's scale,
// anti-aliases at render resolution (FXAA, or TAA resolving at render
// resolution), tone maps, then upsamples with AMD FSR 1 EASU and sharpens with
// RCAS (SpatialUpscalePass). With dynamic resolution enabled, an upscaler's
// scale range applies relative to its preset (upscaledDynamicResolution in
// SpatialUpscale.hpp). A vendor upscaler such as DLSS would be another value
// here behind its own capability check. Serialized by value.
enum class UpscalerMode : uint8_t {
    Off,
    Taau,
    Fsr1,
};

// Output-to-render ratio per axis: Quality 1.5x, Balanced 1.7x, Performance
// 2x (AMD's FSR 1 presets). See upscalerScaleFactor in SpatialUpscale.hpp.
enum class UpscalerQuality : uint8_t {
    Quality,
    Balanced,
    Performance,
};

struct UpscalerSettings {
    UpscalerMode mode{UpscalerMode::Off};
    UpscalerQuality quality{UpscalerQuality::Quality};
    // RCAS sharpening in [0, 1]; 1 is AMD's maximum (0 stops of attenuation)
    // and each 0.5 below it halves the sharpening. Used by Fsr1 only.
    float sharpness{0.9f};
};

struct RenderSettings {
    AntialiasingMode antialiasing{AntialiasingMode::Fxaa};
    UpscalerSettings upscaler;
    ShadowQuality shadows{ShadowQuality::Single};
    LightingQuality lighting{LightingQuality::PhysicallyBased};
    BloomQuality bloom{BloomQuality::Off};
    // Energy-conserving mix weight of the bloom chain over the scene.
    float bloomIntensity{0.04f};
    // Upsample tent radius in source-mip texels.
    float bloomFilterRadius{1.0f};
    TonemapOperator tonemap{TonemapOperator::AgX};
    ExposureSettings exposure;
    EnvironmentQuality environment{EnvironmentQuality::Hemisphere};
    // Draws the image-based environment behind geometry instead of a flat clear.
    bool environmentBackground{true};
    LocalLightingQuality localLights{LocalLightingQuality::Clustered};
    // Lights uploaded per frame; the nearest are kept when a scene has more.
    uint32_t maximumLocalLights{1024};
    // Requires clustered local lighting; validation turns it Off otherwise.
    LocalShadowQuality localShadows{LocalShadowQuality::Medium};
    bool reflectionProbes{true};
    ReflectionProbeBlending reflectionProbeBlending{ReflectionProbeBlending::PerPixel};
    IndirectDiffuseMode indirectDiffuse{IndirectDiffuseMode::ProbeVolume};
    bool preferGpuDrivenSubmission{false};
    // Opaque depth and view-space normals are drawn before shading, so opaque
    // pixels shade once and screen-space effects get normals and a depth
    // pyramid. Off restores single-pass depth testing.
    bool depthPrepass{true};
    // Largest projected geometric error, in pixels at internal render
    // resolution, a simplified mesh LOD may show. Zero always draws full detail.
    float lodErrorThresholdPixels{1.0f};
    float cascadeSplitLambda{0.65f};
    float cascadeBlendFraction{0.10f};
    bool visualizeShadowCascades{false};
    DynamicResolutionSettings dynamicResolution;
    AmbientOcclusionQuality ambientOcclusion{AmbientOcclusionQuality::Off};
    ReflectionQuality reflections{ReflectionQuality::ProbesOnly};
    VolumetricFogQuality volumetricFog{VolumetricFogQuality::Off};
    MotionBlurQuality motionBlur{MotionBlurQuality::Off};
    // Exposure time as a fraction of a 60 Hz frame (a 180 degree shutter is
    // 0.5). Blur length follows on-screen speed, not the frame rate: motion
    // vectors are rescaled by the real frame time (see motion_blur::velocityScale).
    float motionBlurShutterFraction{0.5f};
    // Longest blur, in output pixels, on either side of a pixel.
    float motionBlurMaximumRadiusPixels{32.0f};
    DepthOfFieldSettings depthOfField;
    ColorLutSettings colorLut;
    VignetteSettings vignette;
    FilmGrainSettings filmGrain;
    DecalQuality decals{DecalQuality::On};
    ParticleQuality particles{ParticleQuality::Low};
    // Zero disables enforcement for that processor/pass pair.
    RenderPassBudgets budgets;

    // Block-compressed texture mip streaming. A streamed texture becomes
    // resident with its smallest levels and then gains higher-resolution levels
    // within this many bytes of mip data per frame, most visible textures
    // first. Zero uploads every texture's full chain when it is first used.
    uint32_t textureStreamingBytesPerFrame{8u * 1024u * 1024u};

    // Silent-fallback diagnostics. A mesh or texture whose upload stays
    // deferred (skipped draws or fallback texture) for more than this many
    // consecutive frames is logged once. Zero disables the warning.
    uint32_t deferredUploadWarningFrames{120};
};

struct RenderCapabilities {
    bool descriptorIndexing{false};
    bool multiDrawIndirect{false};
    bool indirectDrawCount{false};
    bool gpuDrivenPath{false};
    bool imageBasedLighting{false};
    bool reflectionProbes{false};
    bool perPixelReflectionProbes{false};
    bool irradianceVolumes{false};
    bool temporalAntialiasing{true};
    bool screenSpaceAmbientOcclusion{true};
    bool screenSpaceReflections{true};
    // Needs RGBA16F 3D storage images.
    bool volumetricFog{true};
    // Needs RGBA16F storage images and the motion-vector target.
    bool motionBlur{true};
    // Needs RGBA16F storage images.
    bool depthOfField{true};
    // FSR 1: RGBA8 storage images and a sampled, renderable output format.
    bool spatialUpscaling{false};
    bool decals{true};
    // Needs the particle compute and draw pipelines.
    bool particles{true};
    bool meshShaders{false};
    bool rayTracing{false};
    bool dedicatedTransferQueue{false};
    bool timelineTransfers{false};
    // Zero means clustered local lighting is unavailable.
    uint32_t maximumLocalLights{1024};
};

struct RenderStats {
    // CPU fields describe this submission; GPU fields identify a retired one.
    uint64_t submittedFrame{0};
    uint64_t gpuSampleFrame{0};
    bool frameSubmitted{false};
    bool gpuTimingsValid{false};
    bool uploadTimingsValid{false};
    double cpuFrameWaitMilliseconds{0.0};
    double cpuAcquireMilliseconds{0.0};
    double cpuPresentMilliseconds{0.0};
    double cpuPostMilliseconds{0.0};
    uint64_t uploadBytes{0};
    uint32_t uploadBackpressureEvents{0};
    uint32_t renderableCount{0};
    uint32_t visibleCount{0};
    uint32_t culledCount{0};
    uint32_t shadowCasterCount{0};
    uint32_t shadowCascadesRendered{0};
    uint32_t shadowCascadesCached{0};
    uint32_t drawCalls{0};
    uint64_t triangleCount{0};
    double gpuFrameMilliseconds{0.0};
    double gpuShadowMilliseconds{0.0};
    double gpuOpaqueMilliseconds{0.0};
    double gpuBlendedMilliseconds{0.0};
    double gpuSceneMilliseconds{0.0};
    double gpuPostMilliseconds{0.0};
    double gpuBloomMilliseconds{0.0};
    double gpuUiMilliseconds{0.0};
    double gpuUploadMilliseconds{0.0};
    double gpuUploadWaitMilliseconds{0.0};
    double cpuShadowMilliseconds{0.0};
    double cpuOpaqueMilliseconds{0.0};
    double cpuBlendedMilliseconds{0.0};
    double cpuBloomMilliseconds{0.0};
    double cpuUiMilliseconds{0.0};
    double cpuUploadMilliseconds{0.0};
    // Lights uploaded for clustered shading this submission.
    uint32_t localLightCount{0};
    // Cluster statistics from the retired frame (GPU-assigned): assignments
    // beyond the per-cluster capacity, and the most lights touching a cluster.
    uint32_t discardedLightAssignments{0};
    uint32_t maximumLightsPerCluster{0};
    double cpuRenderPacketMilliseconds{0.0};
    bool renderPacketsRebuilt{false};
    uint32_t scannedRenderables{0};
    uint32_t updatedObjectRecords{0};
    uint32_t mainThreadAllocations{0};
    uint32_t indirectCommandCount{0};
    uint32_t gpuVisibilityMismatches{0};
    double gpuCullingMilliseconds{0.0};
    uint32_t taskQueueDepth{0};
    uint32_t taskStolenJobs{0};
    double taskWorkerUtilization{0.0};
    double taskMainThreadAssistMilliseconds{0.0};
    double taskGraphLatencyMilliseconds{0.0};
    uint32_t renderGraphBarrierCount{0};
    uint32_t renderGraphQueueTransfers{0};
    uint64_t transientRequestedBytes{0};
    uint64_t transientCommittedBytes{0};
    uint64_t transientAliasedBytes{0};
    uint64_t transientPeakBytes{0};
    uint32_t internalRenderWidth{0};
    uint32_t internalRenderHeight{0};
    float internalResolutionScale{1.0f};
    bool temporalHistoryValid{false};
    bool temporalHistoryInvalidated{false};
    double gpuTemporalMilliseconds{0.0};
    double gpuAmbientOcclusionMilliseconds{0.0};
    double gpuReflectionsMilliseconds{0.0};
    double gpuExposureMilliseconds{0.0};
    // Zero when the retired frame skipped the pass. The prepass includes the
    // GPU-driven late cull between its phases; the pyramid is the final build
    // screen-space effects read (or the occlusion build without a prepass).
    double gpuDepthPrepassMilliseconds{0.0};
    double gpuDepthPyramidMilliseconds{0.0};
    double gpuLightClusterMilliseconds{0.0};
    // Precompute runs only on frames whose environment inputs changed.
    double gpuEnvironmentPrecomputeMilliseconds{0.0};
    double gpuSkyboxMilliseconds{0.0};
    // GPU culling of the redrawn shadow cascades; within gpuShadowMilliseconds.
    double gpuShadowCullingMilliseconds{0.0};
    // Zero when the retired frame re-rendered no local shadow tile.
    double gpuLocalShadowMilliseconds{0.0};
    // Local-light shadow atlas work of this submission: tiles drawn, tiles
    // whose cached contents were reused, tiles still waiting for the per-frame
    // update budget, and shadow-casting lights that found no atlas space.
    uint32_t localShadowTilesRendered{0};
    uint32_t localShadowTilesCached{0};
    uint32_t localShadowTilesPending{0};
    uint32_t localShadowLightsRejected{0};
    uint32_t localShadowCasterCount{0};
    double cpuLocalShadowMilliseconds{0.0};
    uint64_t localShadowAtlasBytes{0};
    // Zero when the retired frame did not blend probes per pixel.
    double gpuReflectionProbeClusterMilliseconds{0.0};
    // Probes resident for per-pixel blending this submission.
    uint32_t reflectionProbeCount{0};
    // Fog injection, temporal reprojection, and integration; zero when Off or
    // when the scene has no volumetric fog.
    double gpuVolumetricFogMilliseconds{0.0};
    // Irradiance-volume probe upload; zero on frames whose bound volume did
    // not change.
    double gpuIrradianceVolumeMilliseconds{0.0};
    // Velocity tiles plus reconstruction; zero when motion blur is Off.
    double gpuMotionBlurMilliseconds{0.0};
    // Circle of confusion, half-resolution gather, and composite; zero when Off.
    double gpuDepthOfFieldMilliseconds{0.0};
    // FSR 1 upsample and sharpen; zero unless the frame used UpscalerMode::Fsr1.
    double gpuFsrEasuMilliseconds{0.0};
    double gpuFsrRcasMilliseconds{0.0};
    // Particle emission, simulation, lighting, and sorting; zero when Off or
    // when the scene has no emitter.
    double gpuParticleSimulationMilliseconds{0.0};
    // Particle billboards; also inside gpuBlendedMilliseconds.
    double gpuParticleDrawMilliseconds{0.0};
    // Emitters simulated this submission and the particle slots they own
    // (the most particles that can be alive).
    uint32_t particleEmitterCount{0};
    uint32_t particleCapacity{0};
    // Device memory of the particle pool, sort keys, and per-frame uploads.
    uint64_t particleBytes{0};
    // Probes in the bound irradiance volume; zero when none is bound.
    uint32_t irradianceVolumeProbes{0};
    // Device memory of the irradiance-volume probe buffer.
    uint64_t irradianceVolumeBytes{0};
    // LUT grade, vignette and film grain; zero when every effect is off.
    double gpuColorGradeMilliseconds{0.0};
    // Decal cluster assignment of the retired frame; zero when it had no decal.
    double gpuDecalClusterMilliseconds{0.0};
    // Decals uploaded this submission, and scene decals left out: beyond
    // maximumDecals (the nearest are kept) or waiting for a texture upload.
    uint32_t decalCount{0};
    uint32_t droppedDecals{0};
    // Retired-frame cluster statistics (GPU-assigned): assignments beyond the
    // per-cluster capacity, and the most decals touching one cluster.
    uint32_t discardedDecalAssignments{0};
    uint32_t maximumDecalsPerCluster{0};
    // Device memory of the decal records and cluster lists of every frame.
    uint64_t decalBytes{0};
    uint64_t temporalHistoryBytes{0};
    // FSR 1 input and upscaled targets; zero until the upscaler first runs.
    uint64_t spatialUpscaleBytes{0};
    uint64_t motionVectorBytes{0};
    uint64_t ambientOcclusionBytes{0};
    uint64_t reflectionBytes{0};
    uint64_t shadowCacheBytes{0};
    // Froxel volumes owned by the volumetric fog pass.
    uint64_t volumetricFogBytes{0};
    // Blurred scene and velocity tiles; allocated on the first blurred frame.
    uint64_t motionBlurBytes{0};
    // Half-resolution gather targets and the composited output of the depth
    // of field pass; allocated on first use.
    uint64_t depthOfFieldBytes{0};
    uint64_t streamingDecodedBytes{0};
    uint64_t streamingUploadedBytes{0};
    uint64_t streamingDecodeBacklogBytes{0};
    uint64_t streamingUploadBacklogBytes{0};
    uint64_t streamingCpuPeakBytes{0};
    uint64_t streamingGpuPeakBytes{0};
    uint32_t streamingResidencyMisses{0};
    uint32_t streamingHardBudgetFailures{0};
    bool pipelineCacheWarm{false};
    uint32_t pipelineCreationEvents{0};
    double pipelineCreationMilliseconds{0.0};
    uint32_t pipelineFirstUseEvents{0};
    double pipelineFirstUseMilliseconds{0.0};

    // Silent-fallback audit. Distinct assets whose GPU upload was deferred to
    // a later frame during this submission: deferred meshes are not drawn and
    // deferred textures sample a fallback until the upload lands.
    uint32_t deferredMeshUploads{0};
    uint32_t deferredTextureUploads{0};
    // Texture mip streaming: resident and requested mip levels summed over
    // the renderer's textures, textures still below their requested level,
    // mip bytes uploaded this submission, and mip bytes still to upload.
    uint32_t textureResidentMips{0};
    uint32_t textureRequestedMips{0};
    uint32_t texturesStreaming{0};
    uint64_t textureStreamedBytes{0};
    uint64_t textureStreamingBacklogBytes{0};
    // Scene local lights left out this submission because the scene had more
    // than the effective local-light limit (the nearest are kept).
    uint32_t droppedLocalLights{0};
};

struct RenderView {
    glm::vec3 position{0.0f, 0.0f, 5.0f};
    glm::vec3 forward{0.0f, 0.0f, -1.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    float verticalFovRadians{glm::radians(60.0f)};
    float nearPlane{0.1f};
    float farPlane{1000.0f};
};

struct GridOverlay {
    float planeHeight{0.0f};
    float spacing{1.0f};
    float fadeDistance{40.0f};
};

class RenderRuntime {
  public:
    const RenderSettings& settings() const {
        return m_settings;
    }

    void setSettings(RenderSettings settings) {
        m_settings = std::move(settings);
    }

    // Directory that relative asset paths in RenderSettings (the colour-grading
    // LUT) resolve against. Empty, the default, uses the packaged asset root.
    void setAssetRoot(std::filesystem::path root) {
        m_assetRoot = std::move(root);
    }

    const std::filesystem::path& assetRoot() const {
        return m_assetRoot;
    }

    const RenderCapabilities& capabilities() const {
        return m_capabilities;
    }

    const RenderStats& stats() const {
        return m_stats;
    }

    void requestViewportSize(uint32_t width, uint32_t height) {
        m_viewportRequested = true;
        m_requestedViewportWidth = width;
        m_requestedViewportHeight = height;
    }

    void setViewportInputActive(bool active) {
        m_viewportInputActive = active;
    }

    void setViewOverride(RenderView view) {
        m_viewOverride = view;
    }

    void clearViewOverride() {
        m_viewOverride.reset();
    }

    void setGridOverlay(std::optional<GridOverlay> grid) {
        m_gridOverlay = grid;
    }

    void setOutlinedObjects(std::span<const RenderObjectId> objects) {
        m_outlinedObjects.assign(objects.begin(), objects.end());
        std::ranges::sort(m_outlinedObjects);
        m_outlinedObjects.erase(std::unique(m_outlinedObjects.begin(), m_outlinedObjects.end()),
                                m_outlinedObjects.end());
    }

    void clearOutlinedObjects() {
        m_outlinedObjects.clear();
    }

    UiTextureHandle viewportTexture() const {
        return m_viewportTexture;
    }

    uint32_t viewportWidth() const {
        return m_viewportWidth;
    }

    uint32_t viewportHeight() const {
        return m_viewportHeight;
    }

  private:
    friend class Renderer;
    friend class MeshPass;

    RenderSettings m_settings;
    std::filesystem::path m_assetRoot;
    RenderCapabilities m_capabilities;
    RenderStats m_stats;
    UiTextureHandle m_viewportTexture;
    uint32_t m_requestedViewportWidth{0};
    uint32_t m_requestedViewportHeight{0};
    uint32_t m_viewportWidth{0};
    uint32_t m_viewportHeight{0};
    bool m_viewportRequested{false};
    bool m_viewportInputActive{false};
    std::optional<RenderView> m_viewOverride;
    std::optional<GridOverlay> m_gridOverlay;
    std::vector<RenderObjectId> m_outlinedObjects;
};
