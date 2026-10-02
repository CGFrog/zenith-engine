#pragma once

#include "../../assets/Assets.hpp"
#include "../../jobs/FrameTaskGraph.hpp"
#include "../../scene/Scene.hpp"
#include "../DeferredUploadTracker.hpp"
#include "../MeshLodSelection.hpp"
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
#include "DepthPyramid.hpp"
#include "EnvironmentPass.hpp"
#include "GpuDrivenPass.hpp"
#include "LocalShadowPass.hpp"
#include "PostProcessPass.hpp"
#include "SceneGeometryViews.hpp"
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

class MeshPass {
  public:
    bool init(const VulkanContext& context, VkFormat colorFormat, VkFormat depthFormat, uint32_t frameCount,
              VkExtent2D extent, VkImageView depthView);
    bool resize(const VulkanContext& context, VkExtent2D extent, VkImageView depthView);
    bool record(VkCommandBuffer commandBuffer, VkImage colorImage, VkImageView colorView, VkImage depthImage,
                VkImageView depthView, VkExtent2D extent, uint32_t frameIndex, const Scene& scene,
                const AssetManager& assets, RenderRuntime& rendering, FrameTaskExecutor& frameTasks,
                VkQueryPool timestampQueries, VkImageLayout colorOldLayout = VK_IMAGE_LAYOUT_UNDEFINED);
    bool uiTexture(TextureHandle handle, const AssetManager& assets, VkSampler& sampler, VkImageView& view);

    bool gpuDrivenSupported() const {
        return m_gpuDrivenSupported;
    }

    uint32_t prewarmedPipelineCount() const {
        // Thirteen graphics pipelines (two depth-normal, one local-shadow), the
        // light-cluster assignment, the depth-pyramid build, ambient occlusion,
        // and the environment pipelines.
        return 15u + m_ambientOcclusion.pipelineCount() + m_environmentPass.pipelineCount() +
               (m_gpuDrivenSupported ? m_gpuDrivenPass.pipelineCount() : 0u);
    }

    uint32_t localLightCapacity() const {
        return m_clusteredLighting.lightCapacity();
    }

    // Empty unless the latest record() for frameIndex ran the depth prepass.
    SceneGeometryViews sceneGeometry(uint32_t frameIndex) const;

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

    // See PostProcessPass::setFixedFrameSeconds.
    void setFixedFrameSeconds(std::optional<float> seconds) {
        m_postProcess.setFixedFrameSeconds(seconds);
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
        Entity entity{entt::null};
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
    // Stages every region in one upload submission.
    bool uploadBufferRegions(std::span<const BufferUploadRegion> regions);
    bool createGeometryBuffer(VkDeviceSize size, VkBufferUsageFlags usage, GpuBuffer& output);
    bool createZeroSkinInfluence();
    void writeClusteredLightingDescriptors();
    // Binds each frame slot's occlusion image to camera binding 13 of both sets.
    void writeAmbientOcclusionDescriptors();
    bool ensureGeometryPage(uint32_t page);
    void destroyGeometryPage(const GeometryPage& page);
    void bindGeometryPage(VkCommandBuffer commandBuffer, uint32_t page) const;
    bool uploadTexture(const TextureAsset& texture, GpuTexture& output);
    bool uploadBlockCompressedTexture(const TextureAsset& texture, GpuTexture& output);
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
        auto operator<=>(const MeshShadingVariant&) const = default;
    };

    struct MeshPipelineSet {
        GraphicsPipeline opaque;
        GraphicsPipeline doubleSided;
        GraphicsPipeline blend;
        GraphicsPipeline blendDoubleSided;
    };

