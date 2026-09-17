#include "MeshPass.hpp"

#include "../shader/ShaderModule.hpp"
#include "../vulkan/VulkanCheck.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cstring>

namespace {
struct CameraData {
    glm::mat4 viewProjection{1.0f};
};

struct PushConstants {
    glm::mat4 model{1.0f};
    glm::vec4 baseColor{1.0f};
};

void imageBarrier(VkCommandBuffer commandBuffer, VkImage image, VkImageAspectFlags aspect, VkImageLayout oldLayout,
                  VkImageLayout newLayout, VkPipelineStageFlags2 srcStage, VkPipelineStageFlags2 dstStage,
                  VkAccessFlags2 srcAccess, VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = srcStage,
        .srcAccessMask = srcAccess,
        .dstStageMask = dstStage,
        .dstAccessMask = dstAccess,
        .oldLayout = oldLayout,
        .newLayout = newLayout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {
            .aspectMask = aspect, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1}};
    VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
} // namespace
} // namespace

bool MeshPass::init(const VulkanContext& context, VkFormat colorFormat, VkFormat depthFormat, uint32_t frameCount) {
    m_device = context.device();
    m_allocator = context.vmaAllocator();
    m_queue = context.gfxQueue();
    m_queueFamily = context.gfxQueueIndex();
    return createDescriptors(frameCount) && createPipeline(colorFormat, depthFormat);
}

bool MeshPass::createDescriptors(uint32_t frameCount) {
    VkDescriptorSetLayoutBinding binding{.binding = 0,
                                         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                         .descriptorCount = 1,
                                         .stageFlags = VK_SHADER_STAGE_VERTEX_BIT};
    VkDescriptorSetLayoutCreateInfo layoutInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &binding,
    };

    if (!checkVulkan(vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_cameraLayout),
                     "Create camera descriptor layout")) {
        return false;
    }

    VkDescriptorPoolSize poolSize{.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = frameCount};
    VkDescriptorPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = frameCount,
        .poolSizeCount = 1,
        .pPoolSizes = &poolSize,
    };

    if (!checkVulkan(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool),
                     "Create camera descriptor pool")) {
        return false;
    }

    std::vector<VkDescriptorSetLayout> layouts(frameCount, m_cameraLayout);
    std::vector<VkDescriptorSet> sets(frameCount);
    VkDescriptorSetAllocateInfo allocate{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = m_descriptorPool,
        .descriptorSetCount = frameCount,
        .pSetLayouts = layouts.data(),
    };

    if (!checkVulkan(vkAllocateDescriptorSets(m_device, &allocate, sets.data()), "Allocate camera descriptor sets")) {
        return false;
    }

    m_cameras.resize(frameCount);

    for (uint32_t i = 0; i < frameCount; ++i) {
        VkBufferCreateInfo buffer{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                  .size = sizeof(CameraData),
                                  .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
        VmaAllocationCreateInfo allocation{.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                                    VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                                           .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
        VmaAllocationInfo info{};

        if (!checkVulkan(vmaCreateBuffer(m_allocator, &buffer, &allocation, &m_cameras[i].buffer.buffer,
                                         &m_cameras[i].buffer.allocation, &info),
                         "Create camera buffer")) {
            return false;
        }

        m_cameras[i].mapped = info.pMappedData;
        m_cameras[i].descriptor = sets[i];
        VkDescriptorBufferInfo descriptorBuffer{
            .buffer = m_cameras[i].buffer.buffer, .offset = 0, .range = sizeof(CameraData)};
        VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                   .dstSet = sets[i],
                                   .dstBinding = 0,
                                   .descriptorCount = 1,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                   .pBufferInfo = &descriptorBuffer};
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    }

    return true;
}

bool MeshPass::createPipeline(VkFormat colorFormat, VkFormat depthFormat) {
    const std::filesystem::path shaders{ZENITH_SHADER_DIR};
    ShaderModule vertex(m_device);
    ShaderModule fragment(m_device);

    if (!vertex.load(shaders / "mesh.vert.spv") || !fragment.load(shaders / "mesh.frag.spv")) {
        return false;
    }

    return m_pipeline.initGraphicsPipeline(m_device, colorFormat, depthFormat, m_cameraLayout, vertex.handle(),
                                           fragment.handle());
}

