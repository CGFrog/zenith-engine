#include "EnvironmentPass.hpp"

#include "engine/graphics/EnvironmentLighting.hpp"
#include "engine/graphics/ReflectionProbes.hpp"
#include "engine/graphics/resources/MipChainBlit.hpp"
#include "engine/graphics/shader/ShaderModule.hpp"
#include "engine/graphics/vulkan/VulkanCheck.hpp"
#include "engine/resources/ResourceLocator.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>

namespace {
constexpr VkFormat radianceFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr uint32_t cubeFaces = 6;
constexpr uint32_t computeGroupSize = 8;
constexpr uint32_t brdfTableSize = 256;
constexpr uint32_t brdfSampleCount = 512;
// Face size of the grid the irradiance projection samples.
constexpr uint32_t irradianceGridSize = 32;
constexpr uint32_t irradianceCoefficientCount = 9;
// The analytic sky is smooth, so a small radiance cube represents it exactly.
constexpr uint32_t proceduralRadianceSize = 128;
// Recently used environments kept resident; the frame's own are never evicted.
constexpr size_t maximumCachedEnvironments = 4;
constexpr uint32_t maximumComputeSets = 256;
// New probe environments precomputed per frame, so a scene's probes spread
// their build cost over a few frames instead of stalling one.
constexpr uint32_t maximumProbeBuildsPerFrame = 2;
constexpr VkDeviceSize irradianceBytes = sizeof(glm::vec4) * irradianceCoefficientCount;

// Mirrors the push-constant blocks of the env_*.comp filters.
struct FilterSettings {
    glm::vec4 parameters{0.0f};
    glm::uvec4 sizes{0u};
};

// Mirrors the push-constant block of env_procedural_sky.comp.
struct ProceduralSkySettings {
    glm::vec4 skyColor{0.0f};
    glm::vec4 groundColor{0.0f};
    glm::uvec4 sizes{0u};
};

// Mirrors the push-constant block of skybox.frag.
struct SkySettings {
    glm::vec4 parameters{0.0f};
    glm::vec4 temporalJitter{0.0f};
};

uint32_t mipCountFor(uint32_t width, uint32_t height) {
    return fullMipChainLength(width, height);
}

uint32_t groupsFor(uint32_t size) {
    return (size + computeGroupSize - 1) / computeGroupSize;
}

void imageBarrier(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                  VkPipelineStageFlags2 sourceStage, VkAccessFlags2 sourceAccess,
                  VkPipelineStageFlags2 destinationStage, VkAccessFlags2 destinationAccess, uint32_t baseMip,
                  uint32_t mipCount, uint32_t layerCount, uint32_t baseLayer = 0) {
    const VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                        .srcStageMask = sourceStage,
                                        .srcAccessMask = sourceAccess,
                                        .dstStageMask = destinationStage,
                                        .dstAccessMask = destinationAccess,
                                        .oldLayout = oldLayout,
                                        .newLayout = newLayout,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .image = image,
                                        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                             .baseMipLevel = baseMip,
                                                             .levelCount = mipCount,
                                                             .baseArrayLayer = baseLayer,
                                                             .layerCount = layerCount}};
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

void memoryBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags2 sourceStage, VkAccessFlags2 sourceAccess,
                   VkPipelineStageFlags2 destinationStage, VkAccessFlags2 destinationAccess) {
    const VkMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                   .srcStageMask = sourceStage,
                                   .srcAccessMask = sourceAccess,
                                   .dstStageMask = destinationStage,
                                   .dstAccessMask = destinationAccess};
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

// Every environment image is read by compute (prefilter, probe capture) and
// fragment shading (background).
constexpr MipChainConsumers environmentImageReaders{.stages = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                                              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                                    .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};

bool hasUsablePixels(const TextureAsset& texture) {
    const size_t components = static_cast<size_t>(texture.width) * texture.height * 4;
    return components != 0 && (texture.rgbaHalf.size() == components || texture.rgba.size() == components);
}

bool sourceUsesMap(const EnvironmentSource& source) {
    return source.mapAsset != nullptr && hasUsablePixels(*source.mapAsset);
}

uint64_t environmentKey(const EnvironmentSource& source, bool usesMap, EnvironmentQuality quality) {
    uint64_t hash = 1469598103934665603ULL;
    const auto append = [&](uint32_t value) {
        for (uint32_t shift = 0; shift < 32; shift += 8) {
            hash = (hash ^ ((value >> shift) & 0xFFu)) * 1099511628211ULL;
        }
    };
    const auto appendColor = [&](const glm::vec3& color) {
        for (int component = 0; component < 3; ++component) {
            uint32_t bits = 0;
            std::memcpy(&bits, &color[component], sizeof(bits));
            append(bits);
        }
    };
    append(usesMap ? 1u : 0u);
    append(static_cast<uint32_t>(quality));
    if (usesMap) {
        append(source.map.index);
        append(source.map.generation);
    } else {
        appendColor(source.skyColor);
        appendColor(source.groundColor);
    }
    return hash;
}

VkDescriptorSetLayoutBinding layoutBinding(uint32_t binding, VkDescriptorType type, VkShaderStageFlags stages) {
    return {.binding = binding, .descriptorType = type, .descriptorCount = 1, .stageFlags = stages};
}
} // namespace

