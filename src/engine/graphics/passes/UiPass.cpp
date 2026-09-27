#include "UiPass.hpp"

#include "../vulkan/VulkanCheck.hpp"

#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>

#include <array>
#include <iostream>

namespace {
void reportVulkanError(VkResult result) {
    if (result != VK_SUCCESS) {
        std::cerr << "Dear ImGui Vulkan error: " << result << '\n';
    }
}

void transitionToPresent(VkCommandBuffer commandBuffer, VkImage image) {
    VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                  .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                  .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                  .dstStageMask = VK_PIPELINE_STAGE_2_NONE,
                                  .dstAccessMask = 0,
                                  .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .image = image,
                                  .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                       .baseMipLevel = 0,
                                                       .levelCount = 1,
                                                       .baseArrayLayer = 0,
                                                       .layerCount = 1}};
    VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}
} // namespace

bool UiPass::init(const VulkanContext& context, const Window& window, VkFormat colorFormat, uint32_t imageCount) {
    m_device = context.device();
    constexpr std::array<VkDescriptorPoolSize, 1> poolSizes{{
        {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1024},
    }};
    VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                    .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
                                    .maxSets = 1024,
                                    .poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
                                    .pPoolSizes = poolSizes.data()};
    if (!checkVulkan(vkCreateDescriptorPool(m_device, &pool, nullptr, &m_descriptorPool),
                     "Create UI descriptor pool")) {
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();
    if (!ImGui_ImplSDL3_InitForVulkan(window.handle())) {
        std::cerr << "Could not initialize Dear ImGui's SDL backend\n";
        ImGui::DestroyContext();
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
        m_device = VK_NULL_HANDLE;
        return false;
    }

    VkPipelineRenderingCreateInfo rendering{.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                            .colorAttachmentCount = 1,
                                            .pColorAttachmentFormats = &colorFormat};
    ImGui_ImplVulkan_InitInfo init{};
    init.Instance = context.instance();
    init.PhysicalDevice = context.pDevice();
    init.Device = context.device();
    init.QueueFamily = context.gfxQueueIndex();
    init.Queue = context.gfxQueue();
    init.DescriptorPool = m_descriptorPool;
    init.MinImageCount = imageCount;
    init.ImageCount = imageCount;
    init.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init.UseDynamicRendering = true;
    init.PipelineRenderingCreateInfo = rendering;
    init.CheckVkResultFn = reportVulkanError;
    if (!ImGui_ImplVulkan_Init(&init)) {
        std::cerr << "Could not initialize Dear ImGui's Vulkan backend\n";
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
        m_device = VK_NULL_HANDLE;
        return false;
    }
    m_initialized = true;
    return true;
}

void UiPass::processEvent(const SDL_Event& event) {
    if (m_initialized) {
        ImGui_ImplSDL3_ProcessEvent(&event);
    }
}

void UiPass::beginFrame() {
    if (!m_initialized) {
        return;
    }
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    m_frameActive = true;
}

void UiPass::cancelFrame() {
    if (m_frameActive) {
        ImGui::EndFrame();
        m_frameActive = false;
    }
}

bool UiPass::record(VkCommandBuffer commandBuffer, VkImage image, VkImageView imageView, VkExtent2D extent,
                    bool clearColor) {
    if (!m_initialized) {
        return false;
    }
    ImGui::Render();
    m_frameActive = false;
    VkRenderingAttachmentInfo color{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                    .imageView = imageView,
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = clearColor ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                    .clearValue = {.color = {{0.04f, 0.04f, 0.05f, 1.0f}}}};
    VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                              .renderArea = {.offset = {0, 0}, .extent = extent},
                              .layerCount = 1,
                              .colorAttachmentCount = 1,
                              .pColorAttachments = &color};
    vkCmdBeginRendering(commandBuffer, &rendering);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
    vkCmdEndRendering(commandBuffer);
    transitionToPresent(commandBuffer, image);
    return true;
}

uint64_t UiPass::addTexture(VkSampler sampler, VkImageView view) {
    if (!m_initialized) {
        return 0;
    }
    return reinterpret_cast<uint64_t>(
        ImGui_ImplVulkan_AddTexture(sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
}

void UiPass::removeTexture(uint64_t texture) {
    if (m_initialized && texture != 0) {
        ImGui_ImplVulkan_RemoveTexture(reinterpret_cast<VkDescriptorSet>(texture));
    }
}

bool UiPass::wantsKeyboardCapture() const {
    return m_initialized && ImGui::GetIO().WantCaptureKeyboard;
}

bool UiPass::wantsMouseCapture() const {
    return m_initialized && ImGui::GetIO().WantCaptureMouse;
}

void UiPass::shutdown() {
    if (!m_initialized) {
        return;
    }
    cancelFrame();
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    m_descriptorPool = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
    m_initialized = false;
}