bool MeshPass::uploadBuffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage, GpuBuffer& output) {
    GpuBuffer staging;
    VkBufferCreateInfo stagingInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    VmaAllocationCreateInfo stagingAllocation{.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                                       VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                                              .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VmaAllocationInfo mapped{};
    if (!checkVulkan(vmaCreateBuffer(m_allocator, &stagingInfo, &stagingAllocation, &staging.buffer,
                                     &staging.allocation, &mapped),
                     "Create staging buffer")) {
        return false;
    }

    std::memcpy(mapped.pMappedData, data, static_cast<size_t>(size));
    vmaFlushAllocation(m_allocator, staging.allocation, 0, size);
    VkBufferCreateInfo deviceInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo deviceAllocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!checkVulkan(
            vmaCreateBuffer(m_allocator, &deviceInfo, &deviceAllocation, &output.buffer, &output.allocation, nullptr),
            "Create GPU buffer")) {
        vmaDestroyBuffer(m_allocator, staging.buffer, staging.allocation);
        return false;
    }
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                     .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                     .queueFamilyIndex = m_queueFamily};
    if (!checkVulkan(vkCreateCommandPool(m_device, &poolInfo, nullptr, &pool), "Create upload command pool")) {
        vmaDestroyBuffer(m_allocator, staging.buffer, staging.allocation);
        return false;
    }
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                         .commandPool = pool,
                                         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                         .commandBufferCount = 1};
    if (!checkVulkan(vkAllocateCommandBuffers(m_device, &allocate, &command), "Allocate upload command buffer")) {
        vkDestroyCommandPool(m_device, pool, nullptr);
        vmaDestroyBuffer(m_allocator, staging.buffer, staging.allocation);
        return false;
    }
    VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (!checkVulkan(vkBeginCommandBuffer(command, &begin), "Begin mesh upload")) {
        vkDestroyCommandPool(m_device, pool, nullptr);
        vmaDestroyBuffer(m_allocator, staging.buffer, staging.allocation);
        return false;
    }
    VkBufferCopy copy{.size = size};
    vkCmdCopyBuffer(command, staging.buffer, output.buffer, 1, &copy);
    if (!checkVulkan(vkEndCommandBuffer(command), "End mesh upload")) {
        vkDestroyCommandPool(m_device, pool, nullptr);
        vmaDestroyBuffer(m_allocator, staging.buffer, staging.allocation);
        return false;
    }
    VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
    const bool submitted = checkVulkan(vkQueueSubmit(m_queue, 1, &submit, VK_NULL_HANDLE), "Submit mesh upload") &&
                           checkVulkan(vkQueueWaitIdle(m_queue), "Wait for mesh upload");
    vkDestroyCommandPool(m_device, pool, nullptr);
    vmaDestroyBuffer(m_allocator, staging.buffer, staging.allocation);
    return submitted;
}

const MeshPass::GpuMesh* MeshPass::ensureMesh(MeshHandle handle, const AssetManager& assets) {
    const uint64_t key = (static_cast<uint64_t>(handle.generation) << 32) | handle.index;
    if (const auto found = m_meshes.find(key); found != m_meshes.end()) {
        return &found->second;
    }

    const MeshAsset* mesh = assets.mesh(handle);

    if (!mesh || mesh->vertices.empty() || mesh->indices.empty()) {
        return nullptr;
    }

    GpuMesh gpu;

    if (!uploadBuffer(mesh->vertices.data(), mesh->vertices.size() * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      gpu.vertices) ||
        !uploadBuffer(mesh->indices.data(), mesh->indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                      gpu.indices)) {
        if (gpu.vertices.buffer) {
            vmaDestroyBuffer(m_allocator, gpu.vertices.buffer, gpu.vertices.allocation);
        }

        if (gpu.indices.buffer) {
            vmaDestroyBuffer(m_allocator, gpu.indices.buffer, gpu.indices.allocation);
        }

        return nullptr;
    }

    return &m_meshes.emplace(key, gpu).first->second;
}

