#pragma once

#include "../../assets/Assets.hpp"
#include "../../jobs/FrameTaskGraph.hpp"
#include "../../scene/Scene.hpp"
#include "../../scene/SceneVisibility.hpp"
#include "../DeferredUploadTracker.hpp"
#include "../MeshLodSelection.hpp"
#include "../MeshPassFramePlan.hpp"
#include "../RenderSubmission.hpp"
#include "../Rendering.hpp"
#include "../TemporalQuality.hpp"
#include "../pipeline/GraphicsPipeline.hpp"
#include "../resources/BindlessTextureTable.hpp"
#include "../resources/FrameTimestamps.hpp"
#include "../resources/GeometryArena.hpp"
#include "../resources/UploadContext.hpp"
#include "../vulkan/VulkanContext.hpp"
#include "AmbientOcclusionPass.hpp"
#include "ClusteredLightingPass.hpp"
#include "DecalClusterPass.hpp"
#include "DepthPyramid.hpp"
#include "EnvironmentPass.hpp"
#include "GpuDrivenPass.hpp"
#include "IdBufferPass.hpp"
#include "IrradianceVolumePass.hpp"
#include "LocalShadowPass.hpp"
#include "ParticlePass.hpp"
#include "PostProcessPass.hpp"
#include "ReflectionProbeClusterPass.hpp"
#include "SceneGeometryViews.hpp"
#include "TransmissionBackground.hpp"
#include "VolumetricFogPass.hpp"
#include "engine/resources/DeferredReleaseQueue.hpp"
#include <array>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// One pick region a view serves this frame (RenderRuntime::requestPick). The
// caller owns the readback, prepared for the region's size, until the frame
// completes; MeshPass fills the outputs while recording.
struct MeshPassPick {
    // Output pixels of the view, already clamped to its extent.
    PickRect region;
    const IdReadback* readback{nullptr};
    // Outputs: whether the region's copy was recorded (false when its targets
    // could not be allocated; the frame still renders), the id table (id n is
    // objects[n-1]), and the camera drawn with.
    bool recorded{false};
    std::vector<PickedObject> objects;
    glm::mat4 viewProjection{1.0f};
};

// One render view a frame records (see RenderRuntime): where it draws and what
// it shows. The targets must stay valid until the frame's submission retires.
struct MeshPassViewRequest {
    // MeshPass view slot (RenderViewId::index); slot 0 is the default view.
    uint32_t view{0};
    VkImage colorImage{VK_NULL_HANDLE};
    VkImageView colorView{VK_NULL_HANDLE};
    // Layout of colorImage before the frame; UNDEFINED discards its contents.
    VkImageLayout colorOldLayout{VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage depthImage{VK_NULL_HANDLE};
    VkImageView depthView{VK_NULL_HANDLE};
    // Output extent; equals the extent the view was created or resized with.
    VkExtent2D extent{};
    // The view's composeViewSettings result; not null.
    const RenderSettings* settings{nullptr};
    // planFrame(*settings, ...); only the default view may use GPU-driven
    // submission, so other views must clear gpuDrivenSubmission.
    MeshPassFramePlan plan;
    // Replaces the scene's primary camera when set.
    std::optional<RenderView> cameraOverride;
    std::optional<GridOverlay> grid;
    // Sorted object ids drawn with the editor outline.
    std::span<const RenderObjectId> outlinedObjects;
    // Sorted object ids the view is isolated to (RenderRuntime::
    // setViewVisibleEntities); null draws every object. An isolated view must
    // clear plan.gpuDrivenSubmission, since GPU culling draws every record.
    const std::vector<RenderObjectId>* visibleObjects{nullptr};
    // Receives the view's statistics when not null.
    RenderViewStats* stats{nullptr};
    // Pick regions to draw into the id buffer; empty skips the pass.
    std::span<MeshPassPick> picks;
};

// The camera a view's latest recording rendered with, for overlays drawn over
// its output after MeshPass::record (debug lines, label projection).
struct MeshPassViewCamera {
    // Unjittered reversed-Z view-projection; NDC spans the output extent.
    glm::mat4 viewProjection{1.0f};
    float nearPlane{0.1f};
    float farPlane{1000.0f};
    // The scene depth's valid region, from its top-left corner.
    VkExtent2D renderExtent{};
};

// Records the scene for one or more render views per frame. A frame first runs
// the work every view shares once (render packets, uploads, environment,
// irradiance volume, local and directional shadows, texture streaming), placed
// by the first (leading) view's camera, then each view's own passes into its
// targets (light, probe and decal clusters, fog, prepass, AO, shading,
// reflections, particles, post-processing). Per-view GPU state (ViewState)
// owns its camera sets, clusters, temporal and exposure history, depth
// pyramid, and post-process targets, so views of different sizes and cameras
// never disturb each other's history.
class MeshPass {
  public:
    // Creates the shared resources and the default view (slot 0).
    bool init(const VulkanContext& context, VkFormat colorFormat, VkFormat depthFormat, uint32_t frameCount,
              VkExtent2D extent, VkImageView depthView);

    // Creates view slot `view` (>= 1) for targets of `extent` whose depth is
    // `depthView`. Fails without leaving state behind.
    bool createView(const VulkanContext& context, uint32_t view, VkExtent2D extent, VkImageView depthView);
    // Resizes a view's size-dependent targets and discards its history. The
    // caller waited until no submitted frame uses the view.
    bool resizeView(const VulkanContext& context, uint32_t view, VkExtent2D extent, VkImageView depthView);
    // Releases a created view. The caller waited until no submitted frame uses
    // it; the default view lives until shutdown.
    void destroyView(const VulkanContext& context, uint32_t view);

