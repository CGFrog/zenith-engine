#include "../resources/MipChainBlit.hpp"
#include "MeshPass.hpp"
#include "MeshPassInternal.hpp"

#include "engine/assets/TextureMipStreaming.hpp"

#include "../vulkan/VulkanCheck.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>

using namespace mesh_pass_detail;

namespace {
struct BlockCompressedFormats {
    VkFormat linear{VK_FORMAT_UNDEFINED};
    // VK_FORMAT_UNDEFINED when the format has no sRGB variant (BC5).
    VkFormat srgb{VK_FORMAT_UNDEFINED};
};

// ensureTexture already rejected every other block-compressed encoding.
BlockCompressedFormats blockCompressedFormats(const TextureAsset& texture) {
    if (texture.format == TextureFormat::Bc7) {
        return {.linear = VK_FORMAT_BC7_UNORM_BLOCK, .srgb = VK_FORMAT_BC7_SRGB_BLOCK};
    }
    return {.linear = VK_FORMAT_BC5_UNORM_BLOCK};
}

// Streamed textures start with the levels that fit in this many texels per
// side: a few kilobytes that make the texture sampleable at once.
constexpr uint32_t streamedTextureTailExtent = 64;
// Staged mip levels start on a texel-block boundary (16 bytes for BC5/BC7).
constexpr VkDeviceSize mipStagingAlignment = 16;
// Largest batch of streamed levels staged in one frame. Every streamed level
// fits (textureRejectionReason bounds a whole chain by the slot size), and the
// slack absorbs per-level alignment padding.
constexpr uint64_t maximumMipStreamingBatchBytes = UploadContext::maximumSlotBytes / 2;
} // namespace

bool MeshPass::uploadTexture(const TextureAsset& texture, GpuTexture& output) {
    if (texture.blockCompressed()) {
        // Streamed textures start with their mip tail; Off uploads every level now.
        const uint32_t firstMip =
            !textureStreamingActive()
                ? 0u
                : textureMipTailFirstLevel(texture.width, texture.height, static_cast<uint32_t>(texture.mips.size()),
                                           streamedTextureTailExtent);
        return uploadBlockCompressedTexture(texture, output, firstMip);
    }
    const auto uploadBegin = std::chrono::steady_clock::now();
    if (texture.width == 0 || texture.height == 0 ||
        texture.rgba.size() != static_cast<size_t>(texture.width) * texture.height * 4) {
        return false;
    }
    const VkDeviceSize byteCount = texture.rgba.size();
    const VkBuffer staging = m_textureUploadContext.stage(texture.rgba.data(), byteCount);
    if (!staging) {
        return false;
    }

    const uint32_t mipLevels = fullMipChainLength(texture.width, texture.height);
    VkImageCreateInfo imageInfo{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                                .imageType = VK_IMAGE_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM,
                                .extent = {.width = texture.width, .height = texture.height, .depth = 1},
                                .mipLevels = mipLevels,
                                .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                         VK_IMAGE_USAGE_SAMPLED_BIT,
                                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    const std::span<const uint32_t> queueFamilies = m_textureUploadContext.sharingQueueFamilies();
    if (queueFamilies.size() > 1) {
        imageInfo.sharingMode = VK_SHARING_MODE_CONCURRENT;
        imageInfo.queueFamilyIndexCount = static_cast<uint32_t>(queueFamilies.size());
        imageInfo.pQueueFamilyIndices = queueFamilies.data();
    }
    VmaAllocationCreateInfo imageAllocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!checkVulkan(
            vmaCreateImage(m_allocator, &imageInfo, &imageAllocation, &output.image, &output.allocation, nullptr),
            "Create material texture")) {
        return false;
    }
    output.mipCount = mipLevels;

    // A dedicated transfer queue's consumers wait on the upload timeline
    // semaphore, so its final barriers only order the transfer work.
    const MipChainConsumers consumers =
        m_textureUploadContext.usesDedicatedQueue()
            ? MipChainConsumers{.stages = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, .access = 0}
            : MipChainConsumers{.stages = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
    const bool uploaded = m_textureUploadContext.submit([&](VkCommandBuffer command) {
        const VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                            .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                                            .srcAccessMask = 0,
                                            .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                            .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                            .image = output.image,
                                            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                                 .baseMipLevel = 0,
                                                                 .levelCount = mipLevels,
                                                                 .baseArrayLayer = 0,
                                                                 .layerCount = 1}};
        const VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
        vkCmdPipelineBarrier2(command, &dependency);
        VkBufferImageCopy copy{.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                    .mipLevel = 0,
                                                    .baseArrayLayer = 0,
                                                    .layerCount = 1},
                               .imageExtent = {.width = texture.width, .height = texture.height, .depth = 1}};
        vkCmdCopyBufferToImage(command, staging, output.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        recordMipChainBlits(
            command, {.image = output.image, .width = texture.width, .height = texture.height, .mipCount = mipLevels},
            consumers);
    });
    if (uploaded) {
        m_uploadGpuMilliseconds += m_textureUploadContext.lastGpuMilliseconds();
    }
    if (!uploaded) {
        return false;
    }

    VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                               .image = output.image,
                               .viewType = VK_IMAGE_VIEW_TYPE_2D,
                               .format = VK_FORMAT_R8G8B8A8_UNORM,
                               .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                    .baseMipLevel = 0,
                                                    .levelCount = mipLevels,
                                                    .baseArrayLayer = 0,
                                                    .layerCount = 1}};
    if (!checkVulkan(vkCreateImageView(m_device, &view, nullptr, &output.view), "Create material texture view")) {
        return false;
    }
    view.format = VK_FORMAT_R8G8B8A8_SRGB;
    const bool created =
        checkVulkan(vkCreateImageView(m_device, &view, nullptr, &output.srgbView), "Create sRGB material texture view");
    m_uploadMilliseconds +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - uploadBegin).count();
    return created;
}

