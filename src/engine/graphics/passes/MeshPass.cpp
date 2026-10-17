#include "MeshPass.hpp"
#include "MeshPassInternal.hpp"

#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <array>
#include <limits>

using namespace mesh_pass_detail;

namespace {
constexpr GeometryPageCapacity defaultGeometryPage{.vertices = 1u << 19, .indices = 1u << 21};
} // namespace

bool MeshPass::init(const VulkanContext& context, VkFormat colorFormat, VkFormat depthFormat, uint32_t frameCount,
                    VkExtent2D extent, VkImageView depthView) {
    m_log = context.log();
    m_context = &context;
    m_device = context.device();
    m_pipelineCache = context.pipelineCache();
    m_allocator = context.vmaAllocator();
    m_blockCompressionSupported = context.blockCompressionSupported();
    m_packetCachingEnabled = SDL_getenv("ZENITH_DISABLE_RENDER_PACKET_CACHE") == nullptr;
    m_outputFormat = colorFormat;
    m_renderItems.reserve(maximumObjectsPerFrame);
    m_shadowVisibleItems.reserve(maximumObjectsPerFrame);
    m_objectItems.reserve(maximumObjectsPerFrame);
    m_gpuRecords.reserve(maximumObjectsPerFrame);
    m_gpuHandleItems.reserve(maximumObjectsPerFrame);
    m_gpuDrivenObjects.reserve(maximumObjectsPerFrame);
    m_gpuDrivenBatches.reserve(maximumObjectsPerFrame);
    m_gpuDrivenCandidates.reserve(maximumObjectsPerFrame);
    m_gpuDrivenKeys.reserve(maximumObjectsPerFrame);
    if (!m_uploadContext.init(context, true) || !m_textureUploadContext.init(context, false) ||
        !createDescriptors(context, frameCount)) {
        return false;
    }
    TextureAsset white{.width = 1, .height = 1, .rgba = {255, 255, 255, 255}};
    TextureAsset normal{.width = 1, .height = 1, .rgba = {128, 128, 255, 255}};
    m_geometry = std::make_unique<GeometryArena>(defaultGeometryPage);
    if (!createZeroSkinInfluence() || !uploadTexture(white, m_whiteTexture) ||
        !uploadTexture(normal, m_normalTexture) || !createBindlessDescriptors(context, frameCount) ||
        !createShadowResources(depthFormat) ||
        !m_localShadows.init(context, frameCount, depthFormat, m_cameraLayout, m_bindlessLayout) ||
        !createPipeline(PostProcessPass::sceneFormat, depthFormat) ||
        !m_environmentPass.init(context, m_cameraLayout, PostProcessPass::sceneFormat, PostProcessPass::motionFormat,
                                depthFormat, frameCount) ||
        !m_particles.init(context, frameCount, m_cameraLayout, PostProcessPass::sceneFormat,
                          PostProcessPass::motionFormat, depthFormat) ||
        !m_idBuffer.init(context, m_pipelineCache, m_cameraLayout, depthFormat)) {
        return false;
    }
    // The default view draws through the frame's camera sets; its bindings are
    // written before the GPU-driven sets copy the shared ones.
    m_views.clear();
    m_views.push_back(std::make_unique<ViewState>());
    ViewState& defaultView = *m_views.front();
    defaultView.sharesFrameSets = true;
    if (!initViewPasses(context, defaultView, extent, depthView)) {
        return false;
    }
    VkPhysicalDeviceVulkan12Features features12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &features12};
    vkGetPhysicalDeviceFeatures2(context.pDevice(), &features);
    const GpuDrivenLimits limits{.maximumObjects = maximumObjectsPerFrame,
                                 .maximumInstances = maximumGpuDrivenInstances,
                                 .maximumBatches = maximumGpuDrivenInstances,
                                 .maximumGroups = maximumGpuDrawGroups,
                                 // Shadow cascades cull into their own regions.
                                 .frustumViews = maximumShadowCascades};
    for (uint32_t frame = 0; frame < frameCount; ++frame) {
        m_localShadows.writeDescriptors(frame, {&m_cameras[frame].descriptor, 1});
    }
    m_gpuDrivenSupported = m_gpuDrivenPass.init(context, frameCount, defaultView.depthPyramid, limits,
                                                features12.drawIndirectCount == VK_TRUE);
    if (m_gpuDrivenSupported && !createGpuDrivenDescriptors()) {
        return false;
    }
    writeAmbientOcclusionDescriptors(defaultView);
    writeTransmissionDescriptors(defaultView);
    if (!m_irradianceVolume.init(context, frameCount)) {
        return false;
    }
    for (uint32_t frame = 0; frame < frameCount; ++frame) {
        const std::array<VkDescriptorSet, 2> sets{m_cameras[frame].descriptor, m_cameras[frame].gpuDrivenDescriptor};
        m_irradianceVolume.writeDescriptors(frame, sets);
    }
    return true;
}