void MeshPass::beginRendering(VkCommandBuffer commandBuffer, VkImage colorImage, VkImageView colorView,
                              VkImage depthImage, VkImageView depthView, VkExtent2D extent) const {
    imageBarrier(commandBuffer, colorImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    imageBarrier(commandBuffer, depthImage, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT, 0, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo color{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                    .imageView = colorView,
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                    .clearValue = {.color = {.float32 = {0.015f, 0.02f, 0.04f, 1.0f}}}};
    VkRenderingAttachmentInfo depth{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                    .imageView = depthView,
                                    .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                    .clearValue = {.depthStencil = {.depth = 1.0f, .stencil = 0}}};
    VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                              .renderArea = {.offset = {0, 0}, .extent = extent},
                              .layerCount = 1,
                              .colorAttachmentCount = 1,
                              .pColorAttachments = &color,
                              .pDepthAttachment = &depth};
    vkCmdBeginRendering(commandBuffer, &rendering);
}

bool MeshPass::record(VkCommandBuffer commandBuffer, VkImage colorImage, VkImageView colorView, VkImage depthImage,
                      VkImageView depthView, VkExtent2D extent, uint32_t frameIndex, const Scene& scene,
                      const AssetManager& assets) {
    if (frameIndex >= m_cameras.size()) {
        return false;
    }

    glm::mat4 view{1.0f};
    float fov = glm::radians(60.0f);
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
    auto cameras = scene.registry().view<const CameraComponent, const TransformComponent>();

    for (Entity entity : cameras) {
        const auto& camera = cameras.get<const CameraComponent>(entity);

        if (camera.primary) {
            view = glm::inverse(cameras.get<const TransformComponent>(entity).world);
            fov = camera.verticalFovRadians;
            nearPlane = camera.nearPlane;
            farPlane = camera.farPlane;
            break;
        }
    }
    glm::mat4 projection = glm::perspectiveRH_ZO(
        fov, static_cast<float>(extent.width) / static_cast<float>(extent.height), nearPlane, farPlane);
    projection[1][1] *= -1.0f;
    CameraData cameraData{projection * view};
    std::memcpy(m_cameras[frameIndex].mapped, &cameraData, sizeof(cameraData));
    vmaFlushAllocation(m_allocator, m_cameras[frameIndex].buffer.allocation, 0, sizeof(cameraData));

    beginRendering(commandBuffer, colorImage, colorView, depthImage, depthView, extent);
    VkViewport viewport{.x = 0,
                        .y = 0,
                        .width = static_cast<float>(extent.width),
                        .height = static_cast<float>(extent.height),
                        .minDepth = 0,
                        .maxDepth = 1};
    VkRect2D scissor{.offset = {0, 0}, .extent = extent};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline.handle());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline.layout(), 0, 1,
                            &m_cameras[frameIndex].descriptor, 0, nullptr);
    auto renderables = scene.registry().view<const TransformComponent, const MeshRendererComponent>();
    for (Entity entity : renderables) {
        const auto& renderer = renderables.get<const MeshRendererComponent>(entity);
        const MeshAsset* mesh = assets.mesh(renderer.mesh);
        const GpuMesh* gpu = ensureMesh(renderer.mesh, assets);
        if (!mesh || !gpu) {
            vkCmdEndRendering(commandBuffer);
            return false;
        }

        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &gpu->vertices.buffer, &offset);
        vkCmdBindIndexBuffer(commandBuffer, gpu->indices.buffer, 0, VK_INDEX_TYPE_UINT32);
        for (const MeshPrimitive& primitive : mesh->primitives) {
            const MaterialAsset* material = assets.material(primitive.material);
            PushConstants push{renderables.get<const TransformComponent>(entity).world,
                               material ? material->baseColor : glm::vec4(1.0f)};
            vkCmdPushConstants(commandBuffer, m_pipeline.layout(),
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
            vkCmdDrawIndexed(commandBuffer, primitive.indexCount, 1, primitive.firstIndex, 0, 0);
        }
    }

    vkCmdEndRendering(commandBuffer);
    imageBarrier(commandBuffer, colorImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, 0);
    return true;
}

void MeshPass::shutdown(const VulkanContext& context) {
    m_pipeline.shutdown(context.device());
    for (auto& [_, mesh] : m_meshes) {
        vmaDestroyBuffer(context.vmaAllocator(), mesh.vertices.buffer, mesh.vertices.allocation);
        vmaDestroyBuffer(context.vmaAllocator(), mesh.indices.buffer, mesh.indices.allocation);
    }
    m_meshes.clear();
    for (auto& camera : m_cameras) {
        if (camera.buffer.buffer) {
            vmaDestroyBuffer(context.vmaAllocator(), camera.buffer.buffer, camera.buffer.allocation);
        }
    }

    m_cameras.clear();

    if (m_descriptorPool) {
        vkDestroyDescriptorPool(context.device(), m_descriptorPool, nullptr);
    }

    if (m_cameraLayout) {
        vkDestroyDescriptorSetLayout(context.device(), m_cameraLayout, nullptr);
    }

    m_descriptorPool = VK_NULL_HANDLE;
    m_cameraLayout = VK_NULL_HANDLE;
}