bool EnvironmentPass::init(const VulkanContext& context, VkDescriptorSetLayout cameraLayout, VkFormat colorFormat,
                           VkFormat motionFormat, VkFormat depthFormat, uint32_t frameCount) {
    m_log = context.log();
    m_device = context.device();
    m_allocator = context.vmaAllocator();
    m_frameCount = frameCount;
    m_frameBindings.assign(frameCount, {});
    if (!createLayouts() || !createSamplers() ||
        !createPipelines(context, cameraLayout, colorFormat, motionFormat, depthFormat) || !createSkySets(frameCount)) {
        return false;
    }
    const VkImageCreateInfo table{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                  .imageType = VK_IMAGE_TYPE_2D,
                                  .format = radianceFormat,
                                  .extent = {.width = brdfTableSize, .height = brdfTableSize, .depth = 1},
                                  .mipLevels = 1,
                                  .arrayLayers = 1,
                                  .samples = VK_SAMPLE_COUNT_1_BIT,
                                  .tiling = VK_IMAGE_TILING_OPTIMAL,
                                  .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (!createImage(table, m_brdfTable, "Create environment BRDF table")) {
        return false;
    }
    m_brdfTable.view = createView(m_brdfTable.image, VK_IMAGE_VIEW_TYPE_2D, radianceFormat, 0, 1, 1);
    return m_brdfTable.view != VK_NULL_HANDLE;
}

bool EnvironmentPass::createLayouts() {
    const VkDescriptorSetLayoutBinding computeBindings[]{
        layoutBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT),
        layoutBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT),
        layoutBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)};
    const VkDescriptorSetLayoutCreateInfo computeInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                      .bindingCount = static_cast<uint32_t>(std::size(computeBindings)),
                                                      .pBindings = computeBindings};
    const VkDescriptorSetLayoutBinding skyBinding =
        layoutBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);
    const VkDescriptorSetLayoutCreateInfo skyInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &skyBinding};
    const VkDescriptorPoolSize computeSizes[]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, maximumComputeSets},
                                              {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, maximumComputeSets},
                                              {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, maximumComputeSets}};
    // Precomputation sets are transient and freed once their frame retires.
    const VkDescriptorPoolCreateInfo computePool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                                 .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
                                                 .maxSets = maximumComputeSets,
                                                 .poolSizeCount = static_cast<uint32_t>(std::size(computeSizes)),
                                                 .pPoolSizes = computeSizes};
    return checkVulkan(vkCreateDescriptorSetLayout(m_device, &computeInfo, nullptr, &m_computeLayout),
                       "Create environment precompute layout") &&
           checkVulkan(vkCreateDescriptorSetLayout(m_device, &skyInfo, nullptr, &m_skyLayout),
                       "Create skybox descriptor layout") &&
           checkVulkan(vkCreateDescriptorPool(m_device, &computePool, nullptr, &m_computePool),
                       "Create environment precompute pool");
}

bool EnvironmentPass::createSamplers() {
    VkSamplerCreateInfo cube{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                             .magFilter = VK_FILTER_LINEAR,
                             .minFilter = VK_FILTER_LINEAR,
                             .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
                             .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                             .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                             .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                             .maxLod = VK_LOD_CLAMP_NONE};
    if (!checkVulkan(vkCreateSampler(m_device, &cube, nullptr, &m_cubeSampler), "Create environment sampler")) {
        return false;
    }
    // Longitude wraps; latitude clamps at the poles.
    VkSamplerCreateInfo equirectangular = cube;
    equirectangular.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    return checkVulkan(vkCreateSampler(m_device, &equirectangular, nullptr, &m_equirectangularSampler),
                       "Create equirectangular sampler");
}

bool EnvironmentPass::createPipelines(const VulkanContext& context, VkDescriptorSetLayout cameraLayout,
                                      VkFormat colorFormat, VkFormat motionFormat, VkFormat depthFormat) {
    const std::filesystem::path shaders = shaderRoot();
    ShaderModule equirectangular(m_device);
    ShaderModule procedural(m_device);
    ShaderModule prefilter(m_device);
    ShaderModule irradiance(m_device);
    ShaderModule brdf(m_device);
    ShaderModule skyboxVertex(m_device);
    ShaderModule skyboxFragment(m_device);
    if (!equirectangular.load(shaders / "env_equirect_to_cube.comp.spv") ||
        !procedural.load(shaders / "env_procedural_sky.comp.spv") ||
        !prefilter.load(shaders / "env_prefilter_specular.comp.spv") ||
        !irradiance.load(shaders / "env_irradiance_sh.comp.spv") || !brdf.load(shaders / "env_brdf_lut.comp.spv") ||
        !skyboxVertex.load(shaders / "skybox.vert.spv") || !skyboxFragment.load(shaders / "skybox.frag.spv")) {
        return false;
    }
    const VkPipelineCache cache = context.pipelineCache();
    return m_equirectangularPipeline.init(m_device, m_computeLayout, equirectangular.handle(), cache,
                                          sizeof(FilterSettings)) &&
           m_proceduralPipeline.init(m_device, m_computeLayout, procedural.handle(), cache,
                                     sizeof(ProceduralSkySettings)) &&
           m_prefilterPipeline.init(m_device, m_computeLayout, prefilter.handle(), cache, sizeof(FilterSettings)) &&
           m_irradiancePipeline.init(m_device, m_computeLayout, irradiance.handle(), cache, sizeof(FilterSettings)) &&
           m_brdfPipeline.init(m_device, m_computeLayout, brdf.handle(), cache, sizeof(FilterSettings)) &&
           m_skyboxPipeline.initSkyboxPipeline(m_device, colorFormat, motionFormat, depthFormat, cameraLayout,
                                               m_skyLayout, skyboxVertex.handle(), skyboxFragment.handle(), cache,
                                               sizeof(SkySettings));
}

bool EnvironmentPass::createSkySets(uint32_t frameCount) {
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, frameCount};
    const VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                          .maxSets = frameCount,
                                          .poolSizeCount = 1,
                                          .pPoolSizes = &size};
    if (!checkVulkan(vkCreateDescriptorPool(m_device, &pool, nullptr, &m_skyPool), "Create skybox descriptor pool")) {
        return false;
    }
    const std::vector<VkDescriptorSetLayout> layouts(frameCount, m_skyLayout);
    m_skySets.resize(frameCount);
    const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                               .descriptorPool = m_skyPool,
                                               .descriptorSetCount = frameCount,
                                               .pSetLayouts = layouts.data()};
    return checkVulkan(vkAllocateDescriptorSets(m_device, &allocate, m_skySets.data()), "Allocate skybox sets");
}

