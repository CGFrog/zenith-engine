#pragma once

#include "engine/assets/Assets.hpp"

#include <cstdint>
#include <glm/glm.hpp>

// GPU form of a MaterialAsset: one std430 record per material in the camera
// set's material buffer (binding 4). Mirrored by MaterialRecord in
// shaders/mesh.frag, shaders/shadow.frag and shaders/depth_normal.frag; change
// all four together.
struct MaterialRecord {
    glm::vec4 baseColor{1.0f};
    glm::vec4 factors{1.0f, 1.0f, 1.0f, 1.0f}; // metallic, roughness, normal scale, occlusion strength
    glm::vec4 emissive{0.0f};                  // rgb radiance; w unused
    // Alpha cutoff, alpha mode (0 opaque, 1 mask, 2 blend), index of
    // refraction, transmission factor.
    glm::vec4 surface{0.5f, 0.0f, 1.5f, 0.0f};
    glm::uvec4 textures{0u};      // bindless slots: base color, normal, metallic-roughness, occlusion
    glm::uvec4 extraTextures{0u}; // emissive slot, lobe flags (material_lobes), transmission slot, unused
    glm::vec4 clearcoat{0.0f};    // factor, perceptual roughness, normal scale, unused
    glm::vec4 sheen{0.0f};        // linear rgb color, perceptual roughness
    glm::vec4 specular{1.0f};     // linear rgb color, factor
    // Volume attenuation coefficient per world unit (rgb) and thickness.
    glm::vec4 volume{0.0f};
    glm::uvec4 lobeTextures{0u};  // bindless slots: clearcoat, clearcoat roughness, clearcoat normal, sheen color
    glm::uvec4 layerTextures{0u}; // bindless slots: sheen roughness, specular, specular color; w unused
};

static_assert(sizeof(MaterialRecord) == 192, "MaterialRecord must match the std430 layout in the shaders");

// Bits of MaterialRecord::extraTextures.y. A shader evaluates a lobe (and
// samples its textures) only when its bit is set, so materials without the
// KHR extensions pay a uniform branch per lobe and nothing else.
namespace material_lobes {
inline constexpr uint32_t clearcoat = 1u << 0;
inline constexpr uint32_t sheen = 1u << 1;
inline constexpr uint32_t transmission = 1u << 2;
// KHR_materials_specular textures; the factors always apply.
inline constexpr uint32_t specularTextures = 1u << 3;
// KHR_materials_volume thickness: transmission refracts through a medium.
inline constexpr uint32_t volume = 1u << 4;
} // namespace material_lobes

// Bindless slots of every texture a record references. The renderer resolves
// them, substituting its neutral fallbacks for absent textures (white for
// factors and colors, flat for normals).
struct MaterialTextureSlots {
    uint32_t baseColor{0};
    uint32_t normal{0};
    uint32_t metallicRoughness{0};
    uint32_t occlusion{0};
    uint32_t emissive{0};
    uint32_t clearcoat{0};
    uint32_t clearcoatRoughness{0};
    uint32_t clearcoatNormal{0};
    uint32_t sheenColor{0};
    uint32_t sheenRoughness{0};
    uint32_t specular{0};
    uint32_t specularColor{0};
    uint32_t transmission{0};
};

uint32_t materialLobeFlags(const MaterialAsset& material);
MaterialRecord packMaterialRecord(const MaterialAsset& material, const MaterialTextureSlots& slots);