    bool hasView(uint32_t view) const {
        return view < m_views.size() && m_views[view] != nullptr;
    }

    // The camera of the view's latest recording; empty until the view first
    // records.
    std::optional<MeshPassViewCamera> recordedCamera(uint32_t view) const {
        return hasView(view) ? m_views[view]->recordedCamera : std::nullopt;
    }

    // Decides a view's graph-shaping choices; pass the result in its request.
    MeshPassFramePlan planFrame(const RenderSettings& settings, const RenderCapabilities& capabilities) const {
        return planMeshPassFrame(settings, capabilities, m_diagnosticOverrides);
    }

    // Records the frame's shared work and then every view in `views` order;
    // views[0] leads (see the class comment) and receives the frame's GPU
    // timestamps. Every request names a distinct existing view.
    bool record(VkCommandBuffer commandBuffer, uint32_t frameIndex, const Scene& scene, const AssetManager& assets,
                RenderRuntime& rendering, std::span<const MeshPassViewRequest> views, FrameTaskExecutor& frameTasks,
                VkQueryPool timestampQueries);
    bool uiTexture(TextureHandle handle, const AssetManager& assets, VkSampler& sampler, VkImageView& view);

    bool gpuDrivenSupported() const {
        return m_gpuDrivenSupported;
    }

    // Pipelines created at startup; each created view adds its own per-view
    // pass pipelines on top (see ViewState).
    uint32_t prewarmedPipelineCount() const {
        // Thirteen graphics pipelines (two depth-normal, one local-shadow), the
        // light- and reflection-probe-cluster assignments, the depth-pyramid
        // build, ambient occlusion, volumetric fog, depth of field, particles,
        // FSR 1, and the environment pipelines.
        const ViewState& view = *m_views.front();
        return 16u + view.ambientOcclusion.pipelineCount() + m_environmentPass.pipelineCount() +
               DepthOfFieldPass::pipelineCount() + ParticlePass::pipelineCount() + IdBufferPass::pipelineCount() +
               (view.volumetricFog.supported() ? VolumetricFogPass::pipelineCount() : 0u) +
               view.postProcess.spatialUpscalePipelineCount() +
               (m_gpuDrivenSupported ? m_gpuDrivenPass.pipelineCount() : 0u);
    }

    bool volumetricFogSupported() const {
        return m_views.front()->volumetricFog.supported();
    }

    bool spatialUpscaleSupported() const {
        return m_views.front()->postProcess.spatialUpscaleSupported();
    }

    uint32_t localLightCapacity() const {
        return m_views.front()->clusteredLighting.lightCapacity();
    }

    VkSemaphore uploadTimelineSemaphore() const {
        return m_uploadContext.timelineSemaphore();
    }

    uint64_t requiredUploadTimelineValue() const {
        return m_uploadContext.requiredTimelineValue();
    }

    VkSemaphore textureUploadTimelineSemaphore() const {
        return m_textureUploadContext.timelineSemaphore();
    }

    uint64_t requiredTextureUploadTimelineValue() const {
        return m_textureUploadContext.requiredTimelineValue();
    }

    // See PostProcessPass::setFixedFrameSeconds. Applies to every view,
    // including views created later.
    void setFixedFrameSeconds(std::optional<float> seconds) {
        m_fixedFrameSeconds = seconds;
        for (const std::unique_ptr<ViewState>& view : m_views) {
            if (view) {
                view->postProcess.setFixedFrameSeconds(seconds);
            }
        }
        m_particles.setFixedFrameSeconds(seconds);
    }

    void setDiagnosticOverrides(const MeshPassDiagnosticOverrides& overrides) {
        m_diagnosticOverrides = overrides;
    }

    void shutdown(const VulkanContext& context);

  private:
    struct GpuBuffer {
        VkBuffer buffer{VK_NULL_HANDLE};
        VmaAllocation allocation{VK_NULL_HANDLE};
    };

    // A mesh lives in one shared geometry page; draws address it by offset.
    struct GpuMesh {
        GeometryAllocation geometry;
    };

    // One buffer per packed vertex stream, all indexed by the same vertex
    // offset. Static pages have no skin stream and bind m_zeroSkinInfluence.
    struct GeometryPage {
        GeometryLayout layout{GeometryLayout::Static};
        GpuBuffer positions;
        GpuBuffer attributes;
        GpuBuffer skin;
        GpuBuffer indices;
    };

    struct BufferUploadRegion {
        const void* source{nullptr};
        VkDeviceSize size{0};
        VkBuffer destination{VK_NULL_HANDLE};
        VkDeviceSize destinationOffset{0};
    };

    struct GpuTexture {
        VkImage image{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
        VkImageView srgbView{VK_NULL_HANDLE};
        VmaAllocation allocation{VK_NULL_HANDLE};
        // Bindless array slots for the linear and sRGB views.
        uint32_t linearSlot{0};
        uint32_t srgbSlot{0};
        // Levels [residentFirstMip, mipCount) hold texels and both views cover
        // exactly them. Block-compressed textures start at their mip tail and
        // stream toward level 0; other textures are complete when created.
        uint32_t mipCount{1};
        uint32_t residentFirstMip{0};
    };