std::optional<EnvironmentFrameState> EnvironmentPass::prepare(VkCommandBuffer commandBuffer, uint32_t frameIndex,
                                                              uint64_t frameSerial,
                                                              const EnvironmentFrameRequest& request,
                                                              std::span<const VkDescriptorSet> cameraSets) {
    if (frameIndex >= m_frameBindings.size()) {
        return std::nullopt;
    }
    m_frameSerial = frameSerial;
    // The caller waited for this frame slot, so every frame a full ring older
    // than this one has completed.
    if (frameSerial > m_frameCount) {
        m_releaseQueue.collect(frameSerial - m_frameCount);
    }
    if (!m_brdfReady && !buildBrdfTable(commandBuffer)) {
        return std::nullopt;
    }
    const Environment* primary = ensureEnvironment(commandBuffer, request.primary, request.quality);
    if (!primary && request.primary.mapAsset) {
        // A map that cannot be built falls back to the analytic sky.
        EnvironmentSource analytic = request.primary;
        analytic.mapAsset = nullptr;
        primary = ensureEnvironment(commandBuffer, analytic, request.quality);
    }
    if (!primary) {
        return std::nullopt;
    }
    const Environment* probe = nullptr;
    if (request.probe && request.probeWeight > 0.0f) {
        probe = ensureEnvironment(commandBuffer, *request.probe, request.quality);
    }
    EnvironmentFrameState state{.probeWeight = probe ? std::clamp(request.probeWeight, 0.0f, 1.0f) : 0.0f,
                                .primaryIrradiance = primary->irradiance.buffer};
    // The probe array binding must stay valid, so without resident probes a
    // one-texel placeholder array is kept instead of the full cache.
    // A full cache that could not be allocated is not retried; the frame keeps
    // the placeholder, so probes are left out and shading uses the scene
    // environment instead.
    const bool fullCache = !request.residentProbes.empty() && !m_probeCacheUnavailable;
    if (!ensureProbeCache(commandBuffer, request.quality, fullCache ? maximumResidentReflectionProbes : 1u)) {
        if (!fullCache) {
            return std::nullopt;
        }
        m_probeCacheUnavailable = true;
        m_log.warning("The reflection probe cache could not be allocated; per-pixel probes are disabled");
        if (!ensureProbeCache(commandBuffer, request.quality, 1u)) {
            return std::nullopt;
        }
    }
    state.probeSlots = updateResidentProbes(commandBuffer, request);
    // Slot 1 always needs a valid descriptor; an unweighted copy of slot 0 costs nothing.
    writeCameraSets(m_frameBindings[frameIndex], cameraSets, {primary, probe ? probe : primary});
    if (FrameBinding& binding = m_frameBindings[frameIndex]; binding.sky != primary->id) {
        const VkDescriptorImageInfo sky{.sampler = m_cubeSampler,
                                        .imageView = primary->radiance.view,
                                        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                         .dstSet = m_skySets[frameIndex],
                                         .dstBinding = 0,
                                         .descriptorCount = 1,
                                         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                         .pImageInfo = &sky};
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
        binding.sky = primary->id;
    }
    m_frameBindings[frameIndex].proceduralSky = primary->procedural;
    // Both slots share the frame's quality and therefore the specular mip count.
    state.specularMaximumLod = static_cast<float>(resolutionFor(request.quality, false).specularMips - 1);
    evictUnusedEnvironments();
    return state;
}

void EnvironmentPass::recordSkybox(VkCommandBuffer commandBuffer, uint32_t frameIndex, VkDescriptorSet cameraSet,
                                   float radianceScale, const glm::vec4& temporalJitter) const {
    if (frameIndex >= m_skySets.size() || m_frameBindings[frameIndex].sky == 0) {
        return;
    }
    const VkDescriptorSet sets[]{cameraSet, m_skySets[frameIndex]};
    const SkySettings settings{
        .parameters = {std::max(radianceScale, 0.0f), m_frameBindings[frameIndex].proceduralSky ? 1.0f : 0.0f, 0, 0},
        .temporalJitter = temporalJitter};
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skyboxPipeline.handle());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skyboxPipeline.layout(), 0,
                            static_cast<uint32_t>(std::size(sets)), sets, 0, nullptr);
    vkCmdPushConstants(commandBuffer, m_skyboxPipeline.layout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(settings),
                       &settings);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
}

EnvironmentPass::Resolution EnvironmentPass::resolutionFor(EnvironmentQuality quality, bool usesMap) {
    // Hemisphere lighting still keeps a low-resolution analytic sky bound so the
    // camera set stays valid without shader variants.
    const bool high = quality == EnvironmentQuality::ImageBasedHigh;
    return {.radianceSize = usesMap ? (high ? 512u : 256u) : proceduralRadianceSize,
            .specularSize = high ? 256u : 128u,
            .specularMips = high ? 6u : 5u,
            .sampleCount = high ? 128u : 64u};
}

const EnvironmentPass::Environment* EnvironmentPass::ensureEnvironment(VkCommandBuffer commandBuffer,
                                                                       const EnvironmentSource& source,
                                                                       EnvironmentQuality quality) {
    const bool usesMap = sourceUsesMap(source);
    const uint64_t key = environmentKey(source, usesMap, quality);
    if (const auto found = m_environments.find(key); found != m_environments.end()) {
        found->second.lastUsedSerial = m_frameSerial;
        return &found->second;
    }
    if (m_failedEnvironments.contains(key)) {
        return nullptr;
    }
    const Resolution resolution = resolutionFor(quality, usesMap);
    EnvironmentSource resolved = source;
    if (!usesMap) {
        resolved.mapAsset = nullptr;
    }
    Environment environment{.procedural = !usesMap, .lastUsedSerial = m_frameSerial, .id = m_nextEnvironmentId++};
    if (!buildEnvironment(commandBuffer, resolved, resolution, environment)) {
        // Commands may already reference the partial build, so wait for the frame.
        m_releaseQueue.defer(m_frameSerial, [this, environment] { destroyEnvironment(environment); });
        // Remember the failure so a source that cannot fit is not rebuilt every frame.
        m_failedEnvironments.insert(key);
        m_log.warning(std::string("Image-based lighting could not prepare an environment ") +
                      (usesMap ? "map; using the analytic sky" : "sky"));
        return nullptr;
    }
    return &m_environments.emplace(key, environment).first->second;
}

