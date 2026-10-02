#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

// Encoding of the depth prepass specular target (PostProcessPass::specularFormat,
// R8G8B8A8_SRGB). CPU mirror of shaders/depth_normal.frag's second output for
// tests and tools; screen-space reflections sample it in GLSL, where the sRGB
// view decodes it back to linear.
//
//   rgb: linear specular reflectance at normal incidence (F0), as the shading
//        pass computes it: mix(0.04, base color, metallic). The sRGB transfer
//        spends the 8 bits where dielectric F0 lives (0.04 stores as 56/255).
//   a:   1 where the prepass wrote an opaque surface, 0 where it was cleared.
//
// The prepass writes it only on frames that trace screen-space reflections;
// otherwise the attachment is bound as VK_NULL_HANDLE and writes are dropped.
// Cost: one 4-byte texel per output pixel per frame slot, allocated on first
// use.
namespace scene_specular {

// Reflectance of non-metals at normal incidence, as in mesh.frag.
inline constexpr float dielectricReflectance = 0.04f;

// F0 of a metallic-roughness material; metallic is clamped to [0, 1].
inline glm::vec3 reflectanceAtNormal(glm::vec3 baseColor, float metallic) {
    return glm::mix(glm::vec3(dielectricReflectance), baseColor, std::clamp(metallic, 0.0f, 1.0f));
}

inline float linearToSrgb(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    return value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

inline float srgbToLinear(float value) {
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

// Round trip through the stored 8-bit sRGB representation.
inline glm::vec3 storeAndLoadReflectance(glm::vec3 reflectance) {
    glm::vec3 loaded;
    for (int channel = 0; channel < 3; ++channel) {
        const float stored = std::round(linearToSrgb(reflectance[channel]) * 255.0f) / 255.0f;
        loaded[channel] = srgbToLinear(stored);
    }
    return loaded;
}

} // namespace scene_specular