// Cooked textures carry their full mip chain. The image always holds every
// level, but only [firstMip, mipCount) is uploaded here; streamTextureMips
// adds the rest. BC7 gets UNORM and sRGB views; BC5 has no sRGB variant and
// gets one view.
bool MeshPass::uploadBlockCompressedTexture(const TextureAsset& texture, GpuTexture& output, uint32_t firstMip) {
    const auto uploadBegin = std::chrono::steady_clock::now();
    const BlockCompressedFormats formats = blockCompressedFormats(texture);
    const uint32_t mipLevels = static_cast<uint32_t>(texture.mips.size());
    firstMip = std::min(firstMip, mipLevels - 1);
    VkImageCreateInfo imageInfo{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .flags = formats.srgb ? VkImageCreateFlags{VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT} : 0u,
                                .imageType = VK_IMAGE_TYPE_2D,
                                .format = formats.linear,
                                .extent = {.width = texture.width, .height = texture.height, .depth = 1},
                                .mipLevels = mipLevels,
                                .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    const std::span<const uint32_t> queueFamilies = m_textureUploadContext.sharingQueueFamilies();
    if (queueFamilies.size() > 1) {
        imageInfo.sharingMode = VK_SHARING_MODE_CONCURRENT;
        imageInfo.queueFamilyIndexCount = static_cast<uint32_t>(queueFamilies.size());
        imageInfo.pQueueFamilyIndices = queueFamilies.data();
    }
    VmaAllocationCreateInfo imageAllocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!checkVulkan(
            vmaCreateImage(m_allocator, &imageInfo, &imageAllocation, &output.image, &output.allocation, nullptr),
            "Create block-compressed material texture")) {
        output = {};
        return false;
    }
    output.mipCount = mipLevels;
    output.residentFirstMip = mipLevels;
    // Nothing references the image until this returns, so failures release it directly.
    const auto fail = [&] {
        destroyTexture(output);
        output = {};
        return false;
    };
    const TextureMipRangeUpload range{
        .texture = &texture, .gpu = &output, .firstMip = firstMip, .levelCount = mipLevels - firstMip};
    if (!uploadTextureMipRanges(std::span(&range, 1))) {
        return fail();
    }
    // The copy may still be in flight from here on, so view failures keep the
    // image alive rather than destroying it under the transfer queue.
    if (!createTextureViews(output.image, formats.linear, formats.srgb, firstMip, mipLevels, output.view,
                            output.srgbView)) {
        return false;
    }
    output.residentFirstMip = firstMip;
    m_uploadMilliseconds +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - uploadBegin).count();
    return true;
}

