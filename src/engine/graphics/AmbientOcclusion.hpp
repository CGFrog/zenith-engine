#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <optional>

// Ground-truth ambient occlusion (GTAO, Jimenez et al. 2016): CPU mirror of
// shaders/gtao.comp, shaders/gtao_denoise.comp, and the occlusion lookup in
// shaders/mesh.frag, for tests and tools. Keep the formulas in sync.
//
// Conventions:
// - Scene depth is reversed Z (near = 1, far = 0, see DepthConvention.hpp);
//   depth 0 is the cleared sky.
// - View space is right-handed with the camera looking down -Z and +Y up, the
//   space the depth-prepass normals are encoded in (SceneNormalEncoding.hpp).
// - Pixel coordinates are continuous render-extent pixels with the origin at
//   the top-left corner, so pixel centers sit at integer + 0.5.
// - The occlusion target holds `scale` texels per render pixel along each axis
//   (0.5 at half resolution, 1 at full). Occlusion texel t is evaluated at the
//   render pixel floor(t / scale), whose center is t / scale + 0.5.
namespace ambient_occlusion {

inline constexpr float pi = 3.14159265358979f;

// Tunable integration settings, shared by both resolutions.
struct GtaoParameters {
    // View-space radius of the occlusion search, in world units.
    float radius{0.75f};
    // Fraction of the radius over which an occluder's weight fades to zero.
    float falloffFraction{0.6f};
    uint32_t sliceCount{3};
    uint32_t stepsPerSide{4};
    // Exponent applied to the integrated visibility.
    float occlusionPower{1.25f};
};

// Number of occlusion texels that cover the render extent at `scale`.
glm::uvec2 occlusionExtent(glm::uvec2 renderExtent, float scale);

// Texels the occlusion passes write: one texel beyond occlusionExtent (when
// storage allows), so bilinear lookups at the right and bottom edges of a
// dynamic-resolution render extent never read stale texels.
glm::uvec2 dispatchExtent(glm::uvec2 renderExtent, float scale, glm::uvec2 storageExtent);

// Render pixel an occlusion texel is evaluated at, clamped to the render extent.
glm::ivec2 sourcePixel(glm::uvec2 texel, float scale, glm::uvec2 renderExtent);

// Normalized occlusion-target coordinate a fragment at gl_FragCoord.xy samples:
// (fragCoord * scale + 0.5 * (1 - scale)) / storageExtent, which maps every
// texel's source pixel center onto that texel's center.
glm::vec2 occlusionUv(glm::vec2 fragmentCoordinate, float scale, glm::uvec2 storageExtent);

// View-space position of a continuous render pixel at a reversed-Z depth, using
// the inverse of the (jittered) projection that produced the depth.
glm::vec3 reconstructViewPosition(const glm::mat4& inverseProjection, glm::vec2 pixel, glm::uvec2 renderExtent,
                                  float depth);

// Cosine-weighted visible fraction of one slice: the GTAO inner integral for
// horizon angles h0 (negative side) and h1 (positive side) measured from the
// view vector, with the normal projected into the slice at angle n. Returns
// cos(n) + n * sin(n) for an unoccluded slice before weighting.
float sliceArcVisibility(float h0, float h1, float n);

// Slices are spaced uniformly in screen angle, which spaces their planes
// unevenly around a view vector that is off the optical axis. Weighting each
// slice by d(rotation about the view vector) / d(screen angle) =
// |v.z| / (1 - (d.v)^2) keeps the integral exact. `direction` is the slice's
// unit view-space direction (screen direction with y flipped, z = 0).
float sliceDensityWeight(glm::vec3 direction, glm::vec3 viewVector);

// View-space position at a render pixel, or nothing for the sky.
using ViewPositionSource = std::function<std::optional<glm::vec3>(glm::ivec2 pixel)>;

struct GtaoSample {
    // Render pixel (integer) the occlusion is evaluated at.
    glm::ivec2 pixel{0};
    glm::uvec2 renderExtent{1u};
    glm::vec3 viewPosition{0.0f};
    glm::vec3 viewNormal{0.0f, 0.0f, 1.0f};
    // Render pixels per view-space unit at a view distance of 1:
    // 0.5 * renderExtent.y * |projection[1][1]|.
    float projectionScale{1.0f};
    // Per-pixel rotation of the slices and offset of the steps, in [0, 1).
    float sliceNoise{0.0f};
    float stepNoise{0.0f};
};

// Visibility in [0, 1] (1 = unoccluded) before the occlusion power is applied.
float integrateVisibility(const GtaoSample& sample, const ViewPositionSource& positions,
                          const GtaoParameters& parameters);

// Final stored occlusion for a raw visibility.
float applyOcclusionPower(float visibility, const GtaoParameters& parameters);

// Lagarde's specular occlusion from ambient occlusion, roughness, and n.v. It
// is 1 when unoccluded, 0 when fully occluded, and never decreases with ao.
float specularOcclusion(float nDotV, float ambientOcclusion, float roughness);

// Side length of the per-pixel noise tile and radius of the denoise window.
inline constexpr int noiseTileSize = 4;
inline constexpr int denoiseRadius = 2;

// Per-axis spatial weight of a denoise neighbour at `offset` texels, for
// |offset| <= denoiseRadius. Every noise phase receives the same total weight.
float denoiseSpatialWeight(int offset);

// Weight of a denoise neighbour with view depth `sampleDepth` around a center
// at `centerDepth`; 1 at equal depth, falling to 0 across an edge.
float denoiseDepthWeight(float centerDepth, float sampleDepth);

} // namespace ambient_occlusion