bool EnvironmentPass::buildEnvironment(VkCommandBuffer commandBuffer, const EnvironmentSource& source,
                                       const Resolution& resolution, Environment& environment) {
    VkImageCreateInfo cube{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                           .flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
                           .imageType = VK_IMAGE_TYPE_2D,
                           .format = radianceFormat,
                           .extent = {.width = resolution.radianceSize, .height = resolution.radianceSize, .depth = 1},
                           .mipLevels = mipCountFor(resolution.radianceSize, resolution.radianceSize),
                           .arrayLayers = cubeFaces,
                           .samples = VK_SAMPLE_COUNT_1_BIT,
                           .tiling = VK_IMAGE_TILING_OPTIMAL,
                           .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (!createImage(cube, environment.radiance, "Create environment radiance cube")) {
        return false;
    }
    environment.radiance.view =
        createView(environment.radiance.image, VK_IMAGE_VIEW_TYPE_CUBE, radianceFormat, 0, cube.mipLevels, cubeFaces);
    cube.extent = {.width = resolution.specularSize, .height = resolution.specularSize, .depth = 1};
    cube.mipLevels = resolution.specularMips;
    // Transfer source for copies into the resident probe cache.
    cube.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!environment.radiance.view || !createImage(cube, environment.specular, "Create prefiltered specular cube")) {
        return false;
    }
    environment.specular.view = createView(environment.specular.image, VK_IMAGE_VIEW_TYPE_CUBE, radianceFormat, 0,
                                           resolution.specularMips, cubeFaces);
    const VkBufferCreateInfo buffer{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                    .size = sizeof(glm::vec4) * irradianceCoefficientCount,
                                    .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    const VmaAllocationCreateInfo allocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!environment.specular.view ||
        !checkVulkan(vmaCreateBuffer(m_allocator, &buffer, &allocation, &environment.irradiance.buffer,
                                     &environment.irradiance.allocation, nullptr),
                     "Create environment irradiance buffer")) {
        return false;
    }
    return renderRadiance(commandBuffer, source, resolution, environment.radiance) &&
           prefilterSpecular(commandBuffer, resolution, environment) &&
           projectIrradiance(commandBuffer, resolution, environment);
}

bool EnvironmentPass::renderRadiance(VkCommandBuffer commandBuffer, const EnvironmentSource& source,
                                     const Resolution& resolution, const Image& radiance) {
    const uint32_t size = resolution.radianceSize;
    const uint32_t mipCount = mipCountFor(size, size);
    imageBarrier(commandBuffer, radiance.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                 VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, 0, mipCount, cubeFaces);
    const VkImageView target = createView(radiance.image, VK_IMAGE_VIEW_TYPE_2D_ARRAY, radianceFormat, 0, 1, cubeFaces);
    const VkDescriptorSet set = target ? allocateComputeSet() : VK_NULL_HANDLE;
    if (!set) {
        retireView(target);
        return false;
    }
    const VkDescriptorImageInfo targetInfo{.imageView = target, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
    std::array<VkWriteDescriptorSet, 2> writes{};
    writes[0] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                 .dstSet = set,
                 .dstBinding = 1,
                 .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                 .pImageInfo = &targetInfo};
    if (source.mapAsset) {
        Image equirectangular;
        uint32_t sourceMips = 0;
        if (!uploadEquirectangular(commandBuffer, *source.mapAsset, equirectangular, sourceMips)) {
            retireImage(equirectangular);
            retireView(target);
            retireComputeSet(set);
            return false;
        }
        const VkDescriptorImageInfo sourceInfo{.sampler = m_equirectangularSampler,
                                               .imageView = equirectangular.view,
                                               .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writes[1] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                     .dstSet = set,
                     .dstBinding = 0,
                     .descriptorCount = 1,
                     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                     .pImageInfo = &sourceInfo};
        vkUpdateDescriptorSets(m_device, 2, writes.data(), 0, nullptr);
        // The map spans four faces around the horizon; pick the mip whose texel
        // density matches the cube to avoid aliasing.
        const float sourceLod =
            std::clamp(std::log2(static_cast<float>(source.mapAsset->width) / static_cast<float>(4 * size)), 0.0f,
                       static_cast<float>(sourceMips - 1));
        const FilterSettings settings{.parameters = {sourceLod, 0.0f, 0.0f, 0.0f}, .sizes = {size, 0u, 0u, 0u}};
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_equirectangularPipeline.handle());
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_equirectangularPipeline.layout(), 0, 1,
                                &set, 0, nullptr);
        vkCmdPushConstants(commandBuffer, m_equirectangularPipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(settings), &settings);
        vkCmdDispatch(commandBuffer, groupsFor(size), groupsFor(size), cubeFaces);
        retireImage(equirectangular);
    } else {
        vkUpdateDescriptorSets(m_device, 1, writes.data(), 0, nullptr);
        const ProceduralSkySettings settings{.skyColor = {glm::max(source.skyColor, glm::vec3(0.0f)), 0.0f},
                                             .groundColor = {glm::max(source.groundColor, glm::vec3(0.0f)), 0.0f},
                                             .sizes = {size, 0u, 0u, 0u}};
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_proceduralPipeline.handle());
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_proceduralPipeline.layout(), 0, 1,
                                &set, 0, nullptr);
        vkCmdPushConstants(commandBuffer, m_proceduralPipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(settings), &settings);
        vkCmdDispatch(commandBuffer, groupsFor(size), groupsFor(size), cubeFaces);
    }
    retireView(target);
    retireComputeSet(set);
    imageBarrier(commandBuffer, radiance.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT, 0,
                 mipCount, cubeFaces);
    recordMipChainBlits(
        commandBuffer,
        {.image = radiance.image, .width = size, .height = size, .layerCount = cubeFaces, .mipCount = mipCount},
        environmentImageReaders);
    return true;
}