void MeshPass::recordDeferredUploads(const RenderSettings& settings, RenderStats& stats) {
    m_deferredUploads.setConfiguration({.warningFrames = settings.deferredUploadWarningFrames});
    const DeferredUploadFrameSummary deferred = m_deferredUploads.endFrame();
    stats.deferredMeshUploads = deferred.deferredMeshes;
    stats.deferredTextureUploads = deferred.deferredTextures;
    for (const DeferredUploadWarning& warning : deferred.overdue) {
        m_log.warning(describeDeferredUpload(warning));
    }
}

void MeshPass::pruneUnusedAssets(const AssetManager& assets) {
    // Asset leases keep an asset alive until every frame that drew it retired,
    // so geometry ranges and material indices can be reused immediately.
    std::erase_if(m_meshes, [&](const auto& entry) {
        if (assets.mesh(handleFromKey<MeshHandle>(entry.first)) != nullptr) {
            return false;
        }
        m_geometry->free(entry.second.geometry);
        return true;
    });
    std::erase_if(m_unpackableMeshes,
                  [&](uint64_t key) { return assets.mesh(handleFromKey<MeshHandle>(key)) == nullptr; });
    std::erase_if(m_rejectedTextures,
                  [&](uint64_t key) { return assets.texture(handleFromKey<TextureHandle>(key)) == nullptr; });
    std::erase_if(m_textures, [&](const auto& entry) {
        if (assets.texture(handleFromKey<TextureHandle>(entry.first)) != nullptr) {
            return false;
        }
        retireTexture(entry.second);
        return true;
    });
    std::erase_if(m_materials, [&](const auto& entry) {
        if (assets.material(handleFromKey<MaterialHandle>(entry.first)) != nullptr) {
            return false;
        }
        m_freeMaterialIndices.push_back(entry.second.index);
        return true;
    });
}

bool MeshPass::uiTexture(TextureHandle handle, const AssetManager& assets, VkSampler& sampler, VkImageView& view) {
    const GpuTexture* texture = ensureTexture(handle, assets);
    // UI descriptor sets keep the view they were given, so a UI image gets
    // every level now and its views never change afterwards.
    if (!texture || !completeTextureMips(handleKey(handle), assets)) {
        return false;
    }
    sampler = m_sampler;
    view = texture->srgbView ? texture->srgbView : texture->view;
    return true;
}

