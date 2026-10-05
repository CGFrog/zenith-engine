#include "engine/graphics/MaterialRecord.hpp"

#include "engine/scene/Scene.hpp"

#include <algorithm>
#include <cmath>

namespace {
// Beer-Lambert coefficient that leaves `color` of white light after `distance`.
glm::vec3 attenuationCoefficient(const MaterialTransmission& transmission) {
    if (!std::isfinite(transmission.attenuationDistance) || transmission.attenuationDistance <= 0.0f) {
        return glm::vec3(0.0f);
    }
    const glm::vec3 color = glm::clamp(transmission.attenuationColor, glm::vec3(1.0e-4f), glm::vec3(1.0f));
    return -glm::log(color) / transmission.attenuationDistance;
}
} // namespace

uint32_t materialLobeFlags(const MaterialAsset& material) {
    uint32_t flags = 0;
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
        .volume = {attenuationCoefficient(material.transmission), std::max(material.transmission.thickness, 0.0f)},
        .lobeTextures = {slots.clearcoat, slots.clearcoatRoughness, slots.clearcoatNormal, slots.sheenColor},
        .layerTextures = {slots.sheenRoughness, slots.specular, slots.specularColor, 0u}};
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