bool EnvironmentPass::uploadEquirectangular(VkCommandBuffer commandBuffer, const TextureAsset& texture, Image& output,
                                            uint32_t& mipCount) {
    const bool highDynamicRange = !texture.rgbaHalf.empty();
    const size_t componentCount = static_cast<size_t>(texture.width) * texture.height * 4;
    const VkDeviceSize byteCount = componentCount * (highDynamicRange ? sizeof(uint16_t) : sizeof(uint8_t));
    const VkBufferCreateInfo stagingInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = byteCount, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    const VmaAllocationCreateInfo stagingAllocation{.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                                             VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                                                    .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    Buffer staging;
    VmaAllocationInfo stagingDetails{};
    if (!checkVulkan(vmaCreateBuffer(m_allocator, &stagingInfo, &stagingAllocation, &staging.buffer,
                                     &staging.allocation, &stagingDetails),
                     "Create environment staging buffer")) {
        return false;
    }
    // The staging copy executes with this frame, so it retires with it.
    retireBuffer(staging);
    const void* texels = highDynamicRange ? static_cast<const void*>(texture.rgbaHalf.data()) : texture.rgba.data();
    std::memcpy(stagingDetails.pMappedData, texels, byteCount);
    if (!checkVulkan(vmaFlushAllocation(m_allocator, staging.allocation, 0, byteCount),
                     "Flush environment staging buffer")) {
        return false;
    }
    // Low-dynamic-range maps are authored in sRGB, like the hemisphere colors' sources.
    const VkFormat format = highDynamicRange ? radianceFormat : VK_FORMAT_R8G8B8A8_SRGB;
    mipCount = mipCountFor(texture.width, texture.height);
    const VkImageCreateInfo image{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                  .imageType = VK_IMAGE_TYPE_2D,
                                  .format = format,
                                  .extent = {.width = texture.width, .height = texture.height, .depth = 1},
                                  .mipLevels = mipCount,
                                  .arrayLayers = 1,
                                  .samples = VK_SAMPLE_COUNT_1_BIT,
                                  .tiling = VK_IMAGE_TILING_OPTIMAL,
                                  .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                           VK_IMAGE_USAGE_SAMPLED_BIT,
                                  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (!createImage(image, output, "Create equirectangular environment image")) {
        return false;
    }
    output.view = createView(output.image, VK_IMAGE_VIEW_TYPE_2D, format, 0, mipCount, 1);
    if (!output.view) {
        return false;
    }
    imageBarrier(commandBuffer, output.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT, 0, mipCount, 1);
    const VkBufferImageCopy copy{.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                      .mipLevel = 0,
                                                      .baseArrayLayer = 0,
                                                      .layerCount = 1},
                                 .imageExtent = {.width = texture.width, .height = texture.height, .depth = 1}};
    vkCmdCopyBufferToImage(commandBuffer, staging.buffer, output.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    recordMipChainBlits(commandBuffer,
                        {.image = output.image, .width = texture.width, .height = texture.height, .mipCount = mipCount},
                        environmentImageReaders);
    return true;
}

bool EnvironmentPass::prefilterSpecular(VkCommandBuffer commandBuffer, const Resolution& resolution,
                                        const Environment& environment) {
    imageBarrier(commandBuffer, environment.specular.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                 VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, 0, resolution.specularMips, cubeFaces);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_prefilterPipeline.handle());
    const VkDescriptorImageInfo sourceInfo{.sampler = m_cubeSampler,
                                           .imageView = environment.radiance.view,
                                           .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const float radianceMips = static_cast<float>(mipCountFor(resolution.radianceSize, resolution.radianceSize));
    for (uint32_t mip = 0; mip < resolution.specularMips; ++mip) {
        const VkImageView target =
            createView(environment.specular.image, VK_IMAGE_VIEW_TYPE_2D_ARRAY, radianceFormat, mip, 1, cubeFaces);
        const VkDescriptorSet set = target ? allocateComputeSet() : VK_NULL_HANDLE;
        if (!set) {
            retireView(target);
            return false;
        }
        const VkDescriptorImageInfo targetInfo{.imageView = target, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        const VkWriteDescriptorSet writes[]{{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                             .dstSet = set,
                                             .dstBinding = 0,
                                             .descriptorCount = 1,
                                             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                             .pImageInfo = &sourceInfo},
                                            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                             .dstSet = set,
                                             .dstBinding = 1,
                                             .descriptorCount = 1,
                                             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                             .pImageInfo = &targetInfo}};
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(std::size(writes)), writes, 0, nullptr);
        const uint32_t size = std::max(resolution.specularSize >> mip, 1u);
        const FilterSettings settings{
            .parameters = {environment_lighting::prefilteredMipRoughness(mip, resolution.specularMips),
                           static_cast<float>(resolution.radianceSize), radianceMips, 0.0f},
            .sizes = {size, resolution.sampleCount, 0u, 0u}};
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_prefilterPipeline.layout(), 0, 1, &set,
                                0, nullptr);
        vkCmdPushConstants(commandBuffer, m_prefilterPipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(settings), &settings);
        vkCmdDispatch(commandBuffer, groupsFor(size), groupsFor(size), cubeFaces);
        retireView(target);
        retireComputeSet(set);
    }
    imageBarrier(commandBuffer, environment.specular.image, VK_IMAGE_LAYOUT_GENERAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 0, resolution.specularMips, cubeFaces);
    return true;
}

bool EnvironmentPass::projectIrradiance(VkCommandBuffer commandBuffer, const Resolution& resolution,
                                        const Environment& environment) {
    const VkDescriptorSet set = allocateComputeSet();
    if (!set) {
        return false;
    }
    const VkDescriptorImageInfo sourceInfo{.sampler = m_cubeSampler,
                                           .imageView = environment.radiance.view,
                                           .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorBufferInfo outputInfo{.buffer = environment.irradiance.buffer, .range = VK_WHOLE_SIZE};
    const VkWriteDescriptorSet writes[]{{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                         .dstSet = set,
                                         .dstBinding = 0,
                                         .descriptorCount = 1,
                                         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                         .pImageInfo = &sourceInfo},
                                        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                         .dstSet = set,
                                         .dstBinding = 2,
                                         .descriptorCount = 1,
                                         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                         .pBufferInfo = &outputInfo}};
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(std::size(writes)), writes, 0, nullptr);
    const uint32_t gridSize = std::min(irradianceGridSize, resolution.radianceSize);
    const float sourceLod = std::log2(static_cast<float>(resolution.radianceSize) / static_cast<float>(gridSize));
    const FilterSettings settings{.parameters = {sourceLod, 0.0f, 0.0f, 0.0f}, .sizes = {gridSize, 0u, 0u, 0u}};
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_irradiancePipeline.handle());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_irradiancePipeline.layout(), 0, 1, &set, 0,
                            nullptr);
    vkCmdPushConstants(commandBuffer, m_irradiancePipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(settings),
                       &settings);
    vkCmdDispatch(commandBuffer, 1, 1, 1);
    retireComputeSet(set);
    // Shading reads the coefficients in fragment shaders, volumetric fog in compute.
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    return true;
}

