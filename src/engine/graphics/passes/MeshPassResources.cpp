#include "MeshPass.hpp"
#include "MeshPassInternal.hpp"

#include "engine/assets/CompressedTexture.hpp"
#include "engine/assets/PackedVertexStreams.hpp"
#include "engine/graphics/MaterialRecord.hpp"

#include "../vulkan/VulkanCheck.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <string>

using namespace mesh_pass_detail;

bool MeshPass::uploadBufferRegions(std::span<const BufferUploadRegion> regions) {
    const auto uploadBegin = std::chrono::steady_clock::now();
    constexpr VkDeviceSize stagingAlignment = 16;
    std::vector<VkDeviceSize> stagingOffsets;
    stagingOffsets.reserve(regions.size());
    VkDeviceSize stagingBytes = 0;
    for (const BufferUploadRegion& region : regions) {
        stagingOffsets.push_back(stagingBytes);
        stagingBytes += (region.size + stagingAlignment - 1) / stagingAlignment * stagingAlignment;
    }
    std::vector<std::byte> stagingData(static_cast<size_t>(stagingBytes));
    for (size_t index = 0; index < regions.size(); ++index) {
        std::memcpy(stagingData.data() + stagingOffsets[index], regions[index].source,
                    static_cast<size_t>(regions[index].size));
    }
    const VkBuffer staging = m_uploadContext.stage(stagingData.data(), stagingBytes);
    if (!staging) {
        return false;
    }
    const bool submitted = m_uploadContext.submit([&](VkCommandBuffer command) {
        for (size_t index = 0; index < regions.size(); ++index) {
            const VkBufferCopy copy{.srcOffset = stagingOffsets[index],
                                    .dstOffset = regions[index].destinationOffset,
                                    .size = regions[index].size};
            vkCmdCopyBuffer(command, staging, regions[index].destination, 1, &copy);
        }
    });
    if (submitted) {
        m_uploadGpuMilliseconds += m_uploadContext.lastGpuMilliseconds();
    }
    m_uploadMilliseconds +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - uploadBegin).count();
    return submitted;
}

bool MeshPass::createGeometryBuffer(VkDeviceSize size, VkBufferUsageFlags usage, GpuBuffer& output) {
    VkBufferCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const std::span<const uint32_t> queueFamilies = m_uploadContext.sharingQueueFamilies();
    if (queueFamilies.size() > 1) {
        info.sharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = static_cast<uint32_t>(queueFamilies.size());
        info.pQueueFamilyIndices = queueFamilies.data();
    }
    VmaAllocationCreateInfo allocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    return checkVulkan(vmaCreateBuffer(m_allocator, &info, &allocation, &output.buffer, &output.allocation, nullptr),
                       "Create geometry buffer");
}

bool MeshPass::createZeroSkinInfluence() {
    const PackedSkinInfluence zero{};
    if (!createGeometryBuffer(sizeof(zero), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, m_zeroSkinInfluence)) {
        return false;
    }
    const BufferUploadRegion region{.source = &zero, .size = sizeof(zero), .destination = m_zeroSkinInfluence.buffer};
    return uploadBufferRegions({&region, 1});
}

bool MeshPass::ensureGeometryPage(uint32_t page) {
    while (m_geometryPages.size() <= page) {
        const uint32_t pageIndex = static_cast<uint32_t>(m_geometryPages.size());
        const VkDeviceSize vertices = m_geometry->pageCapacity(pageIndex).vertices;
        const VkDeviceSize indices = m_geometry->pageCapacity(pageIndex).indices;
        GeometryPage created{.layout = m_geometry->pageLayout(pageIndex)};
        const bool skinned = created.layout == GeometryLayout::Skinned;
        const bool allocated =
            createGeometryBuffer(vertex_packing::positionStride * vertices, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                 created.positions) &&
            createGeometryBuffer(vertex_packing::attributeStride * vertices, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                 created.attributes) &&
            (!skinned || createGeometryBuffer(vertex_packing::skinStride * vertices, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                              created.skin)) &&
            createGeometryBuffer(sizeof(uint32_t) * indices, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, created.indices);
        if (!allocated) {
            destroyGeometryPage(created);
            return false;
        }
        m_geometryPages.push_back(created);
    }
    return true;
}

void MeshPass::destroyGeometryPage(const GeometryPage& page) {
    for (const GpuBuffer* buffer : {&page.positions, &page.attributes, &page.skin, &page.indices}) {
        if (buffer->buffer) {
            vmaDestroyBuffer(m_allocator, buffer->buffer, buffer->allocation);
        }
    }
}

