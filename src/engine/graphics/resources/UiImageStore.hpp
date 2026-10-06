#pragma once

// GPU images behind RenderRuntime's UI images.

#include "../Rendering.hpp"
#include "../passes/UiPass.hpp"
#include "../vulkan/VulkanContext.hpp"
#include "engine/resources/DeferredReleaseQueue.hpp"

#include <cstdint>
#include <vector>

// GPU images behind RenderRuntime's UI images. Uploads requested pixels into
// the frame being recorded, publishes their ImGui textures, and retires
// replaced or destroyed images once every frame that may sample them has
// completed.
class UiImageStore {
  public:
    bool init(const VulkanContext& context, UiPass& uiPass);
    // Records pending uploads into commandBuffer, which belongs to the frame
    // with serial recordingSerial, and publishes the new textures.
    void sync(VkCommandBuffer commandBuffer, uint64_t recordingSerial, RenderRuntime& rendering);
    // Releases images whose last sampling frame is at or before completedSerial.
    void collect(uint64_t completedSerial);
    // After the device is idle and before the UI pass shuts down.
    void shutdown();

  private:
    struct GpuImage {
        uint32_t generation{0};
        VkImage image{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
        VmaAllocation allocation{VK_NULL_HANDLE};
        uint64_t texture{0};
    };

    bool upload(VkCommandBuffer commandBuffer, uint64_t recordingSerial, const UiImage& pixels, GpuImage& image);
    void retire(uint64_t lastUseSerial, GpuImage image);
    void destroy(GpuImage& image);

    VkDevice m_device{VK_NULL_HANDLE};
    VmaAllocator m_allocator{VK_NULL_HANDLE};
    UiPass* m_uiPass{nullptr};
    VkFormat m_format{VK_FORMAT_R8G8B8A8_UNORM};
    VkSampler m_linearSampler{VK_NULL_HANDLE};
    VkSampler m_nearestSampler{VK_NULL_HANDLE};
    // Indexed by UI image slot.
    std::vector<GpuImage> m_images;
    DeferredReleaseQueue m_releases;
};
