#pragma once

#include "../../assets/Assets.hpp"
#include "../../scene/Scene.hpp"
#include "../pipeline/GraphicsPipeline.hpp"
#include "../vulkan/VulkanContext.hpp"
#include <unordered_map>
#include <vector>

class MeshPass {
  public:
    bool init(const VulkanContext& context, VkFormat colorFormat, VkFormat depthFormat, uint32_t frameCount);
    bool record(VkCommandBuffer commandBuffer, VkImage colorImage, VkImageView colorView, VkImage depthImage,
                VkImageView depthView, VkExtent2D extent, uint32_t frameIndex, const Scene& scene,
                const AssetManager& assets);
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

    struct CameraBuffer {
        GpuBuffer buffer;
        void* mapped{nullptr};
        VkDescriptorSet descriptor{VK_NULL_HANDLE};
    };

    bool createDescriptors(uint32_t frameCount);
    bool uploadBuffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage, GpuBuffer& output);
    const GpuMesh* ensureMesh(MeshHandle handle, const AssetManager& assets);
    bool createPipeline(VkFormat colorFormat, VkFormat depthFormat);
    void beginRendering(VkCommandBuffer commandBuffer, VkImage colorImage, VkImageView colorView, VkImage depthImage,
                        VkImageView depthView, VkExtent2D extent) const;

    VkDevice m_device{VK_NULL_HANDLE};
    VmaAllocator m_allocator{VK_NULL_HANDLE};
    VkQueue m_queue{VK_NULL_HANDLE};
    uint32_t m_queueFamily{0};
    VkDescriptorSetLayout m_cameraLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    std::vector<CameraBuffer> m_cameras;
    std::unordered_map<uint64_t, GpuMesh> m_meshes;
    GraphicsPipeline m_pipeline;
};