const MeshPass::GpuMesh* MeshPass::ensureMesh(MeshHandle handle, const AssetManager& assets) {
    const uint64_t key = handleKey(handle);
    if (const auto found = m_meshes.find(key); found != m_meshes.end()) {
        return &found->second;
    }
    const MeshAsset* mesh = assets.mesh(handle);
    if (!mesh || mesh->vertices.empty() || mesh->indices.empty() || m_unpackableMeshes.contains(key)) {
        return nullptr;
    }
    std::string packError;
    const std::optional<PackedVertexStreams> streams = vertex_packing::packVertexStreams(mesh->vertices, packError);
    if (!streams) {
        m_log.error("Mesh vertices cannot be packed for upload: " + packError);
        m_unpackableMeshes.insert(key);
        return nullptr;
    }
    // Matches uploadBufferRegions' 16-byte region alignment. A mesh larger than
    // one staging slot can never upload, so it is not retried as a deferral.
    const auto alignedBytes = [](size_t bytes) { return (static_cast<VkDeviceSize>(bytes) + 15) / 16 * 16; };
    const VkDeviceSize stagingBytes = alignedBytes(streams->positions.size() * sizeof(glm::vec3)) +
                                      alignedBytes(streams->attributes.size() * sizeof(PackedVertexAttributes)) +
                                      alignedBytes(mesh->indices.size() * sizeof(uint32_t)) +
                                      alignedBytes(streams->skin.size() * sizeof(PackedSkinInfluence));
    if (stagingBytes > UploadContext::maximumSlotBytes) {
        m_log.error("Mesh rejected: " + std::to_string(stagingBytes) + " bytes exceed the " +
                    std::to_string(UploadContext::maximumSlotBytes) + "-byte upload limit");
        m_unpackableMeshes.insert(key);
        return nullptr;
    }
    const auto geometry =
        m_geometry->allocate(static_cast<uint32_t>(mesh->vertices.size()), static_cast<uint32_t>(mesh->indices.size()),
                             streams->skinned() ? GeometryLayout::Skinned : GeometryLayout::Static);
    if (!geometry) {
        // Retried every frame while the arena is full; logged once per episode.
        if (!m_geometryArenaFullReported) {
            m_log.warning("Geometry arena could not place a mesh; its draws are deferred until space frees");
            m_geometryArenaFullReported = true;
        }
        m_deferredUploads.markDeferred(DeferredUploadKind::Mesh, key);
        return nullptr;
    }
    m_geometryArenaFullReported = false;
    bool uploaded = ensureGeometryPage(geometry->page);
    if (uploaded) {
        const GeometryPage& page = m_geometryPages[geometry->page];
        const auto firstVertex = static_cast<VkDeviceSize>(geometry->vertexOffset);
        std::vector<BufferUploadRegion> regions{{.source = streams->positions.data(),
                                                 .size = streams->positions.size() * sizeof(glm::vec3),
                                                 .destination = page.positions.buffer,
                                                 .destinationOffset = vertex_packing::positionStride * firstVertex},
                                                {.source = streams->attributes.data(),
                                                 .size = streams->attributes.size() * sizeof(PackedVertexAttributes),
                                                 .destination = page.attributes.buffer,
                                                 .destinationOffset = vertex_packing::attributeStride * firstVertex},
                                                {.source = mesh->indices.data(),
                                                 .size = mesh->indices.size() * sizeof(uint32_t),
                                                 .destination = page.indices.buffer,
                                                 .destinationOffset = sizeof(uint32_t) * geometry->firstIndex}};
        if (streams->skinned()) {
            regions.push_back({.source = streams->skin.data(),
                               .size = streams->skin.size() * sizeof(PackedSkinInfluence),
                               .destination = page.skin.buffer,
                               .destinationOffset = vertex_packing::skinStride * firstVertex});
        }
        uploaded = uploadBufferRegions(regions);
    }
    if (!uploaded) {
        // Typically staging backpressure while a scene loads; retried next frame.
        m_geometry->free(*geometry);
        m_deferredUploads.markDeferred(DeferredUploadKind::Mesh, key);
        return nullptr;
    }
    m_deferredUploads.markUploaded(DeferredUploadKind::Mesh, key);
    return &m_meshes.emplace(key, GpuMesh{*geometry}).first->second;
}

