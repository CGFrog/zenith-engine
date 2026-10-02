#pragma once

#include "../Rendering.hpp"
#include "../resources/FrameTimestamps.hpp"
#include "../vulkan/VulkanContext.hpp"
#include "BloomExposurePass.hpp"
#include "ColorGradePass.hpp"
#include "SceneGeometryViews.hpp"
#include "ScreenSpaceReflectionPass.hpp"
#include "TemporalResolvePass.hpp"
#include <chrono>
#include <filesystem>
#include <optional>
#include <vector>

// Per-frame inputs of the post chain beyond the camera and settings.
struct PostProcessFrame {
    // Increases by one per recorded frame; retires replaced resources and
    // seeds film grain, so deterministic runs repeat the grain exactly.
    uint64_t serial{0};
    // Directory relative asset paths in the settings resolve against (see
    // RenderRuntime::setAssetRoot); empty uses the packaged asset root.
    const std::filesystem::path& assetRoot;
    // Receives the colour-grade scope (FrameTimestamp::colorGradeBegin/End),
    // written back to back when no grade effect is active.
    OptionalPassTimestamps& timestamps;
};

class PostProcessPass {
  public:
    static constexpr VkFormat sceneFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat motionFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    // Depth prepass output: octahedral view-space normal in rg, roughness in
    // b, coverage in a. SceneNormalEncoding.hpp documents the encoding.
    static constexpr VkFormat normalFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    // Depth prepass output for screen-space reflections: linear specular
    // reflectance (F0) in rgb through the sRGB transfer, coverage in a.
    // SceneSpecularEncoding.hpp documents the encoding.
    static constexpr VkFormat specularFormat = VK_FORMAT_R8G8B8A8_SRGB;

    bool init(const VulkanContext& context, VkFormat outputFormat, VkFormat depthFormat, uint32_t frameCount,
              VkExtent2D extent, VkImageView depthView);
    bool resize(const VulkanContext& context, VkExtent2D extent, VkImageView depthView);
    void prepareScene(VkCommandBuffer commandBuffer, uint32_t frameIndex);
    // Whether recordReflections() traces for this geometry and quality:
    // reflections need the depth prepass outputs, including specular.
    static bool tracesReflections(const SceneGeometryViews& geometry, ReflectionQuality reflections);
    // Blends screen-space reflections into the frame's HDR target when
    // tracesReflections() holds, between the opaque surfaces (and background)
    // and the blended ones. The scene pass must have ended rendering; the HDR
    // target is in COLOR_ATTACHMENT_OPTIMAL before and after, ready for the
    // pass to resume with its attachments preserved. Records timestamps 12 and
    // 13 either way. Fails only if the reflection target cannot be allocated.
    bool recordReflections(VkCommandBuffer commandBuffer, uint32_t frameIndex, const SceneGeometryViews& geometry,
                           ReflectionQuality reflections, const ScreenSpaceReflectionCamera& camera,
                           VkQueryPool timestampQueries);
    bool record(VkCommandBuffer commandBuffer, uint32_t frameIndex, VkImage outputImage, VkImageView outputView,
                VkImage depthImage, VkExtent2D extent, VkExtent2D internalExtent, const RenderSettings& settings,
                const glm::mat4& inverseViewProjection, const glm::mat4& previousViewProjection,
                const TemporalResolveFrame& temporal, const glm::vec3& cameraPosition,
                const std::optional<GridOverlay>& gridOverlay, const PostProcessFrame& frame,
                VkQueryPool timestampQueries, double& bloomCpuMilliseconds,
                VkImageLayout outputOldLayout = VK_IMAGE_LAYOUT_UNDEFINED);
    void shutdown(const VulkanContext& context);

    // Advances exposure adaptation by this many seconds per recorded frame
    // instead of by wall-clock time; nullopt restores wall-clock adaptation.
    void setFixedFrameSeconds(std::optional<float> seconds) {
        m_fixedFrameSeconds = seconds;
    }

    VkImage sceneImage(uint32_t frameIndex) const;
    VkImageView sceneView(uint32_t frameIndex) const;
    VkImage motionImage(uint32_t frameIndex) const;
    VkImageView motionView(uint32_t frameIndex) const;
    // Moves the frame's normal target to COLOR_ATTACHMENT_OPTIMAL for the depth
    // prepass, discarding its previous contents. The frame's specular target
    // is unwritten until prepareSpecular().
    void prepareNormals(VkCommandBuffer commandBuffer, uint32_t frameIndex);
    // Allocates the frame's specular target on first use and moves it to
    // COLOR_ATTACHMENT_OPTIMAL for the depth prepass, discarding its previous
    // contents. Call after prepareNormals() on frames that trace screen-space
    // reflections. Fails only if the target cannot be allocated.
    bool prepareSpecular(VkCommandBuffer commandBuffer, uint32_t frameIndex);
    // Makes the prepass normals, and the specular target if prepared this
    // frame, readable (SHADER_READ_ONLY_OPTIMAL) by compute and fragment
    // shaders.
    void finishNormals(VkCommandBuffer commandBuffer, uint32_t frameIndex);
    VkImageView normalView(uint32_t frameIndex) const;
    // The specular target if prepareSpecular() ran since the slot's last
    // prepareNormals(), else VK_NULL_HANDLE.
    VkImageView specularView(uint32_t frameIndex) const;

