#pragma once

// Encoding of the depth prepass normal target (PostProcessPass::normalFormat, A2B10G10R10_UNORM).

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

// Encoding of the depth prepass normal target (PostProcessPass::normalFormat,
// A2B10G10R10_UNORM). CPU mirror of shaders/depth_normal.frag for tests and
// tools; screen-space consumers decode the same way in GLSL.
//
//   rg: octahedral view-space normal, mapped from [-1, 1] to [0, 1].
//       View space is right-handed: +X right, +Y up, the camera looks down -Z,
//       so normals facing the camera have positive Z.
//   b:  perceptual roughness, clamped to [0.045, 1] like the shading pass.
//   a:  1 where the prepass wrote an opaque surface, 0 where it was cleared.
namespace scene_normal {

inline glm::vec2 signNotZero(glm::vec2 value) {
    return {value.x >= 0.0f ? 1.0f : -1.0f, value.y >= 0.0f ? 1.0f : -1.0f};
}

// Maps a unit vector to the [-1, 1] octahedral square.
inline glm::vec2 encodeOctahedral(glm::vec3 normal) {
    normal /= std::abs(normal.x) + std::abs(normal.y) + std::abs(normal.z);
    glm::vec2 encoded(normal.x, normal.y);
    if (normal.z < 0.0f) {
        encoded = (1.0f - glm::abs(glm::vec2(encoded.y, encoded.x))) * signNotZero(encoded);
    }
    return encoded;
}

inline glm::vec3 decodeOctahedral(glm::vec2 encoded) {
    glm::vec3 normal(encoded.x, encoded.y, 1.0f - std::abs(encoded.x) - std::abs(encoded.y));
    const float fold = std::max(-normal.z, 0.0f);
    normal.x += normal.x >= 0.0f ? -fold : fold;
    normal.y += normal.y >= 0.0f ? -fold : fold;
    return glm::normalize(normal);
}

// Rounds a [0, 1] value to the 10-bit UNORM the target stores.
inline float quantizeUnorm10(float value) {
    constexpr float maximum = 1023.0f;
    return std::round(std::clamp(value, 0.0f, 1.0f) * maximum) / maximum;
}

// Round trip through the stored representation of the normal channels.
inline glm::vec3 storeAndLoadNormal(glm::vec3 normal) {
    const glm::vec2 stored = encodeOctahedral(glm::normalize(normal)) * 0.5f + 0.5f;
    const glm::vec2 loaded(quantizeUnorm10(stored.x), quantizeUnorm10(stored.y));
    return decodeOctahedral(loaded * 2.0f - 1.0f);
}

} // namespace scene_normal