std::string MeshPass::textureRejectionReason(const TextureAsset& texture) const {
    const auto exceedsStaging = [](size_t bytes) { return bytes > UploadContext::maximumSlotBytes; };
    if (texture.blockCompressed()) {
        if (!m_blockCompressionSupported) {
            return "the device cannot sample BC formats";
        }
        if (const AssetPipelineResult valid = validateBlockCompressedTexture(texture); !valid) {
            return valid.error;
        }
        if (exceedsStaging(texture.compressed.size())) {
            return "compressed data exceeds the upload limit";
        }
        return {};
    }
    if (texture.width == 0 || texture.height == 0 ||
        texture.rgba.size() != static_cast<size_t>(texture.width) * texture.height * 4) {
        return "RGBA8 pixel data does not match the texture dimensions";
    }
    if (exceedsStaging(texture.rgba.size())) {
        return "RGBA8 pixel data exceeds the upload limit";
    }
    return {};
}

const MeshPass::GpuTexture* MeshPass::ensureTexture(TextureHandle handle, const AssetManager& assets) {
    const uint64_t key = (static_cast<uint64_t>(handle.generation) << 32) | handle.index;
    if (const auto found = m_textures.find(key); found != m_textures.end()) {
        return &found->second;
    }
    const TextureAsset* texture = assets.texture(handle);
    if (!texture || m_rejectedTextures.contains(key)) {
        return nullptr;
    }
    // Permanent failures are remembered so they are neither retried nor
    // logged every frame; only retryable failures count as deferred.
    if (const std::string reason = textureRejectionReason(*texture); !reason.empty()) {
        m_log.warning("Texture rejected: " + reason + "; materials sample a fallback texture");
        m_rejectedTextures.insert(key);
        return nullptr;
    }
    GpuTexture gpu;
    if (!uploadTexture(*texture, gpu)) {
        m_deferredUploads.markDeferred(DeferredUploadKind::Texture, key);
        return nullptr;
    }
    if (!assignTextureSlots(gpu)) {
        // Nothing has referenced the slots or views yet, so release directly.
        destroyTexture(gpu);
        m_deferredUploads.markDeferred(DeferredUploadKind::Texture, key);
        return nullptr;
    }
    m_deferredUploads.markUploaded(DeferredUploadKind::Texture, key);
    return &m_textures.emplace(key, gpu).first->second;
}

bool MeshPass::assignTextureSlots(GpuTexture& texture) {
    const auto linear = m_textureTable->acquire(texture.view);
    if (linear && !texture.srgbView) {
        // Formats without an sRGB variant (BC5) sample linearly in every slot.
        texture.linearSlot = *linear;
        texture.srgbSlot = *linear;
        m_textureTableFullReported = false;
        return true;
    }
    const auto srgb = linear ? m_textureTable->acquire(texture.srgbView) : std::nullopt;
    if (!linear || !srgb) {
        if (linear) {
            m_textureTable->release(*linear);
        }
        // Retried every frame while the table is full; logged once per episode.
        if (!m_textureTableFullReported) {
            m_log.warning("Bindless texture table is full (" + std::to_string(m_textureTable->capacity()) +
                          " slots); textures are deferred until slots free");
            m_textureTableFullReported = true;
        }
        return false;
    }
    m_textureTableFullReported = false;
    texture.linearSlot = *linear;
    texture.srgbSlot = *srgb;
    return true;
}

void MeshPass::retireTexture(const GpuTexture& texture) {
    // The slot is rewritten in every frame set before that set is bound again;
    // the views die only after all frames that could still bind them retire.
    m_textureTable->release(texture.linearSlot);
    if (texture.srgbSlot != texture.linearSlot) {
        m_textureTable->release(texture.srgbSlot);
    }
    m_releaseQueue.defer(m_frameSerial, [this, texture] { destroyTexture(texture); });
}

void MeshPass::destroyTexture(const GpuTexture& texture) {
    if (texture.srgbView) {
        vkDestroyImageView(m_device, texture.srgbView, nullptr);
    }
    if (texture.view) {
        vkDestroyImageView(m_device, texture.view, nullptr);
    }
    if (texture.image) {
        vmaDestroyImage(m_allocator, texture.image, texture.allocation);
    }
}

