#pragma once

#include "engine/assets/Assets.hpp"

#include <array>
#include <functional>
#include <glm/glm.hpp>
#include <span>
#include <unordered_map>
#include <vector>

struct PrefilteredEnvironment {
    glm::vec3 diffuseSky{0.0f};
    glm::vec3 diffuseGround{0.0f};
    std::vector<glm::vec3> specularRoughnessLevels;
    uint64_t sourceHash{0};
};

// Deterministic CPU preprocessing used by tools and as the compatible runtime
// fallback. The result is cached by content, not by source path.
class EnvironmentLightingCache {
  public:
    const PrefilteredEnvironment& preprocess(const TextureAsset& equirectangular, uint32_t roughnessLevels = 6);

    size_t entryCount() const {
        return m_entries.size();
    }

    void clear() {
        m_entries.clear();
    }

  private:
    std::unordered_map<uint64_t, PrefilteredEnvironment> m_entries;
};

// CPU reference for the image-based-lighting conventions the GPU precompute in
// EnvironmentPass and the env_*.comp shaders implement; the GLSL mirrors these
// definitions by hand, so change both together.
//
// Directions are world space with +Y up. Cube faces follow Vulkan's layer order
// (+X, -X, +Y, -Y, +Z, -Z) and face coordinates are in [0, 1] with (0, 0) at the
// first texel of the face.
namespace environment_lighting {
struct CubeFaceCoordinate {
    uint32_t face{0};
    glm::vec2 uv{0.0f};
};

glm::vec3 cubeFaceDirection(uint32_t face, const glm::vec2& uv);
CubeFaceCoordinate cubeFaceCoordinate(const glm::vec3& direction);

// Equirectangular maps put +Y on the first row and wrap longitude around Y,
// with u = 0.5 facing +X.
glm::vec2 equirectangularCoordinate(const glm::vec3& direction);
glm::vec3 equirectangularDirection(const glm::vec2& uv);

// Analytic sky used when an environment has no map: a horizon band between
// the linear-light sky and ground colors. The sun is drawn by the skybox only,
// because direct lighting already accounts for it.
glm::vec3 proceduralSkyRadiance(const glm::vec3& direction, const glm::vec3& skyColor, const glm::vec3& groundColor);

// Order-2 spherical harmonics, already convolved with the clamped cosine and
// divided by pi: evaluating them yields the Lambertian outgoing radiance for a
// white surface, so a constant environment evaluates to its own radiance.
using IrradianceCoefficients = std::array<glm::vec3, 9>;
using RadianceFunction = std::function<glm::vec3(const glm::vec3&)>;

IrradianceCoefficients projectIrradiance(const RadianceFunction& radiance, uint32_t faceSize);
glm::vec3 evaluateIrradiance(const IrradianceCoefficients& coefficients, const glm::vec3& normal);

// Split-sum environment BRDF: specular = prefiltered * (F0 * scale + bias).
// Roughness is perceptual (GGX alpha = roughness squared).
glm::vec2 integrateEnvironmentBrdf(float nDotV, float roughness, uint32_t sampleCount);

// Roughness assigned to a prefiltered specular mip; mip 0 is a mirror.
float prefilteredMipRoughness(uint32_t mip, uint32_t mipCount);
} // namespace environment_lighting
