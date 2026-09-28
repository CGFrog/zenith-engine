#pragma once

#include "../../assets/Assets.hpp"
#include "../../scene/Scene.hpp"
#include "../Rendering.hpp"
#include "../pipeline/GraphicsPipeline.hpp"
#include "../resources/UploadContext.hpp"
#include "../vulkan/VulkanContext.hpp"
#include "PostProcessPass.hpp"
#include <array>
#include <unordered_map>
#include <vector>

class MeshPass {
  public:
    bool init(const VulkanContext& context, VkFormat colorFormat, VkFormat depthFormat, uint32_t frameCount,
              VkExtent2D extent, VkImageView depthView);
    bool resize(const VulkanContext& context, VkExtent2D extent, VkImageView depthView);
    bool record(VkCommandBuffer commandBuffer, VkImage colorImage, VkImageView colorView, VkImage depthImage,
                VkImageView depthView, VkExtent2D extent, uint32_t frameIndex, const Scene& scene,
                const AssetManager& assets, RenderRuntime& rendering, VkQueryPool timestampQueries,
                VkImageLayout colorOldLayout = VK_IMAGE_LAYOUT_UNDEFINED);
    bool uiTexture(TextureHandle handle, const AssetManager& assets, VkSampler& sampler, VkImageView& view);
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
        MaterialHandle materialHandle;
        const MeshAsset* mesh{nullptr};
        const MeshPrimitive* primitive{nullptr};
        const MaterialAsset* material{nullptr};
        const GpuMesh* gpuMesh{nullptr};
        const GpuMaterial* gpuMaterial{nullptr};
        glm::mat4 world{1.0f};
        glm::vec3 boundsCenter{0.0f};
        float boundsRadius{0.0f};
        bool outlined{false};
        std::vector<glm::mat4> jointPalette;
        uint32_t skinPaletteOffset{0};
    };

    bool createDescriptors(const VulkanContext& context, uint32_t frameCount);
    bool uploadBuffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage, GpuBuffer& output);
    bool uploadTexture(const TextureAsset& texture, GpuTexture& output);
    const GpuMesh* ensureMesh(MeshHandle handle, const AssetManager& assets);
    const GpuTexture* ensureTexture(TextureHandle handle, const AssetManager& assets);
    const GpuMaterial* ensureMaterial(MaterialHandle handle, const AssetManager& assets);
    bool createPipeline(VkFormat colorFormat, VkFormat depthFormat);
    bool createShadowResources(VkFormat depthFormat);
    bool buildRenderItems(const Scene& scene, const AssetManager& assets,
                          const std::vector<RenderObjectId>& outlinedObjects, std::vector<RenderItem>& items);
    bool recordShadow(VkCommandBuffer commandBuffer, uint32_t frameIndex, const std::vector<RenderItem>& items,
                      const DirectionalLightComponent& light, const std::array<glm::mat4, 3>& lightViewProjections,
                      uint32_t cascadeCount, RenderStats& stats);
    void beginRendering(VkCommandBuffer commandBuffer, VkImage colorImage, VkImageView colorView, VkImage depthImage,
                        VkImageView depthView, VkExtent2D extent) const;

    VkDevice m_device{VK_NULL_HANDLE};
    VmaAllocator m_allocator{VK_NULL_HANDLE};
    UploadContext m_uploadContext;
    VkDescriptorSetLayout m_cameraLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_materialLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    VkSampler m_sampler{VK_NULL_HANDLE};
    std::vector<CameraBuffer> m_cameras;
    std::unordered_map<uint64_t, GpuMesh> m_meshes;
    std::unordered_map<uint64_t, GpuTexture> m_textures;
    std::unordered_map<uint64_t, GpuMaterial> m_materials;
    std::vector<RenderItem> m_renderItems;
    uint64_t m_renderPacketRevision{0};
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
    bool m_depthInitialized{false};
    GraphicsPipeline m_pipeline;
    GraphicsPipeline m_doubleSidedPipeline;
    GraphicsPipeline m_blendPipeline;
    GraphicsPipeline m_blendDoubleSidedPipeline;
    GraphicsPipeline m_shadowPipeline;
    GraphicsPipeline m_selectionMaskPipeline;
    GraphicsPipeline m_outlinePipeline;
    PostProcessPass m_postProcess;
};
