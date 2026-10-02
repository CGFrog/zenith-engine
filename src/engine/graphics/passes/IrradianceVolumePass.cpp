#include "IrradianceVolumePass.hpp"

#include "../IrradianceVolume.hpp"
#include "../vulkan/VulkanCheck.hpp"

#include <algorithm>

namespace {
// vkCmdUpdateBuffer accepts at most 65536 bytes per command.
constexpr VkDeviceSize maximumUpdateBytes = 65536;
} // namespace

bool IrradianceVolumePass::init(const VulkanContext& context, uint32_t frameCount) {
    m_device = context.device();
    m_allocator = context.vmaAllocator();
    m_boundBuffers.assign(frameCount, VK_NULL_HANDLE);
    // A one-probe placeholder keeps binding 8 valid before any volume is bound;
    // shaders never read it because no volume is enabled in CameraData.
    m_capacity = sizeof(irradiance_volume::GpuProbe);
    return allocate(m_capacity, m_buffer);
}

void IrradianceVolumePass::shutdown() {
    m_releases.collect(UINT64_MAX);
    destroy(m_buffer);
    m_buffer = {};
    m_capacity = 0;
    m_uploaded.reset();
    m_uploadedBytes.clear();
    m_boundBuffers.clear();
}

bool IrradianceVolumePass::allocate(VkDeviceSize size, Buffer& output) const {
    const VkBufferCreateInfo buffer{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                    .size = size,
                                    .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo allocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    return checkVulkan(vmaCreateBuffer(m_allocator, &buffer, &allocation, &output.buffer, &output.allocation, nullptr),
                       "Create irradiance volume probe buffer");
}

void IrradianceVolumePass::destroy(const Buffer& buffer) const {
    if (buffer.buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_allocator, buffer.buffer, buffer.allocation);
    }
}

void IrradianceVolumePass::writeDescriptors(uint32_t frameIndex, std::span<const VkDescriptorSet> cameraSets) {
    const VkDescriptorBufferInfo info{.buffer = m_buffer.buffer, .offset = 0, .range = VK_WHOLE_SIZE};
    std::vector<VkWriteDescriptorSet> writes;
    for (VkDescriptorSet set : cameraSets) {
        if (set == VK_NULL_HANDLE) {
            continue;
        }
        writes.push_back({.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                          .dstSet = set,
                          .dstBinding = binding,
                          .descriptorCount = 1,
                          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                          .pBufferInfo = &info});
    }
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    m_boundBuffers[frameIndex] = m_buffer.buffer;
}

void IrradianceVolumePass::recordUpload(VkCommandBuffer commandBuffer, std::span<const std::byte> bytes) const {
    // Earlier frames may still read the buffer in fragment shaders; the barrier
    // orders those reads before the overwrite (write-after-read).
    VkBufferMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                                   .srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                   .srcAccessMask = VK_ACCESS_2_NONE,
                                   .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                   .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .buffer = m_buffer.buffer,
                                   .offset = 0,
                                   .size = VK_WHOLE_SIZE};
    VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
    for (VkDeviceSize offset = 0; offset < bytes.size(); offset += maximumUpdateBytes) {
        const VkDeviceSize size = std::min<VkDeviceSize>(maximumUpdateBytes, bytes.size() - offset);
        vkCmdUpdateBuffer(commandBuffer, m_buffer.buffer, offset, size, bytes.data() + offset);
    }
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

bool IrradianceVolumePass::prepare(VkCommandBuffer commandBuffer, uint32_t frameIndex, uint64_t frameSerial,
                                   const std::shared_ptr<const IrradianceVolumeData>& data,
                                   std::span<const VkDescriptorSet> cameraSets) {
    const uint64_t frameCount = m_boundBuffers.size();
    if (frameSerial > frameCount) {
        m_releases.collect(frameSerial - frameCount);
    }
    if (data && data != m_uploaded) {
        const std::vector<irradiance_volume::GpuProbe> probes = irradiance_volume::packProbes(*data);
        const auto bytes = std::as_bytes(std::span(probes));
        // Reloading a scene (undo, play mode) yields new data objects with the
        // same contents; those keep the buffer as is.
        if (!std::ranges::equal(bytes, m_uploadedBytes)) {
            if (bytes.size() > m_capacity) {
                Buffer replacement;
                if (!allocate(bytes.size(), replacement)) {
                    return false;
                }
                // Frames recorded up to this one may still reference the old buffer.
                m_releases.defer(frameSerial, [this, retired = m_buffer] { destroy(retired); });
                m_buffer = replacement;
                m_capacity = bytes.size();
            }
            recordUpload(commandBuffer, bytes);
            m_uploadedBytes.assign(bytes.begin(), bytes.end());
        }
        m_uploaded = data;
    }
    if (m_boundBuffers[frameIndex] != m_buffer.buffer) {
        writeDescriptors(frameIndex, cameraSets);
    }
    return true;
}
