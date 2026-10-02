#pragma once

#include "engine/assets/IrradianceVolumeAsset.hpp"
#include "engine/scene/Scene.hpp"

#include <array>
#include <glm/glm.hpp>
#include <vector>

// CPU reference of irradiance-volume shading. mesh.frag mirrors these
// definitions by hand (search for "Irradiance volume"), and the editor baker
// uses the same mapping and sampler for its secondary bounces, so change them
// together.
//
// Shading picks the eight probes around a surface point, weights each by
// trilinear position, a smooth backface term, and a Chebyshev visibility test
// against the probe's depth moments (as in DDGI), and blends the result toward
// the environment across the volume's edge.
namespace irradiance_volume {
// Octahedral mapping of a unit direction to [0, 1]^2 with +Y at the center
// (0.5, 0.5); the lower hemisphere folds out to the corners.
glm::vec2 octahedralEncode(const glm::vec3& direction);
glm::vec3 octahedralDecode(const glm::vec2& coordinate);
// Direction through the center of a depth-moment texel.
glm::vec3 depthTexelDirection(uint32_t texelX, uint32_t texelY);

// Bilinearly filtered depth moments in a direction; texel lookups clamp at the
// map's border.
glm::vec2 sampleDepthMoments(const IrradianceProbe& probe, const glm::vec3& direction);

// Probability that a point at `distance` from the probe is visible to it, from
// one-tailed Chebyshev's inequality on the moments, sharpened by cubing. One
// at or in front of the mean distance.
float chebyshevVisibility(const glm::vec2& moments, float distance);

struct ProbeCorner {
    glm::uvec3 index{0u};
    // Trilinear weight; the eight corners sum to one.
    float trilinear{0.0f};
};

// The eight probes enclosing `position` (clamped to the probe lattice) and
// their trilinear weights. Corners repeat on axes with a single probe.
std::array<ProbeCorner, 8> surroundingProbes(const IrradianceVolumeData& data, const glm::vec3& position);

// Fraction of the volume applied at `position`: one inside the box shrunk by
// blendDistance, falling linearly to zero at the boundary, zero outside.
float edgeWeight(const IrradianceVolumeData& data, const glm::vec3& position, float blendDistance);

struct ShadingParameters {
    // World-space offsets applied along the surface normal and toward the
    // viewer before choosing and testing probes, to avoid self-shadowing.
    float normalBias{0.1f};
    float viewBias{0.1f};
    float blendDistance{0.5f};
};

struct Sample {
    // Lambertian outgoing radiance of a white surface (irradiance / pi),
    // computed from the clamped lattice even outside the box.
    glm::vec3 irradiance{0.0f};
    // edgeWeight at the unbiased position, or zero when no probe was visible.
    float volumeWeight{0.0f};
};

Sample sample(const IrradianceVolumeData& data, const ShadingParameters& parameters, const glm::vec3& position,
              const glm::vec3& normal, const glm::vec3& viewDirection);

// Chooses the baked volume to bind for a camera: one containing the camera,
// else the nearest; ties go to the lower scene entity ID. entt::null when no
// volume has baked data.
Entity selectVolume(const Scene& scene, const glm::vec3& cameraPosition);

// std430 record of one probe in the shader storage buffer; mirrors
// IrradianceProbe in shaders/mesh.frag. irradiance[0].w is 1 for valid probes.
struct GpuProbe {
    std::array<glm::vec4, 9> irradiance{};
    std::array<glm::vec2, irradianceProbeDepthTexels> depthMoments{};
};

static_assert(sizeof(GpuProbe) == 9 * 16 + irradianceProbeDepthTexels * 8, "GpuProbe must match the std430 layout");

std::vector<GpuProbe> packProbes(const IrradianceVolumeData& data);
} // namespace irradiance_volume
