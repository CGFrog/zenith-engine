#include "MeshPass.hpp"

#include "engine/resources/ResourceLocator.hpp"

#include "../shader/ShaderModule.hpp"

#include <cstddef>

bool MeshPass::createPipeline(VkFormat colorFormat, VkFormat depthFormat) {
    const std::filesystem::path shaders = shaderRoot();
    ShaderModule shadowVertex(m_device);
    ShaderModule shadowFragment(m_device);
    ShaderModule selectionMaskVertex(m_device);
    ShaderModule selectionMaskFragment(m_device);
    ShaderModule outlineVertex(m_device);
    ShaderModule outlineFragment(m_device);
    ShaderModule depthNormalVertex(m_device);
    ShaderModule depthNormalFragment(m_device);

    if (!shadowVertex.load(shaders / "shadow.vert.spv") || !shadowFragment.load(shaders / "shadow.frag.spv") ||
        !outlineVertex.load(shaders / "outline.vert.spv") || !outlineFragment.load(shaders / "outline.frag.spv") ||
        !selectionMaskVertex.load(shaders / "selection_mask.vert.spv") ||
        !selectionMaskFragment.load(shaders / "selection_mask.frag.spv") ||
        !depthNormalVertex.load(shaders / "depth_normal.vert.spv") ||
        !depthNormalFragment.load(shaders / "depth_normal.frag.spv")) {
        return false;
    }

    m_sceneColorFormat = colorFormat;
    m_sceneDepthFormat = depthFormat;
    const auto createDepthNormal = [&](GraphicsPipeline& pipeline, bool doubleSided) {
        return pipeline.initDepthNormalPipeline(
            m_device, PostProcessPass::normalFormat, PostProcessPass::specularFormat, depthFormat, m_cameraLayout,
            m_bindlessLayout, depthNormalVertex.handle(), depthNormalFragment.handle(), m_pipelineCache, doubleSided);
    };
    if (!createDepthNormal(m_depthNormalPipeline, false) ||
        !createDepthNormal(m_depthNormalDoubleSidedPipeline, true)) {
        return false;
    }
    return m_shadowPipeline.initShadowPipeline(m_device, depthFormat, m_cameraLayout, m_bindlessLayout,
                                               shadowVertex.handle(), shadowFragment.handle(), m_pipelineCache) &&
           m_selectionMaskPipeline.initSelectionMaskPipeline(m_device, colorFormat, PostProcessPass::motionFormat,
                                                             depthFormat, m_cameraLayout, selectionMaskVertex.handle(),
                                                             selectionMaskFragment.handle(), m_pipelineCache) &&
           m_outlinePipeline.initOutlinePipeline(m_device, colorFormat, PostProcessPass::motionFormat, depthFormat,
                                                 m_cameraLayout, outlineVertex.handle(), outlineFragment.handle(),
                                                 m_pipelineCache);
}

const MeshPass::MeshPipelineSet* MeshPass::meshPipelines(MeshShadingVariant variant) {
    if (const auto found = m_meshPipelines.find(variant); found != m_meshPipelines.end()) {
        return found->second.get();
    }
    const std::filesystem::path shaders = shaderRoot();
    ShaderModule vertex(m_device);
    ShaderModule fragment(m_device);
    if (!vertex.load(shaders / "mesh.vert.spv") || !fragment.load(shaders / "mesh.frag.spv")) {
        return nullptr;
    }

    // Mirrors the constant_id declarations in shaders/mesh.frag.
    struct Constants {
        VkBool32 physicallyBased;
        int32_t shadowCascades;
        VkBool32 solidSurfaces;
    };

    const Constants constants{.physicallyBased = variant.physicallyBased ? VK_TRUE : VK_FALSE,
                              .shadowCascades = static_cast<int32_t>(variant.shadowCascades),
                              .solidSurfaces = variant.solidSurfaces ? VK_TRUE : VK_FALSE};
    const VkSpecializationMapEntry entries[]{
        {.constantID = 0, .offset = offsetof(Constants, physicallyBased), .size = sizeof(VkBool32)},
        {.constantID = 1, .offset = offsetof(Constants, shadowCascades), .size = sizeof(int32_t)},
        {.constantID = 2, .offset = offsetof(Constants, solidSurfaces), .size = sizeof(VkBool32)}};
    const VkSpecializationInfo specialization{.mapEntryCount = static_cast<uint32_t>(std::size(entries)),
                                              .pMapEntries = entries,
                                              .dataSize = sizeof(constants),
                                              .pData = &constants};
    auto pipelines = std::make_unique<MeshPipelineSet>();
    const SceneDepthUsage depthUsage =
        variant.depthPrepass ? SceneDepthUsage::EqualToPrepass : SceneDepthUsage::TestAndWrite;
    const auto create = [&](GraphicsPipeline& pipeline, bool doubleSided, bool blending) {
        return pipeline.initGraphicsPipeline(m_device, m_sceneColorFormat, PostProcessPass::motionFormat,
                                             m_sceneDepthFormat, m_cameraLayout, m_bindlessLayout, vertex.handle(),
                                             fragment.handle(), m_pipelineCache, doubleSided, blending, &specialization,
                                             depthUsage);
    };
    bool created = create(pipelines->opaque, false, false) && create(pipelines->doubleSided, true, false) &&
                   create(pipelines->blend, false, true) && create(pipelines->blendDoubleSided, true, true);
    if (created && variant.wireframeOverlay) {
        created = createWireframeOverlay(*pipelines, vertex.handle());
    }
    if (!created) {
        pipelines->shutdown(m_device);
        return nullptr;
    }
    return m_meshPipelines.emplace(variant, std::move(pipelines)).first->second.get();
}

bool MeshPass::createWireframeOverlay(MeshPipelineSet& pipelines, VkShaderModule meshVertex) {
    ShaderModule fragment(m_device);
    if (!fragment.load(shaderRoot() / "mesh_wireframe.frag.spv")) {
        return false;
    }
    const auto create = [&](GraphicsPipeline& pipeline, bool doubleSided) {
        return pipeline.initWireframeOverlayPipeline(m_device, m_sceneColorFormat, PostProcessPass::motionFormat,
                                                     m_sceneDepthFormat, m_cameraLayout, m_bindlessLayout, meshVertex,
                                                     fragment.handle(), m_pipelineCache, doubleSided);
    };
    return create(pipelines.wireframe, false) && create(pipelines.wireframeDoubleSided, true);
}

void MeshPass::MeshPipelineSet::shutdown(VkDevice device) {
    opaque.shutdown(device);
    doubleSided.shutdown(device);
    blend.shutdown(device);
    blendDoubleSided.shutdown(device);
    wireframe.shutdown(device);
    wireframeDoubleSided.shutdown(device);
}