void MeshPass::shutdown(const VulkanContext& context) {
    if (m_gpuDrivenSupported) {
        m_gpuDrivenPass.shutdown(context);
    }
    m_gpuDrivenSupported = false;
    for (std::unique_ptr<ViewState>& view : m_views) {
        if (view) {
            destroyViewState(context, *view);
        }
    }
    m_views.clear();
    m_uploadContext.shutdown();
    m_textureUploadContext.shutdown();
    for (auto& [variant, pipelines] : m_meshPipelines) {
        pipelines->shutdown(context.device());
    }
    m_meshPipelines.clear();
    m_shadowPipeline.shutdown(context.device());
    m_selectionMaskPipeline.shutdown(context.device());
    m_outlinePipeline.shutdown(context.device());
    m_environmentPass.shutdown();
    m_irradianceVolume.shutdown();
    m_depthNormalPipeline.shutdown(context.device());
    m_depthNormalDoubleSidedPipeline.shutdown(context.device());
    m_particles.shutdown();
    m_idBuffer.shutdown();
    m_particleSpriteViews.fill(VK_NULL_HANDLE);
    m_meshes.clear();
    m_unpackableMeshes.clear();
    m_rejectedTextures.clear();
    m_deferredUploads = DeferredUploadTracker{};
    m_meshUploadsPending = false;
    for (const GeometryPage& page : m_geometryPages) {
        destroyGeometryPage(page);
    }
    m_geometryPages.clear();
    if (m_zeroSkinInfluence.buffer) {
        vmaDestroyBuffer(context.vmaAllocator(), m_zeroSkinInfluence.buffer, m_zeroSkinInfluence.allocation);
    }
    m_zeroSkinInfluence = {};
    m_geometry.reset();
    // Shutdown runs after the device is idle, so deferred releases may run now.
    m_releaseQueue.collect(std::numeric_limits<uint64_t>::max());
    // After the queue ran: it may hold atlases this pass retired.
    m_localShadows.shutdown();
    m_changedShadowCasters.clear();
    for (auto& [_, texture] : m_textures) {
        destroyTexture(texture);
    }
    m_textures.clear();
    m_textureTable.reset();
    m_overrideMaterials.clear();
    m_freeMaterialIndices.clear();
    m_nextMaterialIndex = 0;
    const auto destroyOwnedTexture = [&](GpuTexture& texture) {
        if (texture.srgbView) {
            vkDestroyImageView(context.device(), texture.srgbView, nullptr);
        }
        if (texture.view) {
            vkDestroyImageView(context.device(), texture.view, nullptr);
        }
        if (texture.image) {
            vmaDestroyImage(context.vmaAllocator(), texture.image, texture.allocation);
        }
        texture = {};
    };
    destroyOwnedTexture(m_whiteTexture);
    destroyOwnedTexture(m_normalTexture);
    destroyDirectionalShadowMap(m_shadowMap);
    m_frameShadowCasters.reset();
    m_localShadowCasterObjects.reset();
    m_materials.clear();
    for (auto& camera : m_cameras) {
        if (camera.buffer.buffer) {
            vmaDestroyBuffer(context.vmaAllocator(), camera.buffer.buffer, camera.buffer.allocation);
        }
        if (camera.objects.buffer) {
            vmaDestroyBuffer(context.vmaAllocator(), camera.objects.buffer, camera.objects.allocation);
        }
        if (camera.skinPalettes.buffer) {
            vmaDestroyBuffer(context.vmaAllocator(), camera.skinPalettes.buffer, camera.skinPalettes.allocation);
        }
        if (camera.previousSkinPalettes.buffer) {
            vmaDestroyBuffer(context.vmaAllocator(), camera.previousSkinPalettes.buffer,
                             camera.previousSkinPalettes.allocation);
        }
        if (camera.materials.buffer) {
            vmaDestroyBuffer(context.vmaAllocator(), camera.materials.buffer, camera.materials.allocation);
        }
    }

    m_cameras.clear();

    if (m_descriptorPool) {
        vkDestroyDescriptorPool(context.device(), m_descriptorPool, nullptr);
    }
    if (m_bindlessPool) {
        vkDestroyDescriptorPool(context.device(), m_bindlessPool, nullptr);
    }

    if (m_cameraLayout) {
        vkDestroyDescriptorSetLayout(context.device(), m_cameraLayout, nullptr);
    }
    if (m_bindlessLayout) {
        vkDestroyDescriptorSetLayout(context.device(), m_bindlessLayout, nullptr);
    }
    if (m_sampler) {
        vkDestroySampler(context.device(), m_sampler, nullptr);
    }
    if (m_shadowSampler) {
        vkDestroySampler(context.device(), m_shadowSampler, nullptr);
    }

    m_descriptorPool = VK_NULL_HANDLE;
    m_bindlessPool = VK_NULL_HANDLE;
    m_cameraLayout = VK_NULL_HANDLE;
    m_bindlessLayout = VK_NULL_HANDLE;
    m_sampler = VK_NULL_HANDLE;
    m_shadowSampler = VK_NULL_HANDLE;
}
