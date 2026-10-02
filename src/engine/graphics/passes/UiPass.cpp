#include "UiPass.hpp"

#include "../vulkan/VulkanCheck.hpp"

#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <vector>

namespace {
void reportVulkanError(VkResult result) {
    if (result != VK_SUCCESS) {
        std::cerr << "Dear ImGui Vulkan error: " << result << '\n';
    }
}

bool isSrgbFormat(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        return true;
    default:
        return false;
    }
}

// Converts ImGui's sRGB vertex colors to linear so that the hardware sRGB
// encode on write reproduces the authored color. Alpha is already linear.
void linearizeVertexColors(ImDrawData& drawData) {
    static const std::array<uint8_t, 256> toLinear = [] {
        std::array<uint8_t, 256> table{};
        for (size_t index = 0; index < table.size(); ++index) {
            const float encoded = static_cast<float>(index) / 255.0f;
            const float linear = encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
            table[index] = static_cast<uint8_t>(std::lround(linear * 255.0f));
        }
        return table;
    }();
    for (ImDrawList* list : drawData.CmdLists) {
        for (ImDrawVert& vertex : list->VtxBuffer) {
            const ImU32 color = vertex.col;
            vertex.col = (static_cast<ImU32>(toLinear[(color >> IM_COL32_R_SHIFT) & 0xffu]) << IM_COL32_R_SHIFT) |
                         (static_cast<ImU32>(toLinear[(color >> IM_COL32_G_SHIFT) & 0xffu]) << IM_COL32_G_SHIFT) |
                         (static_cast<ImU32>(toLinear[(color >> IM_COL32_B_SHIFT) & 0xffu]) << IM_COL32_B_SHIFT) |
                         (color & IM_COL32_A_MASK);
        }
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
    m_linearizeVertexColors = isSrgbFormat(colorFormat);
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

void UiPass::syncFonts(const AssetManager& assets) {
    if (!m_initialized) {
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    bool added = false;
    for (FontHandle handle : assets.fontHandles()) {
        const uint64_t key = (static_cast<uint64_t>(handle.generation) << 32u) | handle.index;
        if (m_fonts.contains(key)) {
            continue;
        }
        const FontAsset* asset = assets.font(handle);
        if (!asset || asset->bytes.empty()) {
            continue;
        }
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        ImFont* font = io.Fonts->AddFontFromMemoryTTF(const_cast<uint8_t*>(asset->bytes.data()),
                                                      static_cast<int>(asset->bytes.size()), 36.0f, &config);
        if (font) {
            m_fonts.emplace(key, font);
            added = true;
        }
    }
    if (added) {
        ImGui_ImplVulkan_CreateFontsTexture();
    }
}

void UiPass::drawGameUi(const GameUiContext& gameUi, const std::function<uint64_t(TextureHandle)>& textureResolver) {
    if (!m_frameActive) {
        return;
    }
    std::vector<const UiCommand*> commands;
    commands.reserve(gameUi.commands().size());
    for (const UiCommand& command : gameUi.commands()) {
        commands.push_back(&command);
    }
    std::stable_sort(commands.begin(), commands.end(),
                     [](const UiCommand* left, const UiCommand* right) { return left->drawOrder < right->drawOrder; });

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    for (const UiCommand* command : commands) {
        if (command->clip) {
            const UiRect& clip = *command->clip;
            drawList->PushClipRect({clip.position.x, clip.position.y},
                                   {clip.position.x + clip.size.x, clip.position.y + clip.size.y}, true);
        }
        std::visit(
            [&](const auto& payload) {
                using Payload = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<Payload, UiPanelCommand>) {
                    const ImU32 color = ImGui::ColorConvertFloat4ToU32(
                        {payload.style.color.r, payload.style.color.g, payload.style.color.b, payload.style.color.a});
                    drawList->AddRectFilled(
                        {payload.rect.position.x, payload.rect.position.y},
                        {payload.rect.position.x + payload.rect.size.x, payload.rect.position.y + payload.rect.size.y},
                        color, payload.style.cornerRadius);
                } else if constexpr (std::is_same_v<Payload, UiTextCommand>) {
                    ImFont* font = ImGui::GetFont();
                    if (payload.font) {
                        const uint64_t key =
                            (static_cast<uint64_t>(payload.font.generation) << 32u) | payload.font.index;
                        if (const auto found = m_fonts.find(key); found != m_fonts.end()) {
                            font = static_cast<ImFont*>(found->second);
                        }
                    }
                    const ImU32 color = ImGui::ColorConvertFloat4ToU32(
                        {payload.color.r, payload.color.g, payload.color.b, payload.color.a});
                    drawList->AddText(font, payload.size, {payload.position.x, payload.position.y}, color,
                                      payload.text.c_str());
                } else if constexpr (std::is_same_v<Payload, UiImageCommand>) {
                    const uint64_t texture = textureResolver(payload.texture);
                    if (texture != 0) {
                        const ImU32 tint = ImGui::ColorConvertFloat4ToU32(
                            {payload.tint.r, payload.tint.g, payload.tint.b, payload.tint.a});
                        drawList->AddImage(static_cast<ImTextureID>(texture),
                                           {payload.rect.position.x, payload.rect.position.y},
                                           {payload.rect.position.x + payload.rect.size.x,
                                            payload.rect.position.y + payload.rect.size.y},
                                           {0.0f, 0.0f}, {1.0f, 1.0f}, tint);
                    }
                }
            },
            command->payload);
        if (command->clip) {
            drawList->PopClipRect();
        }
    }
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
    if (m_linearizeVertexColors) {
        linearizeVertexColors(*ImGui::GetDrawData());
    }
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
    m_fonts.clear();
}