    // Levels [firstMip, firstMip + levelCount) of a block-compressed texture.
    struct TextureMipRangeUpload {
        const TextureAsset* texture{nullptr};
        GpuTexture* gpu{nullptr};
        uint32_t firstMip{0};
        uint32_t levelCount{0};
    };

    // Index of the material's record in the per-frame material buffer.
    struct GpuMaterial {
        uint32_t index{0};
    };

    struct CameraBuffer {
        GpuBuffer buffer;
        void* mapped{nullptr};
        VkDescriptorSet descriptor{VK_NULL_HANDLE};
        GpuBuffer objects;
        void* objectsMapped{nullptr};
        GpuBuffer skinPalettes;
        void* skinPalettesMapped{nullptr};
        GpuBuffer previousSkinPalettes;
        void* previousSkinPalettesMapped{nullptr};
        GpuBuffer materials;
        void* materialsMapped{nullptr};
        // Each frame in flight owns a bindless set, rewritten only after the
        // frame's previous submission retired.
        VkDescriptorSet bindlessDescriptor{VK_NULL_HANDLE};
        // Same bindings, but objects come from the GPU culling output.
        VkDescriptorSet gpuDrivenDescriptor{VK_NULL_HANDLE};
    };

    struct RenderItem {
        MeshHandle meshHandle;
        // The entity whose mesh, material, and skin are drawn.
        Entity entity{entt::null};
        // For a draw produced by a collection instance, the instancing entity
        // (see expandCollectionInstances); null otherwise. Outlines, object
        // ids, and view isolation follow renderObject().
        Entity instancer{entt::null};
        // One-based position of the instance draw in the frame's expansion,
        // mixed into recordId so repeated draws of one member stay distinct;
        // zero for the scene's own items.
        uint32_t instanceOrdinal{0};

        Entity renderObject() const {
            return instancer == entt::null ? entity : instancer;
        }

        uint64_t recordId{0};
        GpuSceneHandle gpuSceneHandle;
        MaterialHandle materialHandle;
        const MeshAsset* mesh{nullptr};
        const MeshPrimitive* primitive{nullptr};
        // Index range chosen for this frame's camera; shadow cascades reuse it.
        MeshLod lod{};
        const MaterialAsset* material{nullptr};
        const GpuMesh* gpuMesh{nullptr};
        const GpuMaterial* gpuMaterial{nullptr};
        glm::mat4 world{1.0f};
        glm::mat4 previousWorld{1.0f};
        glm::vec3 boundsCenter{0.0f};
        float boundsRadius{0.0f};
        bool outlined{false};
        std::vector<glm::mat4> jointPalette;
        // The palette the previous frame rendered with; empty until one was.
        std::vector<glm::mat4> previousJointPalette;
        uint32_t skinPaletteOffset{0};
    };

    // Camera set of a created view for one frame slot. The default view uses
    // the frame's sets (m_cameras) instead, whose object buffer also holds the
    // shadow regions; a created view's object buffer holds only its own
    // CPU-submitted objects, from index 0.
    struct ViewCameraSet {
        GpuBuffer uniform;
        void* uniformMapped{nullptr};
        GpuBuffer objects;
        void* objectsMapped{nullptr};
        VkDescriptorSet descriptor{VK_NULL_HANDLE};
    };

    // GPU state and history of one render view. Bindings that every view
    // shares (shadow map, skin palettes, materials, environment, irradiance
    // volume, local shadows) are copied from the frame's set of the same slot
    // before a created view records; the rest point at the view's own passes.
    // Owns its passes' pipelines, so creating a view also compiles those
    // (through the pipeline cache).
    struct ViewState {
        bool sharesFrameSets{false};
        VkDescriptorPool descriptorPool{VK_NULL_HANDLE};
        std::vector<ViewCameraSet> cameras;
        ClusteredLightingPass clusteredLighting;
        ReflectionProbeClusterPass reflectionProbeClusters;
        DecalClusterPass decalClusters;
        VolumetricFogPass volumetricFog;
        // Fog volume generation each frame slot's camera sets reference.
        std::vector<uint64_t> fogDescriptorGenerations;
        PostProcessPass postProcess;
        DepthPyramid depthPyramid;
        AmbientOcclusionPass ambientOcclusion;
        TransmissionBackground transmissionBackground;
        // Frame slot whose latest recording ran the depth prepass, if any.
        std::optional<uint32_t> depthPrepassFrame;
        bool depthInitialized{false};
        GpuVisibilityResult visibility;
        // What the CPU submits this frame, in draw order, and whether each
        // entry is outlined in this view.
        std::vector<const RenderItem*> visibleItems;
        std::vector<uint8_t> visibleOutlined;
        glm::vec3 previousCameraPosition{0.0f};
        glm::vec3 previousCameraForward{0.0f, 0.0f, -1.0f};
        glm::mat4 previousViewProjection{1.0f};
        glm::mat4 previousUnjitteredViewProjection{1.0f};
        glm::vec2 previousJitterNdc{0.0f};
        bool cameraHistoryValid{false};
        TemporalQualityController temporalQuality;
        std::optional<MeshPassViewCamera> recordedCamera;
    };