bool MeshPass::uploadTextureMipRanges(std::span<const TextureMipRangeUpload> ranges) {
    std::vector<VkBufferImageCopy> copies;
    VkDeviceSize stagingBytes = 0;
    for (const TextureMipRangeUpload& range : ranges) {
        for (uint32_t level = range.firstMip; level < range.firstMip + range.levelCount; ++level) {
            const TextureMipLevel& mip = range.texture->mips[level];
            copies.push_back({.bufferOffset = stagingBytes,
                              .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                   .mipLevel = level,
                                                   .baseArrayLayer = 0,
                                                   .layerCount = 1},
                              .imageExtent = {.width = mip.width, .height = mip.height, .depth = 1}});
            stagingBytes += (mip.size + mipStagingAlignment - 1) / mipStagingAlignment * mipStagingAlignment;
        }
    }
    if (copies.empty()) {
        return true;
    }
    m_mipStagingBytes.resize(static_cast<size_t>(stagingBytes));
    size_t copyIndex = 0;
    for (const TextureMipRangeUpload& range : ranges) {
        for (uint32_t level = range.firstMip; level < range.firstMip + range.levelCount; ++level) {
            const TextureMipLevel& mip = range.texture->mips[level];
            std::memcpy(m_mipStagingBytes.data() + copies[copyIndex++].bufferOffset,
                        range.texture->compressed.data() + mip.offset, static_cast<size_t>(mip.size));
        }
    }
    const VkBuffer staging = m_textureUploadContext.stage(m_mipStagingBytes.data(), stagingBytes);
    if (!staging) {
        return false;
    }

    // Only the uploaded levels change layout. Resident levels keep being
    // sampled through the current views while this runs, and levels above the
    // range stay UNDEFINED until a later range reaches them.
    const auto levelBarriers = [&](VkImageLayout oldLayout, VkImageLayout newLayout, VkPipelineStageFlags2 sourceStage,
                                   VkAccessFlags2 sourceAccess, VkPipelineStageFlags2 destinationStage,
                                   VkAccessFlags2 destinationAccess) {
        std::vector<VkImageMemoryBarrier2> barriers;
        barriers.reserve(ranges.size());
        for (const TextureMipRangeUpload& range : ranges) {
            barriers.push_back({.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                .srcStageMask = sourceStage,
                                .srcAccessMask = sourceAccess,
                                .dstStageMask = destinationStage,
                                .dstAccessMask = destinationAccess,
                                .oldLayout = oldLayout,
                                .newLayout = newLayout,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = range.gpu->image,
                                .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                     .baseMipLevel = range.firstMip,
                                                     .levelCount = range.levelCount,
                                                     .baseArrayLayer = 0,
                                                     .layerCount = 1}});
        }
        return barriers;
    };
    const auto recordBarriers = [](VkCommandBuffer command, const std::vector<VkImageMemoryBarrier2>& barriers) {
        const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                          .imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size()),
                                          .pImageMemoryBarriers = barriers.data()};
        vkCmdPipelineBarrier2(command, &dependency);
    };
    // Graphics-queue consumers wait on the upload timeline semaphore when the
    // copy runs on a dedicated transfer queue, matching the RGBA8 path.
    const bool dedicatedQueue = m_textureUploadContext.usesDedicatedQueue();
    const bool uploaded = m_textureUploadContext.submit([&](VkCommandBuffer command) {
        recordBarriers(command, levelBarriers(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                              VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                              VK_ACCESS_2_TRANSFER_WRITE_BIT));
        size_t firstCopy = 0;
        for (const TextureMipRangeUpload& range : ranges) {
            vkCmdCopyBufferToImage(command, staging, range.gpu->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   range.levelCount, copies.data() + firstCopy);
            firstCopy += range.levelCount;
        }
        recordBarriers(command,
                       levelBarriers(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                     dedicatedQueue ? VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT
                                                    : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                     dedicatedQueue ? 0 : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT));
    });
    if (uploaded) {
        m_uploadGpuMilliseconds += m_textureUploadContext.lastGpuMilliseconds();
    }
    return uploaded;
}

