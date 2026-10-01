#pragma once

#include "../../assets/Assets.hpp"
#include "../../jobs/FrameTaskGraph.hpp"
#include "../../scene/Scene.hpp"
#include "../EnvironmentLighting.hpp"
#include "../RenderSubmission.hpp"
#include "../Rendering.hpp"
#include "../TemporalQuality.hpp"
#include "../pipeline/GraphicsPipeline.hpp"
#include "../resources/BindlessTextureTable.hpp"
#include "../resources/GeometryArena.hpp"
#include "../resources/UploadContext.hpp"
#include "../vulkan/VulkanContext.hpp"
#include "GpuDrivenPass.hpp"
#include "PostProcessPass.hpp"
#include "engine/resources/DeferredReleaseQueue.hpp"
#include <array>
#include <map>
#include <memory>
#include <optional>
#include <span>
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
        return 10u + (m_gpuDrivenSupported ? m_gpuDrivenPass.pipelineCount() : 0u);
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
        const MaterialAsset* material{nullptr};
        const GpuMesh* gpuMesh{nullptr};
        const GpuMaterial* gpuMaterial{nullptr};
        glm::mat4 world{1.0f};
        glm::mat4 previousWorld{1.0f};
        glm::vec3 boundsCenter{0.0f};
        float boundsRadius{0.0f};
        bool outlined{false};
        std::vector<glm::mat4> jointPalette;
        uint32_t skinPaletteOffset{0};
    };

    enum class SceneAttachments {
        Clear,
        // Continue a scene pass whose attachments the caller already prepared.
        Preserve,
    };

    enum class ShadedItems {
        All,
        BlendedOnly,
    };

    bool createDescriptors(const VulkanContext& context, uint32_t frameCount);
    bool createBindlessDescriptors(const VulkanContext& context, uint32_t frameCount);
    bool createGpuDrivenDescriptors();
    // Stages every region in one upload submission.
    bool uploadBufferRegions(std::span<const BufferUploadRegion> regions);
    bool createGeometryBuffer(VkDeviceSize size, VkBufferUsageFlags usage, GpuBuffer& output);
    bool createZeroSkinInfluence();
    bool ensureGeometryPage(uint32_t page);
    void destroyGeometryPage(const GeometryPage& page);
    void bindGeometryPage(VkCommandBuffer commandBuffer, uint32_t page) const;
    bool uploadTexture(const TextureAsset& texture, GpuTexture& output);
    bool uploadBlockCompressedTexture(const TextureAsset& texture, GpuTexture& output);
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

    // Quality choices compiled into mesh.frag as specialization constants, so
    // disabled lighting and shadow paths cost nothing per pixel.
    struct MeshShadingVariant {
        bool physicallyBased{true};
        uint32_t shadowCascades{0};
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
        RenderStats* stats{nullptr};
    };

    bool createPipeline(VkFormat colorFormat, VkFormat depthFormat);
    // Returns the cached pipelines for a variant, compiling them on first use.
    const MeshPipelineSet* meshPipelines(MeshShadingVariant variant);
    bool createShadowResources(VkFormat depthFormat);
    bool buildRenderItems(const Scene& scene, const AssetManager& assets,
                          const std::vector<RenderObjectId>& outlinedObjects, std::vector<RenderItem>& items);
    uint32_t updateRenderItems(const Scene& scene, std::span<const Entity> changedEntities);
    bool recordShadow(VkCommandBuffer commandBuffer, uint32_t frameIndex, const std::vector<RenderItem>& items,
                      const DirectionalLightComponent& light, const std::array<glm::mat4, 3>& lightViewProjections,
                      uint32_t cascadeCount, const std::array<bool, 3>& dirtyCascades, RenderStats& stats);
    void beginRendering(VkCommandBuffer commandBuffer, VkImageView colorView, VkImageView motionView,
                        VkImage depthImage, VkImageView depthView, VkExtent2D extent,
                        SceneAttachments attachments) const;
    bool prepareGpuDrivenFrame(uint32_t frameIndex);
    void recordGpuDrivenGroups(const DrawContext& draw, GpuCullPhase phase);
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
    // Meshes whose vertices cannot be packed; skipped until the asset unloads.
    std::unordered_set<uint64_t> m_unpackableMeshes;
    std::unordered_map<uint64_t, GpuTexture> m_textures;
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
    bool m_depthInitialized{false};
    std::map<MeshShadingVariant, std::unique_ptr<MeshPipelineSet>> m_meshPipelines;
    VkFormat m_sceneColorFormat{VK_FORMAT_UNDEFINED};
    VkFormat m_sceneDepthFormat{VK_FORMAT_UNDEFINED};
    GraphicsPipeline m_shadowPipeline;
    GraphicsPipeline m_selectionMaskPipeline;
    GraphicsPipeline m_outlinePipeline;
    PostProcessPass m_postProcess;
    GpuDrivenPass m_gpuDrivenPass;
    bool m_gpuDrivenSupported{false};
    std::vector<const RenderItem*> m_gpuDrivenCandidates;
    std::vector<IndirectDrawKey> m_gpuDrivenKeys;
    IndirectDrawPlan m_drawPlan;
    std::vector<GpuDrivenObjectRecord> m_gpuDrivenObjects;
    std::vector<GpuDrivenBatch> m_gpuDrivenBatches;
    glm::vec3 m_previousCameraPosition{0.0f};
    glm::vec3 m_previousCameraForward{0.0f, 0.0f, -1.0f};
    glm::mat4 m_previousViewProjection{1.0f};
    bool m_cameraHistoryValid{false};
    TemporalQualityController m_temporalQuality;
    EnvironmentLightingCache m_environmentCache;
    double m_uploadMilliseconds{0.0};
    double m_uploadGpuMilliseconds{0.0};
    uint64_t m_taskGeneration{0};
};