    // The camera and sampling of one view for the frame being recorded.
    struct ViewFrame {
        glm::mat4 view{1.0f};
        // Reversed-Z projection at the internal render extent, jittered with TAA.
        glm::mat4 projection{1.0f};
        glm::mat4 unjitteredViewProjection{1.0f};
        glm::vec3 cameraPosition{0.0f};
        glm::vec3 cameraForward{0.0f, 0.0f, -1.0f};
        float verticalFov{0.0f};
        float nearPlane{0.1f};
        float farPlane{1000.0f};
        bool cameraCut{false};
        TemporalFrameState temporal;
        VkExtent2D renderExtent{};
        MeshLodView lodView;
    };

    // Shared results of the frame stage that every view reads; defined in
    // MeshPass.cpp.
    struct FrameState;

    enum class SceneAttachments {
        Clear,
        // Continue a scene pass whose attachments the caller already prepared.
        Preserve,
        // Clear the color targets but keep the depth prepass result, which the
        // caller left in DEPTH_ATTACHMENT_OPTIMAL.
        ClearColorKeepDepth,
    };

    enum class ShadedItems {
        OpaqueOnly,
        BlendedOnly,
    };

    bool createDescriptors(const VulkanContext& context, uint32_t frameCount);
    bool createBindlessDescriptors(const VulkanContext& context, uint32_t frameCount);
    bool createGpuDrivenDescriptors();
    // Initializes a view's passes and writes their camera-set bindings.
    bool initViewPasses(const VulkanContext& context, ViewState& view, VkExtent2D extent, VkImageView depthView);
    // A created view's per-slot uniform and object buffers and camera sets.
    bool createViewCameraSets(ViewState& view);
    void destroyViewState(const VulkanContext& context, ViewState& view);
    // The view's camera set for a frame slot, and its GPU-driven set (null
    // except for the default view with GPU-driven support).
    VkDescriptorSet viewCameraSet(const ViewState& view, uint32_t frameIndex) const;
    std::array<VkDescriptorSet, 2> viewCameraSets(const ViewState& view, uint32_t frameIndex) const;
    // Copies the bindings every view shares from the frame's set of the slot
    // into a created view's set. The slot's previous submission retired.
    void copySharedCameraBindings(const ViewState& view, uint32_t frameIndex);
    // Stages every region in one upload submission.
    bool uploadBufferRegions(std::span<const BufferUploadRegion> regions);
    bool createGeometryBuffer(VkDeviceSize size, VkBufferUsageFlags usage, GpuBuffer& output);
    bool createZeroSkinInfluence();
    void writeClusteredLightingDescriptors(const ViewState& view);
    // Binds each frame slot's probe buffer to camera binding 17.
    void writeReflectionProbeDescriptors(const ViewState& view);
    void writeDecalDescriptors(const ViewState& view);
    // Decals to apply this frame, packed for upload; empty when decals are off.
    std::vector<GpuDecal> prepareDecals(const Scene& scene, const AssetManager& assets, const RenderSettings& settings,
                                        const glm::vec3& cameraPosition, const glm::mat4& view,
                                        uint32_t& sceneDecalCount);
    // Binds each frame slot's occlusion image to camera binding 13 of both sets.
    void writeAmbientOcclusionDescriptors(const ViewState& view);
    // Binds the fog pass's integrated volume to camera binding 18 of the frame
    // slot's sets when the volume changed since they were last written.
    void writeVolumetricFogDescriptors(ViewState& view, uint32_t frameIndex);
    // Binds the transmission background to camera binding 24 of every set.
    void writeTransmissionBackgroundDescriptors(const ViewState& view);
    bool ensureGeometryPage(uint32_t page);
    void destroyGeometryPage(const GeometryPage& page);
    void bindGeometryPage(VkCommandBuffer commandBuffer, uint32_t page) const;
    bool uploadTexture(const TextureAsset& texture, GpuTexture& output);
    bool uploadBlockCompressedTexture(const TextureAsset& texture, GpuTexture& output, uint32_t firstMip);
    // Stages every range in one texture-upload submission and moves the
    // uploaded levels to SHADER_READ_ONLY_OPTIMAL; views are left unchanged.
    // False on staging backpressure or failure, with nothing published.
    bool uploadTextureMipRanges(std::span<const TextureMipRangeUpload> ranges);
    // Linear and (when srgbFormat is defined) sRGB views of levels
    // [firstMip, mipCount). On failure no view is left behind.
    bool createTextureViews(VkImage image, VkFormat linearFormat, VkFormat srgbFormat, uint32_t firstMip,
                            uint32_t mipCount, VkImageView& view, VkImageView& srgbView);
    // Points the texture's views and bindless slots at levels [firstMip,
    // mipCount) after they were uploaded; the old views retire with the frame.
    bool publishResidentMips(const TextureAsset& texture, GpuTexture& gpu, uint32_t firstMip);
    // Uploads this frame's budget of higher-resolution levels, most visible
    // textures first (see planTextureMipUploads).
    void streamTextureMips(const AssetManager& assets, const glm::vec3& cameraPosition);

    // Streaming needs non-blocking uploads: without a timeline semaphore every
    // texture-upload submission waits for the device, so a frame-by-frame
    // trickle would stall each frame and textures upload whole instead.
    bool textureStreamingActive() const {
        return m_textureStreamingBytesPerFrame != 0 && m_textureUploadContext.timelineSemaphore() != VK_NULL_HANDLE;
    }