const MeshPass::GpuMaterial* MeshPass::ensureMaterial(MaterialHandle handle, const AssetManager& assets) {
    const uint64_t key = handleKey(handle);
    if (const auto found = m_materials.find(key); found != m_materials.end()) {
        return &found->second;
    }
    const MaterialAsset* material = assets.material(handle);
    if (!material) {
        return nullptr;
    }
    const std::optional<uint32_t> index = allocateMaterialIndex();
    if (!index) {
        // Every slot is taken; the caller counts and reports the overflow.
        return nullptr;
    }
    // Upload referenced textures now so frame recording only resolves slots.
    for (const TextureHandle texture : material->textures()) {
        if (texture) {
            ensureTexture(texture, assets);
        }
    }
    return &m_materials.emplace(key, GpuMaterial{*index}).first->second;
}

std::optional<uint32_t> MeshPass::allocateMaterialIndex() {
    if (!m_freeMaterialIndices.empty()) {
        const uint32_t index = m_freeMaterialIndices.back();
        m_freeMaterialIndices.pop_back();
        return index;
    }
    if (m_nextMaterialIndex < maximumMaterials) {
        return m_nextMaterialIndex++;
    }
    return std::nullopt;
}

const MeshPass::GpuMaterial* MeshPass::ensureOverrideMaterial(MaterialHandle material,
                                                              const MaterialOverrideComponent& values) {
    // Bit patterns, so the key orders strictly even for NaN factors.
    const std::array<float, 10> factors{values.baseColorFactor.r, values.baseColorFactor.g, values.baseColorFactor.b,
                                        values.baseColorFactor.a, values.emissive.r,        values.emissive.g,
                                        values.emissive.b,        values.emissiveStrength,  values.roughnessFactor,
                                        values.metallicFactor};
    OverrideMaterialKey key{handleKey(material), {}};
    std::ranges::transform(factors, key.second.begin(), [](float factor) { return std::bit_cast<uint32_t>(factor); });
    if (const auto found = m_overrideMaterials.find(key); found != m_overrideMaterials.end()) {
        found->second.builds[m_sceneKey] = m_renderItemBuild;
        return &found->second.gpu;
    }
    if (m_overrideMaterials.size() >= maximumMaterialOverrides) {
        return nullptr;
    }
    const std::optional<uint32_t> index = allocateMaterialIndex();
    if (!index) {
        return nullptr;
    }
    OverrideMaterial created{
        .gpu = {*index}, .material = material, .values = values, .builds = {{m_sceneKey, m_renderItemBuild}}};
    // The replacement handle is not part of the record; material already is it.
    created.values.material = {};
    return &m_overrideMaterials.emplace(key, created).first->second.gpu;
}

void MeshPass::releaseUnusedOverrideMaterials() {
    std::erase_if(m_overrideMaterials, [&](auto& entry) {
        std::map<uint32_t, uint64_t>& builds = entry.second.builds;
        if (const auto scene = builds.find(m_sceneKey); scene != builds.end() && scene->second != m_renderItemBuild) {
            builds.erase(scene);
        }
        if (!builds.empty()) {
            return false;
        }
        // Records are rewritten for every frame from the live set, so the
        // index can serve another material at once.
        m_freeMaterialIndices.push_back(entry.second.gpu.index);
        return true;
    });
}

void MeshPass::releaseSceneOverrideMaterials(uint32_t sceneKey) {
    std::erase_if(m_overrideMaterials, [&](auto& entry) {
        entry.second.builds.erase(sceneKey);
        if (!entry.second.builds.empty()) {
            return false;
        }
        m_freeMaterialIndices.push_back(entry.second.gpu.index);
        return true;
    });
}