bool MeshPass::createTextureViews(VkImage image, VkFormat linearFormat, VkFormat srgbFormat, uint32_t firstMip,
                                  uint32_t mipCount, VkImageView& view, VkImageView& srgbView) {
    VkImageViewCreateInfo info{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                               .image = image,
                               .viewType = VK_IMAGE_VIEW_TYPE_2D,
                               .format = linearFormat,
                               .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                    .baseMipLevel = firstMip,
                                                    .levelCount = mipCount - firstMip,
                                                    .baseArrayLayer = 0,
                                                    .layerCount = 1}};
    view = VK_NULL_HANDLE;
    srgbView = VK_NULL_HANDLE;
    if (!checkVulkan(vkCreateImageView(m_device, &info, nullptr, &view), "Create block-compressed texture view")) {
        view = VK_NULL_HANDLE;
        return false;
    }
    if (srgbFormat == VK_FORMAT_UNDEFINED) {
        return true;
    }
    info.format = srgbFormat;
    if (!checkVulkan(vkCreateImageView(m_device, &info, nullptr, &srgbView),
                     "Create sRGB block-compressed texture view")) {
        vkDestroyImageView(m_device, view, nullptr);
        view = VK_NULL_HANDLE;
        srgbView = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

bool MeshPass::publishResidentMips(const TextureAsset& texture, GpuTexture& gpu, uint32_t firstMip) {
    const BlockCompressedFormats formats = blockCompressedFormats(texture);
    VkImageView view = VK_NULL_HANDLE;
    VkImageView srgbView = VK_NULL_HANDLE;
    if (!createTextureViews(gpu.image, formats.linear, formats.srgb, firstMip, gpu.mipCount, view, srgbView)) {
        // The levels stay uploaded but unpublished; a later frame uploads and
        // publishes them again.
        return false;
    }
    // Slots keep their indices, so material records stay valid; each frame
    // set rewrites the slot before it is bound again.
    m_textureTable->replace(gpu.linearSlot, view);
    if (gpu.srgbSlot != gpu.linearSlot) {
        m_textureTable->replace(gpu.srgbSlot, srgbView);
    }
    const VkImageView retiredView = gpu.view;
    const VkImageView retiredSrgbView = gpu.srgbView;
    m_releaseQueue.defer(m_frameSerial, [this, retiredView, retiredSrgbView] {
        vkDestroyImageView(m_device, retiredView, nullptr);
        if (retiredSrgbView) {
            vkDestroyImageView(m_device, retiredSrgbView, nullptr);
        }
    });
    gpu.view = view;
    gpu.srgbView = srgbView;
    gpu.residentFirstMip = firstMip;
    return true;
}

void MeshPass::streamTextureMips(const AssetManager& assets, const glm::vec3& cameraPosition) {
    std::vector<uint64_t> pendingKeys;
    for (const auto& [key, gpu] : m_textures) {
        if (gpu.residentFirstMip > 0) {
            pendingKeys.push_back(key);
        }
    }
    m_textureStreamedBytes = 0;
    if (pendingKeys.empty()) {
        return;
    }
    const auto streamBegin = std::chrono::steady_clock::now();
    std::unordered_map<uint64_t, StreamPriority> priorities;
    for (const uint64_t key : pendingKeys) {
        priorities.emplace(key, StreamPriority{});
    }
    for (const RenderItem& item : m_renderItems) {
        if (!item.material) {
            continue;
        }
        const StreamPriority use =
            textureStreamPriority(glm::distance(item.boundsCenter, cameraPosition), item.boundsRadius);
        for (const TextureHandle texture :
             {item.material->baseColorTexture, item.material->normalTexture, item.material->metallicRoughnessTexture,
              item.material->occlusionTexture, item.material->emissiveTexture}) {
            if (const auto found = texture ? priorities.find(handleKey(texture)) : priorities.end();
                found != priorities.end()) {
                found->second = mostDemandingStreamPriority(found->second, use);
            }
        }
    }

    std::vector<std::vector<uint64_t>> levelBytes;
    levelBytes.reserve(pendingKeys.size());
    std::vector<TextureMipStreamCandidate> candidates;
    candidates.reserve(pendingKeys.size());
    for (const uint64_t key : pendingKeys) {
        const TextureAsset* texture = assets.texture(handleFromKey<TextureHandle>(key));
        const GpuTexture& gpu = m_textures.at(key);
        if (!texture || texture->mips.size() != gpu.mipCount) {
            continue;
        }
        std::vector<uint64_t>& bytes = levelBytes.emplace_back();
        for (const TextureMipLevel& mip : texture->mips) {
            bytes.push_back(mip.size);
        }
        candidates.push_back({.key = key,
                              .residency = {.mipCount = gpu.mipCount, .residentFirstMip = gpu.residentFirstMip},
                              .levelBytes = bytes,
                              .priority = priorities.at(key)});
    }
    // Off still finishes textures that were streaming when it was selected.
    const uint64_t budget = textureStreamingActive()
                                ? std::min<uint64_t>(m_textureStreamingBytesPerFrame, maximumMipStreamingBatchBytes)
                                : maximumMipStreamingBatchBytes;
    const std::vector<TextureMipUpload> plan = planTextureMipUploads(candidates, budget);
    std::vector<TextureMipRangeUpload> ranges;
    ranges.reserve(plan.size());
    for (const TextureMipUpload& upload : plan) {
        ranges.push_back({.texture = assets.texture(handleFromKey<TextureHandle>(upload.key)),
                          .gpu = &m_textures.at(upload.key),
                          .firstMip = upload.firstMip,
                          .levelCount = upload.levelCount});
    }
    // Staging backpressure leaves everything as it was; the plan reruns next frame.
    if (!ranges.empty() && uploadTextureMipRanges(ranges)) {
        for (size_t index = 0; index < ranges.size(); ++index) {
            if (publishResidentMips(*ranges[index].texture, *ranges[index].gpu, ranges[index].firstMip)) {
                m_textureStreamedBytes += plan[index].bytes;
            }
        }
    }
    m_uploadMilliseconds +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - streamBegin).count();
}

