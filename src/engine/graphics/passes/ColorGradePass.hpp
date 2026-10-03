#pragma once

#include "../Rendering.hpp"
#include "../vulkan/VulkanContext.hpp"
#include "engine/assets/ColorLut.hpp"
#include "engine/resources/DeferredReleaseQueue.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Display-referred finishing stage after tone mapping (shaders/color_grade.frag):
// a 3D LUT grade, a vignette and film grain, each selected by RenderSettings
// and Off by default. When any is active the post pass tone-maps into this
// pass's display target instead of the output, and record() draws the graded
// result into the output; otherwise the pass records nothing.
//
// Owns the display target (one output-sized RGBA16F image shared by every
// frame in flight: frames run in submission order on one queue, and the
// barriers recorded here order each frame's write after the previous frame's
// read) and the LUT texture (RGBA16F, size^3, linear filtering). The LUT file
// is parsed on the CPU (ColorLut.hpp) only when its setting or the asset root
// changes; a replaced texture and its staging buffer are destroyed a full
// frame ring later. Each frame slot has its own descriptor set, rewritten when
// the slot is next recorded after the LUT changes.
class ColorGradePass {
  public:
    static constexpr VkFormat displayFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat lutFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    bool init(const VulkanContext& context, VkFormat outputFormat, uint32_t frameCount, VkExtent2D extent);
    // Recreates the display target. The caller must ensure the GPU no longer
    // uses it.
    bool resize(VkExtent2D extent);
    void shutdown();

    // Loads the LUT the settings name when that changed (relative paths
    // resolve against assetRoot, or the packaged asset root when it is empty)
    // and records its upload. Returns whether the stage runs this frame. A
    // LUT that fails to load is reported once and skipped. Must be recorded
    // outside dynamic rendering. frameSerial increases by one per recorded
    // frame. Fails only if a Vulkan resource cannot be created.
    bool prepare(VkCommandBuffer commandBuffer, uint64_t frameSerial, const RenderSettings& settings,
                 const std::filesystem::path& assetRoot, bool& active);

    // Discards the display target and moves it to COLOR_ATTACHMENT_OPTIMAL for
    // the post pass, after the previous frame's grade read it.
    void beginDisplayTarget(VkCommandBuffer commandBuffer);

    VkImageView displayView() const {
        return m_display.view;
    }

    // Grades the display target into the output, which must be in
    // COLOR_ATTACHMENT_OPTIMAL. The post pass must have finished writing the
    // display target as a color attachment.
    void record(VkCommandBuffer commandBuffer, uint32_t frameIndex, VkImageView outputView, VkExtent2D extent,
                const RenderSettings& settings, uint64_t frameSerial);

    // Device memory of the display target and the LUT texture.
    uint64_t allocatedBytes() const;

  private:
    struct Image {
        VkImage image{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
        VmaAllocation allocation{VK_NULL_HANDLE};
    };

    struct LutTexture {
        Image image;
        uint32_t size{0};
        glm::vec3 domainMinimum{0.0f};
        glm::vec3 domainMaximum{1.0f};
    };

    bool createPipeline(VkFormat outputFormat);
    bool createDescriptors(uint32_t frameCount);
    bool createDisplayTarget(VkExtent2D extent);
    bool uploadLut(VkCommandBuffer commandBuffer, uint64_t frameSerial, const ColorLut& lut);
    // Reloads the LUT when the requested path or root changed; m_lutLoaded
    // reports whether a requested LUT is resident.
    bool updateLut(VkCommandBuffer commandBuffer, uint64_t frameSerial, const ColorLutSettings& settings,
                   const std::filesystem::path& assetRoot);
    void writeDescriptors(uint32_t frameIndex);
    void destroy(Image& image) const;

    VkDevice m_device{VK_NULL_HANDLE};
    LogChannel m_log;
    VmaAllocator m_allocator{VK_NULL_HANDLE};
    VkPipelineCache m_pipelineCache{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_layout{VK_NULL_HANDLE};
    VkDescriptorPool m_pool{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};
    VkPipeline m_pipeline{VK_NULL_HANDLE};
    VkSampler m_sampler{VK_NULL_HANDLE};
    std::vector<VkDescriptorSet> m_sets;
    // Bumped whenever the LUT texture or display target is replaced; a slot
    // whose set was written at an older generation is rewritten before use.
    // (Comparing view handles would be unsafe: a new view may reuse a
    // destroyed view's handle value.)
    uint64_t m_resourceGeneration{1};
    std::vector<uint64_t> m_boundGenerations;
    Image m_display;
    VkExtent2D m_extent{};
    bool m_displayInitialized{false};
    // The identity placeholder until a requested LUT loads.
    LutTexture m_lut;
    bool m_lutLoaded{false};
    std::string m_requestedPath;
    std::filesystem::path m_requestedRoot;
    DeferredReleaseQueue m_releases;
};