bool EnvironmentPass::buildBrdfTable(VkCommandBuffer commandBuffer) {
    const VkImageView target = createView(m_brdfTable.image, VK_IMAGE_VIEW_TYPE_2D_ARRAY, radianceFormat, 0, 1, 1);
    const VkDescriptorSet set = target ? allocateComputeSet() : VK_NULL_HANDLE;
    if (!set) {
        retireView(target);
        return false;
    }
    const VkDescriptorImageInfo targetInfo{.imageView = target, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
    const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                     .dstSet = set,
                                     .dstBinding = 1,
                                     .descriptorCount = 1,
                                     .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                     .pImageInfo = &targetInfo};
    vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    imageBarrier(commandBuffer, m_brdfTable.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                 VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, 0, 1, 1);
    const FilterSettings settings{.sizes = {brdfTableSize, brdfSampleCount, 0u, 0u}};
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_brdfPipeline.handle());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_brdfPipeline.layout(), 0, 1, &set, 0,
                            nullptr);
    vkCmdPushConstants(commandBuffer, m_brdfPipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(settings),
                       &settings);
    vkCmdDispatch(commandBuffer, groupsFor(brdfTableSize), groupsFor(brdfTableSize), 1);
    imageBarrier(commandBuffer, m_brdfTable.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 0, 1, 1);
    retireView(target);
    retireComputeSet(set);
    m_brdfReady = true;
    return true;
}

VkDescriptorSet EnvironmentPass::allocateComputeSet() {
    const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                               .descriptorPool = m_computePool,
                                               .descriptorSetCount = 1,
                                               .pSetLayouts = &m_computeLayout};
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!checkVulkan(vkAllocateDescriptorSets(m_device, &allocate, &set), "Allocate environment precompute set")) {
        return VK_NULL_HANDLE;
    }
    return set;
}

