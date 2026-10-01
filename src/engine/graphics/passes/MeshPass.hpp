#pragma once

#include "../../assets/Assets.hpp"
#include "../../jobs/FrameTaskGraph.hpp"
#include "../../scene/Scene.hpp"
#include "../EnvironmentLighting.hpp"
#include "../RenderSubmission.hpp"
#include "../Rendering.hpp"
#include "../TemporalQuality.hpp"
#include "../pipeline/GraphicsPipeline.hpp"
#include "../resources/UploadContext.hpp"
#include "../vulkan/VulkanContext.hpp"
#include "GpuDrivenPass.hpp"
#include "PostProcessPass.hpp"
#include <array>
#include <map>
#include <memory>
#include <unordered_map>
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
        return 10u + (m_gpuDrivenSupported ? 2u : 0u);
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

    struct GpuMesh {
        GpuBuffer vertices;
        GpuBuffer indices;
    };

    struct GpuTexture {
        VkImage image{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
        VkImageView srgbView{VK_NULL_HANDLE};
        VmaAllocation allocation{VK_NULL_HANDLE};
    };

    struct GpuMaterial {
        VkDescriptorSet descriptor{VK_NULL_HANDLE};
    };

    struct CameraBuffer {
        GpuBuffer buffer;
        void* mapped{nullptr};
        VkDescriptorSet descriptor{VK_NULL_HANDLE};
        GpuBuffer objects;
        void* objectsMapped{nullptr};
        GpuBuffer skinPalettes;
        void* skinPalettesMapped{nullptr};
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
        bool newlySpawned{true};
        std::vector<glm::mat4> jointPalette;
        uint32_t skinPaletteOffset{0};
    };

    bool createDescriptors(const VulkanContext& context, uint32_t frameCount);
    bool uploadBuffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage, GpuBuffer& output);
    bool uploadTexture(const TextureAsset& texture, GpuTexture& output);
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
    void beginRendering(VkCommandBuffer commandBuffer, VkImage colorImage, VkImageView colorView,
                        VkImageView motionView, VkImage depthImage, VkImageView depthView, VkExtent2D extent) const;

    VkDevice m_device{VK_NULL_HANDLE};
    VkPipelineCache m_pipelineCache{VK_NULL_HANDLE};
    VmaAllocator m_allocator{VK_NULL_HANDLE};
    UploadContext m_uploadContext;
    UploadContext m_textureUploadContext;
    VkDescriptorSetLayout m_cameraLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_materialLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    VkSampler m_sampler{VK_NULL_HANDLE};
    std::vector<CameraBuffer> m_cameras;
    std::unordered_map<uint64_t, GpuMesh> m_meshes;
    std::unordered_map<uint64_t, GpuTexture> m_textures;
    std::unordered_map<uint64_t, GpuMaterial> m_materials;
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
    std::vector<GpuDrivenObjectRecord> m_gpuDrivenObjects;
    std::vector<GpuDrivenBatch> m_gpuDrivenBatches;
    std::vector<const RenderItem*> m_gpuDrivenBatchItems;
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