    // Makes every level of an uploaded texture resident now.
    bool completeTextureMips(uint64_t key, const AssetManager& assets);
    void recordTextureStreaming(const AssetManager& assets, RenderStats& stats) const;
    // Why the texture can never upload on this device; empty when it can.
    std::string textureRejectionReason(const TextureAsset& texture) const;
    bool assignTextureSlots(GpuTexture& texture);
    void retireTexture(const GpuTexture& texture);
    void destroyTexture(const GpuTexture& texture);
    void writeMaterialRecords(uint32_t frameIndex, const AssetManager& assets);
    void updateBindlessDescriptor(uint32_t frameIndex);
    void bindGeometry(VkCommandBuffer commandBuffer, const GpuMesh& mesh, std::optional<uint32_t>& boundPage) const;
    const GpuMesh* ensureMesh(MeshHandle handle, const AssetManager& assets);
    const GpuTexture* ensureTexture(TextureHandle handle, const AssetManager& assets);
    const GpuMaterial* ensureMaterial(MaterialHandle handle, const AssetManager& assets);
    void pruneUnusedAssets(const AssetManager& assets);
    // Closes the frame's deferred-upload bookkeeping: fills the stats counters
    // and logs assets that just stayed deferred past the configured bound.
    void recordDeferredUploads(const RenderSettings& settings, RenderStats& stats);

    // Quality choices compiled into mesh.frag as specialization constants, so
    // disabled lighting and shadow paths cost nothing per pixel.
    struct MeshShadingVariant {
        bool physicallyBased{true};
        uint32_t shadowCascades{0};
        // Opaque pipelines test EQUAL against prepass depth without writing it.
        bool depthPrepass{false};
        // View shading (ViewShading.hpp): untextured clay surfaces, and the
        // polygon-mode LINE overlay pipelines for Wireframe.
        bool solidSurfaces{false};
        bool wireframeOverlay{false};
        auto operator<=>(const MeshShadingVariant&) const = default;
    };

    struct MeshPipelineSet {
        GraphicsPipeline opaque;
        GraphicsPipeline doubleSided;
        GraphicsPipeline blend;
        GraphicsPipeline blendDoubleSided;
        // Created only for wireframeOverlay variants.
        GraphicsPipeline wireframe;
        GraphicsPipeline wireframeDoubleSided;

        bool hasWireframeOverlay() const {
            return wireframe.handle() != VK_NULL_HANDLE;
        }

        void shutdown(VkDevice device);
    };

    // Per-view state shared by the draw-recording helpers.
    struct DrawContext {
        VkCommandBuffer commandBuffer{VK_NULL_HANDLE};
        uint32_t frameIndex{0};
        const MeshPipelineSet* pipelines{nullptr};
        // Null for every view but the leading one, which writes the frame's
        // timestamps.
        VkQueryPool timestampQueries{VK_NULL_HANDLE};
        OptionalPassTimestamps* passTimestamps{nullptr};
        RenderStats* stats{nullptr};
        // World-to-view transform; the depth prepass encodes view-space normals.
        glm::mat4 view{1.0f};
        VkExtent2D renderExtent{};
        // The view being recorded, its camera set for the frame slot, and the
        // index of its first CPU-submitted object in that set's object buffer.
        ViewState* viewState{nullptr};
        VkDescriptorSet cameraSet{VK_NULL_HANDLE};
        uint32_t objectBase{0};
    };

    // Single- and double-sided pipelines for opaque surfaces of one pass.
    struct OpaquePipelines {
        const GraphicsPipeline* singleSided{nullptr};
        const GraphicsPipeline* doubleSided{nullptr};
        // Depth-normal pipelines take DrawContext::view as a push constant.
        bool pushesView{false};
    };

    bool createPipeline(VkFormat colorFormat, VkFormat depthFormat);
    // Returns the cached pipelines for a variant, compiling them on first use.
    const MeshPipelineSet* meshPipelines(MeshShadingVariant variant);
    bool createWireframeOverlay(MeshPipelineSet& pipelines, VkShaderModule meshVertex);
    bool createShadowResources(VkFormat depthFormat);
    // Builds the items of every entity m_sceneVisibilityMode draws, plus the
    // draws collection instances expand to.
    bool buildRenderItems(const Scene& scene, const AssetManager& assets,
                          const std::vector<RenderObjectId>& outlinedObjects, std::vector<RenderItem>& items);
    // Appends one item per primitive of entity's mesh drawn at world. Returns
    // false when an asset is invalid; a mesh still uploading is skipped.
    bool appendRenderItems(const Scene& scene, const AssetManager& assets,
                           const std::vector<RenderObjectId>& outlinedObjects, Entity entity, Entity instancer,
                           const glm::mat4& world, std::vector<RenderItem>& items);
    uint32_t updateRenderItems(const Scene& scene, std::span<const Entity> changedEntities);
    // Who selects the directional shadow casters of each dirty cascade.
    enum class ShadowCulling {
        // Frustum-tests render items on the CPU and draws them instanced.
        Cpu,
        // Culls on the GPU into GpuDrivenPass frustum views and draws the
        // indirect commands; requires this frame's GPU-driven preparation.
        Gpu,
    };