    // Per-frame state shared by the draw-recording helpers.
    struct DrawContext {
        VkCommandBuffer commandBuffer{VK_NULL_HANDLE};
        uint32_t frameIndex{0};
        const MeshPipelineSet* pipelines{nullptr};
        VkQueryPool timestampQueries{VK_NULL_HANDLE};
        OptionalPassTimestamps* passTimestamps{nullptr};
        RenderStats* stats{nullptr};
        // World-to-view transform; the depth prepass encodes view-space normals.
        glm::mat4 view{1.0f};
        VkExtent2D renderExtent{};
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
    bool createShadowResources(VkFormat depthFormat);
    bool buildRenderItems(const Scene& scene, const AssetManager& assets,
                          const std::vector<RenderObjectId>& outlinedObjects, std::vector<RenderItem>& items);
    uint32_t updateRenderItems(const Scene& scene, std::span<const Entity> changedEntities);
    // Who selects the directional shadow casters of each dirty cascade.
    enum class ShadowCulling {
        // Frustum-tests render items on the CPU and draws them instanced.
        Cpu,
        // Culls on the GPU into GpuDrivenPass frustum views and draws the
        // indirect commands; requires this frame's GPU-driven preparation.
        Gpu,
    };

    // Updates the local-light shadow atlas and tells clustered lighting which
    // uploaded lights are shadowed. Runs before anything binds the frame's
    // camera sets (it may rebind the atlas).
    void recordLocalShadows(VkCommandBuffer commandBuffer, uint32_t frameIndex, const Scene& scene,
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
    bool recordShadow(VkCommandBuffer commandBuffer, uint32_t frameIndex, const std::vector<RenderItem>& items,
                      const DirectionalLightComponent& light, const std::array<glm::mat4, 3>& lightViewProjections,
                      uint32_t cascadeCount, const std::array<bool, 3>& dirtyCascades, ShadowCulling culling,
                      const MeshLodView& lodView, OptionalPassTimestamps& passTimestamps, RenderStats& stats);
    // Draws a GPU-culled cascade's indirect commands; called inside the
    // cascade's rendering scope with the shadow pipeline bound.
    void recordGpuShadowCascade(VkCommandBuffer commandBuffer, uint32_t frameIndex, uint32_t cascade,
                                RenderStats& stats);
    void beginRendering(VkCommandBuffer commandBuffer, VkImageView colorView, VkImageView motionView,
                        VkImage depthImage, VkImageView depthView, VkExtent2D extent,
                        SceneAttachments attachments) const;
    // specularView may be VK_NULL_HANDLE, which discards the specular output.
    void beginDepthPrepass(VkCommandBuffer commandBuffer, VkImageView normalView, VkImageView specularView,
                           VkImage depthImage, VkImageView depthView, VkExtent2D extent,
                           SceneAttachments attachments) const;
    void prepareDepthForClear(VkCommandBuffer commandBuffer, VkImage depthImage) const;
    void setSceneViewport(const DrawContext& draw) const;
    // Draws opaque depth and normals, then builds the depth pyramid from it.
    // With GPU-driven submission it also runs the late cull between phases.
    // Specular reflectance is written only when reflections trace screen
    // space. Fails only if the specular target cannot be allocated.
    bool recordDepthPrepass(const DrawContext& draw, bool gpuDrivenActive, VkImage depthImage, VkImageView depthView,
                            const glm::mat4& viewProjection, const MeshLodView& lodView, ReflectionQuality reflections);
    void recordDepthNormalItems(const DrawContext& draw);
    // Inside the scene pass, after opaque surfaces and the background: blends
    // screen-space reflections into the HDR target when they are traced,
    // ending and resuming the pass around the compute work.
    bool recordScreenSpaceReflections(const DrawContext& draw, VkImage depthImage, VkImageView depthView,
                                      const RenderSettings& settings, const ScreenSpaceReflectionCamera& camera);
    // Computes the frame's occlusion from this frame's prepass outputs, or
    // clears it to unoccluded when `quality` is Off (also used when the
    // prepass did not run). Records timestamps 10 and 11 either way.
    void recordAmbientOcclusion(const DrawContext& draw, AmbientOcclusionQuality quality, const glm::mat4& projection,
                                bool temporalNoise);
    void bindOpaquePipeline(const DrawContext& draw, const GraphicsPipeline& pipeline, OpaquePipelines pipelines,
                            VkDescriptorSet cameraSet) const;
    void selectRenderItemLods(const MeshLodView& view);
    bool prepareGpuDrivenFrame(uint32_t frameIndex, const MeshLodView& lodView);
    void recordGpuDrivenGroups(const DrawContext& draw, GpuCullPhase phase, OpaquePipelines pipelines);
    void prepareDepthForPyramid(VkCommandBuffer commandBuffer, VkImage depthImage) const;
    void restoreDepthAfterPyramid(VkCommandBuffer commandBuffer, VkImage depthImage) const;
    // Fills m_visibleItems with what the CPU submits this frame, in draw order.
    bool collectCpuSubmittedItems(bool gpuDrivenActive, const glm::vec3& cameraPosition);
    void uploadCpuSubmittedObjects(uint32_t frameIndex);
    void recordSelectionMask(const DrawContext& draw);
    void recordShadedItems(const DrawContext& draw, ShadedItems selection);
    void recordOutlines(const DrawContext& draw);

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
    GpuVisibilityResult m_visibility;
    SceneRevisions m_sceneRevisions{};
    uint64_t m_assetRevision{0};
    uint64_t m_outlineRevision{0};
    bool m_packetCachingEnabled{true};
    std::vector<const RenderItem*> m_visibleItems;
    std::vector<const RenderItem*> m_shadowVisibleItems;
    GpuTexture m_whiteTexture;
    GpuTexture m_normalTexture;
    GpuTexture m_shadowTexture;
    std::array<VkImageView, 3> m_shadowLayerViews{};
    VkSampler m_shadowSampler{VK_NULL_HANDLE};
    VkFormat m_shadowFormat{VK_FORMAT_UNDEFINED};
    bool m_shadowInitialized{false};
    SceneRevisions m_shadowRevisions{};
    std::array<glm::mat4, 3> m_cachedLightViewProjections{};
    uint32_t m_cachedCascadeCount{0};
    LocalShadowPass m_localShadows;
    SceneRevisions m_localShadowRevisions{};
    uint64_t m_localShadowAssetRevision{0};
    std::vector<ShadowCasterBounds> m_changedShadowCasters;
    // Objects written to the local-shadow region of this frame's object buffer.
    uint32_t m_localShadowObjectCount{0};
    bool m_depthInitialized{false};
    std::map<MeshShadingVariant, std::unique_ptr<MeshPipelineSet>> m_meshPipelines;
    VkFormat m_sceneColorFormat{VK_FORMAT_UNDEFINED};
    VkFormat m_sceneDepthFormat{VK_FORMAT_UNDEFINED};
    GraphicsPipeline m_shadowPipeline;
    GraphicsPipeline m_selectionMaskPipeline;
    GraphicsPipeline m_outlinePipeline;
    GraphicsPipeline m_depthNormalPipeline;
    GraphicsPipeline m_depthNormalDoubleSidedPipeline;
    PostProcessPass m_postProcess;
    DepthPyramid m_depthPyramid;
    AmbientOcclusionPass m_ambientOcclusion;
    // Frame slot whose latest record() ran the depth prepass, if any.
    std::optional<uint32_t> m_depthPrepassFrame;
    GpuDrivenPass m_gpuDrivenPass;
    ClusteredLightingPass m_clusteredLighting;
    bool m_gpuDrivenSupported{false};
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
    glm::vec3 m_previousCameraPosition{0.0f};
    glm::vec3 m_previousCameraForward{0.0f, 0.0f, -1.0f};
    glm::mat4 m_previousViewProjection{1.0f};
    glm::mat4 m_previousUnjitteredViewProjection{1.0f};
    glm::vec2 m_previousJitterNdc{0.0f};
    bool m_cameraHistoryValid{false};
    TemporalQualityController m_temporalQuality;
    EnvironmentPass m_environmentPass;
    double m_uploadMilliseconds{0.0};
    double m_uploadGpuMilliseconds{0.0};
    uint64_t m_taskGeneration{0};
};