bool MeshPass::completeTextureMips(uint64_t key, const AssetManager& assets) {
    GpuTexture& gpu = m_textures.at(key);
    const TextureAsset* texture = assets.texture(handleFromKey<TextureHandle>(key));
    if (gpu.residentFirstMip == 0) {
        return true;
    }
    if (!texture || texture->mips.size() != gpu.mipCount) {
        return false;
    }
    const TextureMipRangeUpload range{
        .texture = texture, .gpu = &gpu, .firstMip = 0, .levelCount = gpu.residentFirstMip};
    return uploadTextureMipRanges(std::span(&range, 1)) && publishResidentMips(*texture, gpu, 0);
}

void MeshPass::recordTextureStreaming(const AssetManager& assets, RenderStats& stats) const {
    stats.textureStreamedBytes = m_textureStreamedBytes;
    for (const auto& [key, gpu] : m_textures) {
        stats.textureResidentMips += gpu.mipCount - gpu.residentFirstMip;
        stats.textureRequestedMips += gpu.mipCount;
        if (gpu.residentFirstMip == 0) {
            continue;
        }
        ++stats.texturesStreaming;
        if (const TextureAsset* texture = assets.texture(handleFromKey<TextureHandle>(key))) {
            for (uint32_t level = 0; level < gpu.residentFirstMip && level < texture->mips.size(); ++level) {
                stats.textureStreamingBacklogBytes += texture->mips[level].size;
            }
        }
    }
}