    // Updates the local-light shadow atlas and returns each light's first
    // shadow tile (negative when unshadowed), for clustered lighting. Runs
    // before anything binds the frame's camera sets (it may rebind the atlas).
    std::vector<int32_t> recordLocalShadows(VkCommandBuffer commandBuffer, uint32_t frameIndex, const Scene& scene,
                                            const AssetManager& assets, const RenderSettings& settings,
                                            std::span<const LocalLight> lights, const glm::vec3& cameraPosition,
                                            OptionalPassTimestamps& timestamps, RenderStats& stats);
    // Bounds of casters that moved or animated since the last local-shadow
    // update, before and after the change.
    void collectShadowCasterChanges(const Scene& scene);
    // Draws one atlas tile's casters from the local-shadow object region.
    // Returns false when that region is full.
    bool drawLocalShadowCasters(VkCommandBuffer commandBuffer, uint32_t frameIndex, const glm::mat4& viewProjection,
                                RenderStats& stats);
    // Clears and redraws the dirty cascades; cached cascades keep their depth.
    // casterObjects, when not null, limits CPU-culled casters to those sorted
    // objects (every view of the frame isolated); GPU culling never runs then.
    bool recordShadow(VkCommandBuffer commandBuffer, uint32_t frameIndex, const std::vector<RenderItem>& items,
                      const DirectionalLightComponent& light, const std::array<glm::mat4, 3>& lightViewProjections,
                      uint32_t cascadeCount, const std::array<bool, 3>& dirtyCascades, ShadowCulling culling,
                      const std::vector<RenderObjectId>* casterObjects, const MeshLodView& lodView,
                      OptionalPassTimestamps& passTimestamps, RenderStats& stats);
    // Draws a GPU-culled cascade's indirect commands; called inside the
    // cascade's rendering scope with the shadow pipeline bound.
    void recordGpuShadowCascade(VkCommandBuffer commandBuffer, uint32_t frameIndex, uint32_t cascade,
                                RenderStats& stats);
    // depthInitialized: whether the view's depth holds an earlier frame's
    // depth in DEPTH_READ_ONLY_OPTIMAL; read only when attachments is Clear.
    void beginRendering(VkCommandBuffer commandBuffer, VkImageView colorView, VkImageView motionView,
                        VkImage depthImage, VkImageView depthView, VkExtent2D extent, SceneAttachments attachments,
                        bool depthInitialized) const;
    // specularView may be VK_NULL_HANDLE, which discards the specular output.
    void beginDepthPrepass(VkCommandBuffer commandBuffer, VkImageView normalView, VkImageView specularView,
                           VkImage depthImage, VkImageView depthView, VkExtent2D extent, SceneAttachments attachments,
                           bool depthInitialized) const;
    void prepareDepthForClear(VkCommandBuffer commandBuffer, VkImage depthImage, bool depthInitialized) const;
    void setSceneViewport(const DrawContext& draw) const;
    // Draws opaque depth and normals, then builds the depth pyramid from it.
    // With GPU-driven submission it also runs the late cull between phases.
    // Specular reflectance is always written (see the definition for why).
    // Fails only if the specular target cannot be allocated.
    bool recordDepthPrepass(const DrawContext& draw, bool gpuDrivenActive, VkImage depthImage, VkImageView depthView,
                            const glm::mat4& viewProjection, const MeshLodView& lodView);
    void recordDepthNormalItems(const DrawContext& draw);
    // Inside the scene pass, after opaque surfaces and the background: blends
    // screen-space reflections into the HDR target when they are traced,
    // ending and resuming the pass around the compute work.
    bool recordScreenSpaceReflections(const DrawContext& draw, VkImage depthImage, VkImageView depthView,
                                      ReflectionQuality reflections, const ScreenSpaceReflectionCamera& camera);
    // Computes the frame's occlusion from this frame's prepass outputs, or
    // clears it to unoccluded when `quality` is Off (also used when the
    // prepass did not run). Records timestamps 10 and 11 either way.
    void recordAmbientOcclusion(const DrawContext& draw, AmbientOcclusionQuality quality, const glm::mat4& projection,
                                bool temporalNoise);
    // Records this frame's fog (or clears it) between light clustering and the
    // depth prepass, with its timestamp scope.
    void recordVolumetricFog(const DrawContext& draw, const VolumetricFogFrame* frame);
    // Moves the directional shadow map to DEPTH_READ_ONLY_OPTIMAL before any
    // cascade was rendered, so compute passes may bind it.
    void prepareShadowMapForSampling(VkCommandBuffer commandBuffer);
    // Records the frame's particle emission, simulation, and sorting with its
    // timestamp scope; the emitters' sprites are resolved into
    // m_particleSpriteViews for the draw.
    bool recordParticleSimulation(const DrawContext& draw, const Scene& scene, const AssetManager& assets,
                                  ParticleQuality quality, const ParticleSimulationFrame& frame);
    // Inside the scene pass after blended surfaces: draws this frame's
    // particles in their own rendering scope, then resumes the scene pass.
    void recordParticles(const DrawContext& draw, VkImage depthImage, VkImageView depthView);
    void bindOpaquePipeline(const DrawContext& draw, const GraphicsPipeline& pipeline, OpaquePipelines pipelines,
                            VkDescriptorSet cameraSet) const;
    void selectRenderItemLods(const MeshLodView& view);
    // visibleInstanceCount: the default view's CPU-visible instances.
    bool prepareGpuDrivenFrame(uint32_t frameIndex, const MeshLodView& lodView, uint32_t visibleInstanceCount);
    void recordGpuDrivenGroups(const DrawContext& draw, GpuCullPhase phase, OpaquePipelines pipelines);
    void prepareDepthForPyramid(VkCommandBuffer commandBuffer, VkImage depthImage) const;
    void restoreDepthAfterPyramid(VkCommandBuffer commandBuffer, VkImage depthImage) const;
    // Fills view.visibleItems with what the CPU submits this frame, in draw
    // order, and marks the entries outlined in this view. visibleObjects, when
    // not null, isolates the view to those sorted objects.
    bool collectCpuSubmittedItems(ViewState& view, bool gpuDrivenActive, const glm::vec3& cameraPosition,
                                  std::span<const RenderObjectId> outlinedObjects,
                                  const std::vector<RenderObjectId>* visibleObjects);
    void uploadCpuSubmittedObjects(const DrawContext& draw);
    // Empty unless the view's latest recording for frameIndex ran the depth
    // prepass.
    SceneGeometryViews sceneGeometry(const ViewState& view, uint32_t frameIndex) const;
    // The view's camera and sampling for this frame (advances its temporal
    // controller, so call once per view and frame).
    ViewFrame prepareViewFrame(ViewState& view, const MeshPassViewRequest& request, const Scene& scene,
                               double previousGpuFrameMilliseconds) const;
    // Shared frame stage, placed by the leading view.
    bool recordFrameStage(VkCommandBuffer commandBuffer, uint32_t frameIndex, const Scene& scene,
                          const AssetManager& assets, RenderRuntime& rendering, const MeshPassViewRequest& lead,
                          const ViewFrame& leadFrame, FrameTaskExecutor& frameTasks, VkQueryPool timestampQueries,
                          OptionalPassTimestamps& passTimestamps, FrameState& frame);
    // One view's passes into its targets. `leading` marks views[0], the only
    // view given the timestamp pool.
    bool recordView(VkCommandBuffer commandBuffer, uint32_t frameIndex, const Scene& scene, const AssetManager& assets,
                    RenderRuntime& rendering, const MeshPassViewRequest& request, const ViewFrame& viewFrame,
                    bool leading, FrameTaskExecutor& frameTasks, VkQueryPool timestampQueries,
                    OptionalPassTimestamps& passTimestamps, FrameState& frame);
    // Draws the view's frustum-visible objects into the id buffer once per
    // pick region in request.picks and records each region's readback copy.
    bool recordPicks(const DrawContext& draw, const Scene& scene, const MeshPassViewRequest& request,
                     const ViewFrame& viewFrame);
    void recordSelectionMask(const DrawContext& draw);
    void recordShadedItems(const DrawContext& draw, ShadedItems selection);
    void recordOutlines(const DrawContext& draw);
    // Wireframe view shading: draws the edges of every surface drawn this
    // frame (GPU-driven groups and CPU-submitted items) over them, inside the
    // scene pass. Does nothing unless draw.pipelines has the overlay.
    void recordWireframeOverlay(const DrawContext& draw, bool gpuDrivenActive);
    // Inside the scene pass, after opaque surfaces, the background and
    // screen-space reflections: when a transmissive surface will draw, ends
    // the pass, copies the scene into the transmission background and resumes.
    void recordTransmissionBackground(const DrawContext& draw, VkImage depthImage, VkImageView depthView);