bool EnvironmentPass::ensureProbeCache(VkCommandBuffer commandBuffer, EnvironmentQuality quality, uint32_t slotCount) {
    // The placeholder is one 1-texel cube; the full cache matches the
    // prefiltered cubes of the frame's quality so slots are plain copies.
    const bool placeholder = slotCount <= 1;
    const Resolution resolution = resolutionFor(quality, false);
    const uint32_t size = placeholder ? 1u : resolution.specularSize;
    const uint32_t mips = placeholder ? 1u : resolution.specularMips;
    if (m_probeCache.id != 0 && m_probeCache.slots.size() == slotCount && m_probeCache.size == size &&
        m_probeCache.mips == mips) {
        m_probeCache.quality = quality;
        return true;
    }
    if (m_probeCache.id != 0) {
        // Older frames' descriptor sets still name the cache until they retire.
        m_releaseQueue.defer(m_frameSerial, [this, cache = m_probeCache] { destroyProbeCache(cache); });
        m_probeCache = {};
    }
    ProbeCache cache{.quality = quality, .size = size, .mips = mips};
    const uint32_t layerCount = cubeFaces * slotCount;
    const VkImageCreateInfo array{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                  .flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
                                  .imageType = VK_IMAGE_TYPE_2D,
                                  .format = radianceFormat,
                                  .extent = {.width = size, .height = size, .depth = 1},
                                  .mipLevels = mips,
                                  .arrayLayers = layerCount,
                                  .samples = VK_SAMPLE_COUNT_1_BIT,
                                  .tiling = VK_IMAGE_TILING_OPTIMAL,
                                  .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    const VkBufferCreateInfo buffer{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                    .size = irradianceBytes * slotCount,
                                    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo allocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    bool created = createImage(array, cache.specular, "Create reflection probe cache");
    if (created) {
        cache.specular.view =
            createView(cache.specular.image, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, radianceFormat, 0, mips, layerCount);
        created = cache.specular.view != VK_NULL_HANDLE &&
                  checkVulkan(vmaCreateBuffer(m_allocator, &buffer, &allocation, &cache.irradiance.buffer,
                                              &cache.irradiance.allocation, nullptr),
                              "Create reflection probe irradiance buffer");
    }
    if (!created) {
        destroyProbeCache(cache);
        return false;
    }
    // Empty slots are never sampled, but clearing keeps the whole array in a
    // defined, shader-readable state.
    imageBarrier(commandBuffer, cache.specular.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT, 0, mips, layerCount);
    const VkClearColorValue black{};
    const VkImageSubresourceRange everything{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                             .baseMipLevel = 0,
                                             .levelCount = mips,
                                             .baseArrayLayer = 0,
                                             .layerCount = layerCount};
    vkCmdClearColorImage(commandBuffer, cache.specular.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                         &everything);
    imageBarrier(commandBuffer, cache.specular.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 0, mips, layerCount);
    vkCmdFillBuffer(commandBuffer, cache.irradiance.buffer, 0, VK_WHOLE_SIZE, 0);
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    cache.slots.assign(slotCount, {});
    cache.id = m_nextProbeCacheId++;
    m_probeCache = cache;
    return true;
}

std::vector<int32_t> EnvironmentPass::updateResidentProbes(VkCommandBuffer commandBuffer,
                                                           const EnvironmentFrameRequest& request) {
    const size_t requested = std::min<size_t>(request.residentProbes.size(), maximumResidentReflectionProbes);
    std::vector<int32_t> result(request.residentProbes.size(), -1);
    if (requested == 0 || m_probeCache.slots.size() < maximumResidentReflectionProbes) {
        return result;
    }
    std::vector<uint64_t> keys(requested);
    std::vector<bool> claimed(m_probeCache.slots.size(), false);
    const auto findSlot = [&](uint64_t key) -> int32_t {
        for (uint32_t slot = 0; slot < m_probeCache.slots.size(); ++slot) {
            if (m_probeCache.slots[slot].key == key) {
                return static_cast<int32_t>(slot);
            }
        }
        return -1;
    };
    // Resident probes keep their slots first, so a newcomer never evicts a
    // probe this frame still needs.
    for (size_t index = 0; index < requested; ++index) {
        const EnvironmentSource& source = request.residentProbes[index];
        keys[index] = environmentKey(source, sourceUsesMap(source), request.quality);
        if (const int32_t slot = findSlot(keys[index]); slot >= 0) {
            result[index] = slot;
            claimed[slot] = true;
            m_probeCache.slots[slot].lastUsedSerial = m_frameSerial;
        }
    }
    uint32_t builds = 0;
    for (size_t index = 0; index < requested; ++index) {
        if (result[index] >= 0) {
            continue;
        }
        // Probes sharing a source share the slot an earlier one just filled.
        if (const int32_t slot = findSlot(keys[index]); slot >= 0) {
            result[index] = slot;
            continue;
        }
        int32_t victim = -1;
        for (uint32_t slot = 0; slot < m_probeCache.slots.size(); ++slot) {
            if (!claimed[slot] &&
                (victim < 0 || m_probeCache.slots[slot].lastUsedSerial < m_probeCache.slots[victim].lastUsedSerial)) {
                victim = static_cast<int32_t>(slot);
            }
        }
        const bool cached = m_environments.contains(keys[index]);
        if (victim < 0 || m_failedEnvironments.contains(keys[index]) ||
            (!cached && builds >= maximumProbeBuildsPerFrame)) {
            continue;
        }
        const Environment* environment =
            ensureEnvironment(commandBuffer, request.residentProbes[index], request.quality);
        builds += cached ? 0u : 1u;
        if (!environment) {
            continue;
        }
        copyIntoProbeSlot(commandBuffer, *environment, static_cast<uint32_t>(victim));
        m_probeCache.slots[victim] = {.key = keys[index], .lastUsedSerial = m_frameSerial};
        claimed[victim] = true;
        result[index] = victim;
    }
    return result;
}

void EnvironmentPass::copyIntoProbeSlot(VkCommandBuffer commandBuffer, const Environment& environment, uint32_t slot) {
    const uint32_t mips = m_probeCache.mips;
    const uint32_t baseLayer = slot * cubeFaces;
    constexpr VkPipelineStageFlags2 samplers =
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    // The source may have been prefiltered earlier in this command buffer.
    imageBarrier(commandBuffer, environment.specular.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, samplers, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, 0, mips, cubeFaces);
    // Earlier frames may still sample the slot being replaced.
    imageBarrier(commandBuffer, m_probeCache.specular.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 0, mips, cubeFaces, baseLayer);
    // Irradiance was projected by a compute shader; earlier frames' transfers
    // may still read the slot's coefficients.
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                  VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    std::vector<VkImageCopy> regions(mips);
    for (uint32_t mip = 0; mip < mips; ++mip) {
        const uint32_t size = std::max(m_probeCache.size >> mip, 1u);
        regions[mip] = {.srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                           .mipLevel = mip,
                                           .baseArrayLayer = 0,
                                           .layerCount = cubeFaces},
                        .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                           .mipLevel = mip,
                                           .baseArrayLayer = baseLayer,
                                           .layerCount = cubeFaces},
                        .extent = {.width = size, .height = size, .depth = 1}};
    }
    vkCmdCopyImage(commandBuffer, environment.specular.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   m_probeCache.specular.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   static_cast<uint32_t>(regions.size()), regions.data());
    const VkBufferCopy coefficients{.srcOffset = 0, .dstOffset = irradianceBytes * slot, .size = irradianceBytes};
    vkCmdCopyBuffer(commandBuffer, environment.irradiance.buffer, m_probeCache.irradiance.buffer, 1, &coefficients);
    imageBarrier(commandBuffer, environment.specular.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0, samplers,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 0, mips, cubeFaces);
    imageBarrier(commandBuffer, m_probeCache.specular.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 0, mips, cubeFaces, baseLayer);
    // ReflectionProbeClusterPass gathers the coefficients with a transfer.
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
}

void EnvironmentPass::destroyProbeCache(const ProbeCache& cache) {
    if (cache.specular.view) {
        vkDestroyImageView(m_device, cache.specular.view, nullptr);
    }
    if (cache.specular.image) {
        vmaDestroyImage(m_allocator, cache.specular.image, cache.specular.allocation);
    }
    if (cache.irradiance.buffer) {
        vmaDestroyBuffer(m_allocator, cache.irradiance.buffer, cache.irradiance.allocation);
    }
}