    uint64_t temporalHistoryBytes() const {
        return m_temporalResolve.historyBytes();
    }

    // The reflection target plus every allocated prepass specular target.
    uint64_t reflectionBytes() const;

  private:
    struct Target {
        VkImage image{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
        VmaAllocation allocation{VK_NULL_HANDLE};
        VkDescriptorSet descriptor{VK_NULL_HANDLE};
        bool initialized{false};
        VkImage motionImage{VK_NULL_HANDLE};
        VkImageView motionView{VK_NULL_HANDLE};
        VmaAllocation motionAllocation{VK_NULL_HANDLE};
        bool motionInitialized{false};
        VkImage normalImage{VK_NULL_HANDLE};
        VkImageView normalView{VK_NULL_HANDLE};
        VmaAllocation normalAllocation{VK_NULL_HANDLE};
        bool normalInitialized{false};
        // Allocated on first use; written only on frames that trace
        // screen-space reflections.
        VkImage specularImage{VK_NULL_HANDLE};
        VkImageView specularView{VK_NULL_HANDLE};
        VmaAllocation specularAllocation{VK_NULL_HANDLE};
        bool specularInitialized{false};
        bool specularWritten{false};
    };

    bool createDescriptors(uint32_t frameCount);
    bool createPipelineLayout();
    bool createPipeline(VkFormat outputFormat, VkPipeline& output);
    bool createTargets(const VulkanContext& context, VkExtent2D extent, VkImageView depthView);
    void destroyTargets(const VulkanContext& context);
    bool ensureSpecularTarget(Target& target);
    // Runs the temporal resolve when enabled and binds the latest resolved image
    // as the post pass's binding 2. Records timestamps 19 and 20 either way.
    bool resolveTemporal(VkCommandBuffer commandBuffer, uint32_t frameIndex, VkExtent2D internalExtent, bool enabled,
                         const TemporalResolveFrame& temporal, VkQueryPool timestampQueries);
    void recordBloomAndExposure(VkCommandBuffer commandBuffer, const BloomExposureInput& input,
                                const RenderSettings& settings, VkQueryPool timestampQueries,
                                double& bloomCpuMilliseconds);
    float exposureDeltaSeconds();
    void transitionOutput(VkCommandBuffer commandBuffer, VkImage outputImage, VkImageLayout outputOldLayout);
    // Draws post.frag (scene composite, exposure, bloom, tone map) into target,
    // which must be in COLOR_ATTACHMENT_OPTIMAL. postSettings points to the
    // push-constant block.
    void recordToneMap(VkCommandBuffer commandBuffer, VkDescriptorSet descriptor, VkImageView target,
                       VkPipeline pipeline, VkExtent2D extent, const void* postSettings);

    VkDevice m_device{VK_NULL_HANDLE};
    VkPipelineCache m_pipelineCache{VK_NULL_HANDLE};
    VmaAllocator m_allocator{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_layout{VK_NULL_HANDLE};
    VkDescriptorPool m_pool{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};
    // Tone-maps into the output format, or into the colour-grade display
    // target when a grade effect is active.
    VkPipeline m_pipeline{VK_NULL_HANDLE};
    VkPipeline m_displayPipeline{VK_NULL_HANDLE};
    VkSampler m_colorSampler{VK_NULL_HANDLE};
    VkSampler m_depthSampler{VK_NULL_HANDLE};
    VkExtent2D m_extent{};
    std::vector<Target> m_targets;
    TemporalResolvePass m_temporalResolve;
    ScreenSpaceReflectionPass m_reflections;
    // The most recently recorded frame slot and its render extent; that slot's
    // motion target supplies the previous frame's motion and depth.
    std::optional<uint32_t> m_previousFrameIndex;
    VkExtent2D m_previousRenderExtent{};
    BloomExposurePass m_bloomExposure;
    ColorGradePass m_colorGrade;
    std::optional<std::chrono::steady_clock::time_point> m_previousExposureTime;
    std::optional<float> m_fixedFrameSeconds;
};