    VkDevice m_device{VK_NULL_HANDLE};
    VkPipelineCache m_pipelineCache{VK_NULL_HANDLE};
    VmaAllocator m_allocator{VK_NULL_HANDLE};
    UploadContext m_uploadContext;
    UploadContext m_textureUploadContext;
    VkDescriptorSetLayout m_cameraLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_bindlessLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorPool m_bindlessPool{VK_NULL_HANDLE};
    VkSampler m_sampler{VK_NULL_HANDLE};
    bool m_blockCompressionSupported{false};
    std::vector<CameraBuffer> m_cameras;
    std::unique_ptr<BindlessTextureTable> m_textureTable;
    uint32_t m_whiteSlot{0};
    uint32_t m_whiteSrgbSlot{0};
    uint32_t m_normalSlot{0};
    std::unique_ptr<GeometryArena> m_geometry;
    std::vector<GeometryPage> m_geometryPages;
    // A single zero skin influence, bound with stride 0 for static pages.
    GpuBuffer m_zeroSkinInfluence;
    std::unordered_map<uint64_t, GpuMesh> m_meshes;
    // Meshes whose vertices cannot be packed or that exceed the staging limit;
    // skipped until the asset unloads.
    std::unordered_set<uint64_t> m_unpackableMeshes;
    std::unordered_map<uint64_t, GpuTexture> m_textures;
    // Textures this device can never sample (unsupported or invalid encoding);
    // skipped with one log line until the asset unloads.
    std::unordered_set<uint64_t> m_rejectedTextures;
    // Mesh and texture uploads retried on later frames, with one warning per
    // asset that stays deferred past RenderSettings::deferredUploadWarningFrames.
    DeferredUploadTracker m_deferredUploads;
    // Set when the last packet build skipped a mesh whose upload was deferred,
    // so packets are rebuilt (and the upload retried) until every mesh lands.
    bool m_meshUploadsPending{false};
    // Capacity failures retried every frame are logged once per episode.
    bool m_geometryArenaFullReported{false};
    LogChannel m_log;
    bool m_textureTableFullReported{false};
    std::unordered_map<uint64_t, GpuMaterial> m_materials;
    std::vector<uint32_t> m_freeMaterialIndices;
    uint32_t m_nextMaterialIndex{0};
    // Native resources a frame in flight may still reference, released once
    // every frame recorded before their retirement has completed.
    DeferredReleaseQueue m_releaseQueue;
    uint64_t m_frameSerial{0};
    std::vector<RenderItem> m_renderItems;
    std::unordered_map<RenderObjectId, std::vector<size_t>> m_objectItems;
    std::unordered_map<uint64_t, GpuSceneHandle> m_gpuRecords;
    std::unordered_map<uint64_t, size_t> m_gpuHandleItems;
    PersistentGpuScene m_gpuScene;
    SceneRevisions m_sceneRevisions{};
    uint64_t m_assetRevision{0};
    uint64_t m_outlineRevision{0};
    bool m_packetCachingEnabled{true};
    // Union of every recorded view's outlined objects; RenderItem::outlined
    // marks it, and each view filters by its own list.
    std::vector<RenderObjectId> m_outlinedObjects;
    // RenderRuntime::sceneVisibilityMode for the frame being recorded, and the
    // mode the current render items were built with.
    SceneVisibilityMode m_sceneVisibilityMode{SceneVisibilityMode::Runtime};
    SceneVisibilityMode m_builtVisibilityMode{SceneVisibilityMode::Runtime};
    // Set when the render items include collection-instance draws. Their
    // worlds compose several entities' transforms, so any transform or skin
    // change rebuilds the items instead of updating them in place.
    bool m_hasCollectionInstances{false};
    std::vector<CollectionInstanceDraw> m_collectionInstanceDraws;
    std::vector<const RenderItem*> m_shadowVisibleItems;
    GpuTexture m_whiteTexture;
    GpuTexture m_normalTexture;
    GpuTexture m_shadowTexture;
    std::array<VkImageView, 3> m_shadowLayerViews{};
    VkSampler m_shadowSampler{VK_NULL_HANDLE};
    VkFormat m_shadowFormat{VK_FORMAT_UNDEFINED};
    bool m_shadowInitialized{false};
    SceneRevisions m_shadowRevisions{};
    // This frame's directional shadow casters: nullopt for every object, or
    // the union of the views' isolation sets when every view is isolated.
    std::optional<std::vector<RenderObjectId>> m_frameShadowCasters;
    // What the cached cascades were drawn for besides the scene revisions: the
    // visibility mode and the caster limit.
    SceneVisibilityMode m_shadowVisibilityMode{SceneVisibilityMode::Runtime};
    std::optional<std::vector<RenderObjectId>> m_shadowCasterObjects;
    std::array<glm::mat4, 3> m_cachedLightViewProjections{};
    uint32_t m_cachedCascadeCount{0};
    LocalShadowPass m_localShadows;
    SceneRevisions m_localShadowRevisions{};
    uint64_t m_localShadowAssetRevision{0};
    std::vector<ShadowCasterBounds> m_changedShadowCasters;
    // Objects written to the local-shadow region of this frame's object buffer.
    uint32_t m_localShadowObjectCount{0};
    std::map<MeshShadingVariant, std::unique_ptr<MeshPipelineSet>> m_meshPipelines;
    VkFormat m_sceneColorFormat{VK_FORMAT_UNDEFINED};
    VkFormat m_sceneDepthFormat{VK_FORMAT_UNDEFINED};
    GraphicsPipeline m_shadowPipeline;
    GraphicsPipeline m_selectionMaskPipeline;
    GraphicsPipeline m_outlinePipeline;
    GraphicsPipeline m_depthNormalPipeline;
    GraphicsPipeline m_depthNormalDoubleSidedPipeline;
    // Indexed by view slot; slot 0 (the default view) exists from init to
    // shutdown, other slots between createView and destroyView.
    std::vector<std::unique_ptr<ViewState>> m_views;
    VkFormat m_outputFormat{VK_FORMAT_UNDEFINED};
    std::optional<float> m_fixedFrameSeconds;
    // GPU-driven submission and its occlusion history belong to the default
    // view and bind its depth pyramid.
    GpuDrivenPass m_gpuDrivenPass;
    ParticlePass m_particles;
    IdBufferPass m_idBuffer;
    std::array<VkImageView, particles::maximumSpriteTextures> m_particleSpriteViews{};
    bool m_gpuDrivenSupported{false};
    MeshPassDiagnosticOverrides m_diagnosticOverrides;
    std::vector<const RenderItem*> m_gpuDrivenCandidates;
    std::vector<IndirectDrawKey> m_gpuDrivenKeys;

    // Each candidate contributes one key per LOD level, contiguously.
    struct GpuDrivenKeySource {
        uint32_t candidate{0};
        uint32_t level{0};
    };

    std::vector<GpuDrivenKeySource> m_gpuDrivenKeySources;
    std::vector<uint32_t> m_gpuDrivenKeyBatches;
    IndirectDrawPlan m_drawPlan;
    std::vector<GpuDrivenObjectRecord> m_gpuDrivenObjects;
    std::vector<GpuDrivenBatch> m_gpuDrivenBatches;
    EnvironmentPass m_environmentPass;
    // Irradiance volume (U6): probe buffer behind camera binding 8.
    IrradianceVolumePass m_irradianceVolume;
    double m_uploadMilliseconds{0.0};
    double m_uploadGpuMilliseconds{0.0};
    // RenderSettings::textureStreamingBytesPerFrame of the frame being recorded.
    uint32_t m_textureStreamingBytesPerFrame{RenderSettings{}.textureStreamingBytesPerFrame};
    uint64_t m_textureStreamedBytes{0};
    // Reused CPU staging for batched mip uploads.
    std::vector<std::byte> m_mipStagingBytes;
    uint64_t m_taskGeneration{0};
};