void EnvironmentPass::writeCameraSets(FrameBinding& binding, std::span<const VkDescriptorSet> cameraSets,
                                      const std::array<const Environment*, environmentSlots>& environments) {
    std::array<uint64_t, environmentSlots> ids{};
    for (uint32_t slot = 0; slot < environmentSlots; ++slot) {
        ids[slot] = environments[slot]->id;
    }
    if (std::ranges::equal(binding.cameraSets, cameraSets) && binding.environments == ids &&
        binding.probeCache == m_probeCache.id) {
        return;
    }
    std::array<VkDescriptorImageInfo, environmentSlots> specularInfo{};
    std::array<VkDescriptorBufferInfo, environmentSlots> irradianceInfo{};
    for (uint32_t slot = 0; slot < environmentSlots; ++slot) {
        specularInfo[slot] = {.sampler = m_cubeSampler,
                              .imageView = environments[slot]->specular.view,
                              .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        irradianceInfo[slot] = {.buffer = environments[slot]->irradiance.buffer, .range = VK_WHOLE_SIZE};
    }
    const VkDescriptorImageInfo brdfInfo{.sampler = m_cubeSampler,
                                         .imageView = m_brdfTable.view,
                                         .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo probeInfo{.sampler = m_cubeSampler,
                                          .imageView = m_probeCache.specular.view,
                                          .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(cameraSets.size() * 4);
    for (VkDescriptorSet set : cameraSets) {
        writes.push_back({.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                          .dstSet = set,
                          .dstBinding = specularBinding,
                          .descriptorCount = environmentSlots,
                          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                          .pImageInfo = specularInfo.data()});
        writes.push_back({.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                          .dstSet = set,
                          .dstBinding = irradianceBinding,
                          .descriptorCount = environmentSlots,
                          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                          .pBufferInfo = irradianceInfo.data()});
        writes.push_back({.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                          .dstSet = set,
                          .dstBinding = brdfBinding,
                          .descriptorCount = 1,
                          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                          .pImageInfo = &brdfInfo});
        writes.push_back({.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                          .dstSet = set,
                          .dstBinding = probeSpecularBinding,
                          .descriptorCount = 1,
                          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                          .pImageInfo = &probeInfo});
    }
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    binding.cameraSets.assign(cameraSets.begin(), cameraSets.end());
    binding.environments = ids;
    binding.probeCache = m_probeCache.id;
}

void EnvironmentPass::evictUnusedEnvironments() {
    while (m_environments.size() > maximumCachedEnvironments) {
        auto oldest = m_environments.end();
        for (auto entry = m_environments.begin(); entry != m_environments.end(); ++entry) {
            if (entry->second.lastUsedSerial < m_frameSerial &&
                (oldest == m_environments.end() || entry->second.lastUsedSerial < oldest->second.lastUsedSerial)) {
                oldest = entry;
            }
        }
        if (oldest == m_environments.end()) {
            return;
        }
        // Older frames' descriptor sets may still name it until they retire.
        m_releaseQueue.defer(m_frameSerial, [this, environment = oldest->second] { destroyEnvironment(environment); });
        m_environments.erase(oldest);
    }
}

bool EnvironmentPass::createImage(const VkImageCreateInfo& info, Image& output, const char* operation) {
    const VmaAllocationCreateInfo allocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    return checkVulkan(vmaCreateImage(m_allocator, &info, &allocation, &output.image, &output.allocation, nullptr),
                       operation);
}

VkImageView EnvironmentPass::createView(VkImage image, VkImageViewType type, VkFormat format, uint32_t baseMip,
                                        uint32_t mipCount, uint32_t layerCount) {
    const VkImageViewCreateInfo info{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                     .image = image,
                                     .viewType = type,
                                     .format = format,
                                     .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                          .baseMipLevel = baseMip,
                                                          .levelCount = mipCount,
                                                          .baseArrayLayer = 0,
                                                          .layerCount = layerCount}};
    VkImageView view = VK_NULL_HANDLE;
    if (!checkVulkan(vkCreateImageView(m_device, &info, nullptr, &view), "Create environment image view")) {
        return VK_NULL_HANDLE;
    }
    return view;
}

void EnvironmentPass::retireImage(const Image& image) {
    m_releaseQueue.defer(m_frameSerial, [this, image] {
        if (image.view) {
            vkDestroyImageView(m_device, image.view, nullptr);
        }
        if (image.image) {
            vmaDestroyImage(m_allocator, image.image, image.allocation);
        }
    });
}

void EnvironmentPass::retireBuffer(const Buffer& buffer) {
    m_releaseQueue.defer(m_frameSerial, [this, buffer] {
        if (buffer.buffer) {
            vmaDestroyBuffer(m_allocator, buffer.buffer, buffer.allocation);
        }
    });
}

void EnvironmentPass::retireView(VkImageView view) {
    if (view) {
        m_releaseQueue.defer(m_frameSerial, [this, view] { vkDestroyImageView(m_device, view, nullptr); });
    }
}

void EnvironmentPass::retireComputeSet(VkDescriptorSet set) {
    if (set) {
        m_releaseQueue.defer(m_frameSerial, [this, set] { vkFreeDescriptorSets(m_device, m_computePool, 1, &set); });
    }
}

void EnvironmentPass::destroyEnvironment(const Environment& environment) {
    for (const Image* image : {&environment.radiance, &environment.specular}) {
        if (image->view) {
            vkDestroyImageView(m_device, image->view, nullptr);
        }
        if (image->image) {
            vmaDestroyImage(m_allocator, image->image, image->allocation);
        }
    }
    if (environment.irradiance.buffer) {
        vmaDestroyBuffer(m_allocator, environment.irradiance.buffer, environment.irradiance.allocation);
    }
}

void EnvironmentPass::shutdown() {
    if (!m_device) {
        return;
    }
    // Shutdown runs after the device is idle, so every deferred release may run.
    m_releaseQueue.collect(std::numeric_limits<uint64_t>::max());
    for (const auto& [key, environment] : m_environments) {
        destroyEnvironment(environment);
    }
    m_environments.clear();
    m_failedEnvironments.clear();
    destroyProbeCache(m_probeCache);
    m_probeCache = {};
    m_probeCacheUnavailable = false;
    if (m_brdfTable.view) {
        vkDestroyImageView(m_device, m_brdfTable.view, nullptr);
    }
    if (m_brdfTable.image) {
        vmaDestroyImage(m_allocator, m_brdfTable.image, m_brdfTable.allocation);
    }
    m_brdfTable = {};
    m_brdfReady = false;
    m_equirectangularPipeline.shutdown(m_device);
    m_proceduralPipeline.shutdown(m_device);
    m_prefilterPipeline.shutdown(m_device);
    m_irradiancePipeline.shutdown(m_device);
    m_brdfPipeline.shutdown(m_device);
    m_skyboxPipeline.shutdown(m_device);
    for (VkSampler* sampler : {&m_cubeSampler, &m_equirectangularSampler}) {
        if (*sampler) {
            vkDestroySampler(m_device, *sampler, nullptr);
            *sampler = VK_NULL_HANDLE;
        }
    }
    for (VkDescriptorPool* pool : {&m_computePool, &m_skyPool}) {
        if (*pool) {
            vkDestroyDescriptorPool(m_device, *pool, nullptr);
            *pool = VK_NULL_HANDLE;
        }
    }
    for (VkDescriptorSetLayout* layout : {&m_computeLayout, &m_skyLayout}) {
        if (*layout) {
            vkDestroyDescriptorSetLayout(m_device, *layout, nullptr);
            *layout = VK_NULL_HANDLE;
        }
    }
    m_skySets.clear();
    m_frameBindings.clear();
    m_device = VK_NULL_HANDLE;
}
