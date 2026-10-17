#include "engine/graphics/MaterialRecord.hpp"

#include "engine/scene/Scene.hpp"

#include <algorithm>
#include <cmath>

namespace {
// Beer-Lambert coefficient that leaves `color` of white light after `distance`.
glm::vec3 attenuationCoefficient(const glm::vec3& color, float distance) {
    if (!std::isfinite(distance) || distance <= 0.0f) {
        return glm::vec3(0.0f);
    }
    return -glm::log(glm::clamp(color, glm::vec3(1.0e-4f), glm::vec3(1.0f))) / distance;
}

glm::vec4 volumeRecord(const MaterialAsset& material) {
    if (material.water.enabled) {
        return {attenuationCoefficient(material.water.absorptionColor, material.water.absorptionDistance),
                std::max(material.water.fallbackDepth, 0.0f)};
    }
    return {attenuationCoefficient(material.transmission.attenuationColor, material.transmission.attenuationDistance),
            std::max(material.transmission.thickness, 0.0f)};
}
} // namespace

uint32_t materialLobeFlags(const MaterialAsset& material) {
    if (material.water.enabled) {
        return material_lobes::water;
    }
    uint32_t flags = material.terrain.enabled ? material_lobes::terrain : 0u;
    if (material.clearcoat.factor > 0.0f) {
        flags |= material_lobes::clearcoat;
    }
    if (std::max({material.sheen.color.r, material.sheen.color.g, material.sheen.color.b}) > 0.0f) {
        flags |= material_lobes::sheen;
    }
    if (material.transmission.factor > 0.0f) {
        flags |= material_lobes::transmission;
        if (material.transmission.thickness > 0.0f) {
            flags |= material_lobes::volume;
        }
    }
    if (material.specular.texture || material.specular.colorTexture) {
        flags |= material_lobes::specularTextures;
    }
    return flags;
}

MaterialRecord packMaterialRecord(const MaterialAsset& material, const MaterialTextureSlots& slots) {
    return {
        .baseColor = material.baseColor,
        .factors = {material.metallic, material.roughness, material.normalScale, material.occlusionStrength},
        .emissive = {material.emissive * material.emissiveStrength, 0.0f},
        .surface = {material.alphaCutoff, static_cast<float>(material.alphaMode), std::max(material.ior, 1.0f),
                    std::clamp(material.transmission.factor, 0.0f, 1.0f)},
        .textures = {slots.baseColor, slots.normal, slots.metallicRoughness, slots.occlusion},
        .extraTextures = {slots.emissive, materialLobeFlags(material), slots.transmission, 0u},
        .clearcoat = {std::clamp(material.clearcoat.factor, 0.0f, 1.0f),
                      std::clamp(material.clearcoat.roughness, 0.0f, 1.0f), material.clearcoat.normalScale, 0.0f},
        .sheen = {glm::max(material.sheen.color, glm::vec3(0.0f)), std::clamp(material.sheen.roughness, 0.0f, 1.0f)},
        .specular = {glm::max(material.specular.color, glm::vec3(0.0f)),
                     std::clamp(material.specular.factor, 0.0f, 1.0f)},
        .volume = volumeRecord(material),
        .lobeTextures = {slots.clearcoat, slots.clearcoatRoughness, slots.clearcoatNormal, slots.sheenColor},
        .layerTextures = {slots.sheenRoughness, slots.specular, slots.specularColor, 0u},
        .water =
            material.water.enabled
                ? glm::vec4(std::max(material.water.waveLength, 0.01f), std::max(material.water.waveSteepness, 0.0f),
                            std::max(material.water.waveSpeed, 0.0f), std::max(material.water.shoreFade, 0.0f))
                : glm::vec4(0.0f),
        .terrainAlbedo =
            glm::uvec4(slots.terrainAlbedo[0], slots.terrainAlbedo[1], slots.terrainAlbedo[2], slots.terrainAlbedo[3]),
        .terrainNormal =
            glm::uvec4(slots.terrainNormal[0], slots.terrainNormal[1], slots.terrainNormal[2], slots.terrainNormal[3]),
        .terrainArm = glm::uvec4(slots.terrainArm[0], slots.terrainArm[1], slots.terrainArm[2], slots.terrainArm[3]),
        .terrainTileSize = glm::vec4(material.terrain.layers[0].tileSize, material.terrain.layers[1].tileSize,
                                     material.terrain.layers[2].tileSize, material.terrain.layers[3].tileSize)};
}

bool materialOverrideChangesFactors(const MaterialOverrideComponent& values) {
    return values.baseColorFactor != glm::vec4(1.0f) || values.emissive != glm::vec3(0.0f) ||
           values.roughnessFactor != 1.0f || values.metallicFactor != 1.0f;
}

MaterialAsset applyMaterialOverride(const MaterialAsset& material, const MaterialOverrideComponent& values) {
    MaterialAsset result = material;
    result.baseColor = material.baseColor * glm::max(values.baseColorFactor, glm::vec4(0.0f));
    result.emissive = material.emissive * material.emissiveStrength +
                      glm::max(values.emissive, glm::vec3(0.0f)) * std::max(values.emissiveStrength, 0.0f);
    result.emissiveStrength = 1.0f;
    result.roughness = std::clamp(material.roughness * values.roughnessFactor, 0.0f, 1.0f);
    result.metallic = std::clamp(material.metallic * values.metallicFactor, 0.0f, 1.0f);
    return result;
}