void MeshPass::writeMaterialRecords(uint32_t frameIndex, const AssetManager& assets) {
    auto* records = static_cast<MaterialRecord*>(m_cameras[frameIndex].materialsMapped);
    const auto slotFor = [&](TextureHandle handle, bool srgb, uint32_t fallback) {
        if (!handle) {
            return fallback;
        }
        // A texture missing here failed its first upload, typically because the
        // staging ring was full while a large batch of materials loaded. Retry it
        // every frame until it lands; the fallback covers the frames in between.
        const GpuTexture* texture = ensureTexture(handle, assets);
        if (!texture) {
            return fallback;
        }
        return srgb ? texture->srgbSlot : texture->linearSlot;
    };
    const auto pack = [&](const MaterialAsset& material) {
        return packMaterialRecord(
            material, {.baseColor = slotFor(material.baseColorTexture, true, m_whiteSrgbSlot),
                       .normal = slotFor(material.normalTexture, false, m_normalSlot),
                       .metallicRoughness = slotFor(material.metallicRoughnessTexture, false, m_whiteSlot),
                       .occlusion = slotFor(material.occlusionTexture, false, m_whiteSlot),
                       .emissive = slotFor(material.emissiveTexture, true, m_whiteSrgbSlot),
                       .clearcoat = slotFor(material.clearcoat.texture, false, m_whiteSlot),
                       .clearcoatRoughness = slotFor(material.clearcoat.roughnessTexture, false, m_whiteSlot),
                       .clearcoatNormal = slotFor(material.clearcoat.normalTexture, false, m_normalSlot),
                       .sheenColor = slotFor(material.sheen.colorTexture, true, m_whiteSrgbSlot),
                       .sheenRoughness = slotFor(material.sheen.roughnessTexture, false, m_whiteSlot),
                       .specular = slotFor(material.specular.texture, false, m_whiteSlot),
                       .specularColor = slotFor(material.specular.colorTexture, true, m_whiteSrgbSlot),
                       .transmission = slotFor(material.transmission.texture, false, m_whiteSlot)});
    };
    uint32_t highestIndex = 0;
    for (const auto& [key, gpu] : m_materials) {
        const MaterialAsset* material = assets.material(handleFromKey<MaterialHandle>(key));
        if (!material) {
            continue;
        }
        // Rewritten every frame so edits to factors or texture bindings apply
        // without invalidating anything the GPU might still be reading.
        records[gpu.index] = pack(*material);
        highestIndex = std::max(highestIndex, gpu.index + 1);
    }
    for (const auto& [key, entry] : m_overrideMaterials) {
        // The same, from the material as the override changes it; a record
        // whose material was collected is freed by the next packet build.
        if (const MaterialAsset* material = assets.material(entry.material)) {
            records[entry.gpu.index] = pack(applyMaterialOverride(*material, entry.values));
            highestIndex = std::max(highestIndex, entry.gpu.index + 1);
        }
    }
    if (highestIndex > 0) {
        vmaFlushAllocation(m_allocator, m_cameras[frameIndex].materials.allocation, 0,
                           sizeof(MaterialRecord) * highestIndex);
    }
}

void MeshPass::updateBindlessDescriptor(uint32_t frameIndex) {
    const std::vector<uint32_t> slots = m_textureTable->takePendingWrites(frameIndex);
    if (slots.empty()) {
        return;
    }
    std::vector<VkDescriptorImageInfo> images(slots.size());
    std::vector<VkWriteDescriptorSet> writes(slots.size());
    for (size_t index = 0; index < slots.size(); ++index) {
        images[index] = {.imageView = m_textureTable->view(slots[index]),
                         .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writes[index] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                         .dstSet = m_cameras[frameIndex].bindlessDescriptor,
                         .dstBinding = 1,
                         .dstArrayElement = slots[index],
                         .descriptorCount = 1,
                         .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                         .pImageInfo = &images[index]};
    }
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void MeshPass::bindGeometry(VkCommandBuffer commandBuffer, const GpuMesh& mesh,
                            std::optional<uint32_t>& boundPage) const {
    if (boundPage == mesh.geometry.page) {
        return;
    }
    bindGeometryPage(commandBuffer, mesh.geometry.page);
    boundPage = mesh.geometry.page;
}

void MeshPass::bindGeometryPage(VkCommandBuffer commandBuffer, uint32_t pageIndex) const {
    static_assert(mesh_vertex_binding::position == 0 && mesh_vertex_binding::attributes == 1 &&
                      mesh_vertex_binding::skin == 2,
                  "stream buffers are bound in binding order");
    const GeometryPage& page = m_geometryPages[pageIndex];
    const bool skinned = page.layout == GeometryLayout::Skinned;
    const std::array<VkBuffer, mesh_vertex_binding::count> buffers{
        page.positions.buffer, page.attributes.buffer, skinned ? page.skin.buffer : m_zeroSkinInfluence.buffer};
    const std::array<VkDeviceSize, mesh_vertex_binding::count> offsets{};
    // Static pages repeat one zero influence for every vertex, so the shaders'
    // zero-weight path leaves them unskinned.
    const std::array<VkDeviceSize, mesh_vertex_binding::count> strides{
        vertex_packing::positionStride, vertex_packing::attributeStride, skinned ? vertex_packing::skinStride : 0u};
    vkCmdBindVertexBuffers2(commandBuffer, 0, mesh_vertex_binding::count, buffers.data(), offsets.data(), nullptr,
                            strides.data());
    vkCmdBindIndexBuffer(commandBuffer, page.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
}
