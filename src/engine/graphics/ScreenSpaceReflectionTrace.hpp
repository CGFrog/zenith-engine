#pragma once

#include "engine/graphics/DepthPyramidLayout.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

// Screen-space reflection math shared by shaders/ssr_trace.comp and
// shaders/ssr_composite.comp. The shaders mirror these functions by hand; keep
// them in sync. Conventions:
// - View space is right-handed (+Z towards the camera, the camera looks down
//   -Z); depth is reversed Z (near = 1, far = 0), see DepthConvention.hpp.
// - Screen positions are level-0 pixel coordinates of the render extent
//   (texel centers at +0.5) paired with the depth value, so a ray is a line in
//   (pixel x, pixel y, depth): perspective division keeps both linear along a
//   view-space segment.
// - Tracing walks the depth pyramid's closest-depth channel (g, the maximum in
//   reversed Z); see DepthPyramidLayout.hpp for the level layout.
namespace screen_space_reflection {

// Terms of a perspective projection built like MeshPass builds it
// (glm::perspectiveRH_ZO with swapped planes, Y flipped, optional TAA jitter).
// glm matrices are column-major: projection[column][row].
struct PerspectiveTerms {
    glm::vec2 scale{1.0f};  // projection[0][0], projection[1][1]
    glm::vec2 offset{0.0f}; // projection[2][0], projection[2][1]; holds the jitter
    float depthScale{0.0f}; // projection[2][2]
    float depthBias{0.0f};  // projection[3][2]
};

inline PerspectiveTerms perspectiveTerms(const glm::mat4& projection) {
    return {.scale = {projection[0][0], projection[1][1]},
            .offset = {projection[2][0], projection[2][1]},
            .depthScale = projection[2][2],
            .depthBias = projection[3][2]};
}

// View-space z (negative in front of the camera) at a depth-buffer value.
inline float viewZFromDepth(float depth, const PerspectiveTerms& terms) {
    return -terms.depthBias / (depth + terms.depthScale);
}

inline glm::vec3 viewPositionFromDepth(glm::vec2 ndc, float depth, const PerspectiveTerms& terms) {
    const float viewZ = viewZFromDepth(depth, terms);
    return {-viewZ * (ndc + terms.offset) / terms.scale, viewZ};
}

// Normalized device xy and depth of a view-space point in front of the camera.
inline glm::vec3 projectToNdc(glm::vec3 viewPosition, const PerspectiveTerms& terms) {
    const float w = -viewPosition.z;
    const glm::vec2 ndc = (terms.scale * glm::vec2(viewPosition) + terms.offset * viewPosition.z) / w;
    return {ndc, (terms.depthScale * viewPosition.z + terms.depthBias) / w};
}

inline glm::vec2 ndcToPixel(glm::vec2 ndc, glm::vec2 renderExtent) {
    return (ndc * 0.5f + 0.5f) * renderExtent;
}

inline glm::vec2 pixelToNdc(glm::vec2 pixel, glm::vec2 renderExtent) {
    return pixel / renderExtent * 2.0f - 1.0f;
}

// A reflection ray in screen space: position(t) = origin + direction * t for t
// in [0, maximumT]. t = 1 is the end of the view-space segment; maximumT also
// stops where the ray leaves the render extent or reaches the far plane.
struct ScreenRay {
    glm::vec3 origin{0.0f};
    glm::vec3 direction{0.0f};
    float maximumT{0.0f};
    // View-space length of the segment t in [0, 1].
    float viewLength{0.0f};
};

// Clips a view-space ray to maximumDistance and to just in front of the near
// plane, then projects it. Returns nothing when no segment remains.
inline std::optional<ScreenRay> makeScreenRay(glm::vec3 viewOrigin, glm::vec3 viewDirection, float maximumDistance,
                                              const PerspectiveTerms& terms, glm::vec2 renderExtent) {
    const float nearZ = viewZFromDepth(1.0f, terms);
    float length = maximumDistance;
    if (viewDirection.z > 1e-6f) {
        length = std::min(length, (nearZ - viewOrigin.z) / viewDirection.z * 0.99f);
    }
    if (!(length > 0.0f)) {
        return std::nullopt;
    }
    const glm::vec3 viewEnd = viewOrigin + viewDirection * length;
    const glm::vec3 start = projectToNdc(viewOrigin, terms);
    const glm::vec3 end = projectToNdc(viewEnd, terms);
    ScreenRay ray{.origin = {ndcToPixel(glm::vec2(start), renderExtent), start.z},
                  .direction = {},
                  .maximumT = 1.0f,
                  .viewLength = length};
    ray.direction = glm::vec3(ndcToPixel(glm::vec2(end), renderExtent), end.z) - ray.origin;
    for (int axis = 0; axis < 2; ++axis) {
        if (ray.direction[axis] > 0.0f) {
            ray.maximumT = std::min(ray.maximumT, (renderExtent[axis] - ray.origin[axis]) / ray.direction[axis]);
        } else if (ray.direction[axis] < 0.0f) {
            ray.maximumT = std::min(ray.maximumT, -ray.origin[axis] / ray.direction[axis]);
        }
    }
    if (ray.direction.z < 0.0f) {
        ray.maximumT = std::min(ray.maximumT, -ray.origin.z / ray.direction.z);
    }
    return ray;
}

struct TraceSettings {
    uint32_t maximumIterations{64};
    // A level-0 hit counts only when the ray is at most this far behind the
    // surface, in view units: max(thickness, surface distance * thicknessPerDistance).
    float thickness{0.12f};
    float thicknessPerDistance{0.03f};
};

struct TraceResult {
    bool hit{false};
    // Screen position (pixel xy, depth) of the hit.
    glm::vec3 position{0.0f};
    float t{0.0f};
    uint32_t iterations{0};
};

// Level-0 pixel rectangle of a pyramid texel, following DepthPyramidLayout:
// the last texel of a level also covers the leftover pixels of odd extents.
inline glm::uvec2 pyramidCell(glm::vec2 pixel, uint32_t level, glm::uvec2 renderExtent) {
    const glm::uvec2 levelExtent = depth_pyramid::levelExtent(renderExtent, level);
    const glm::uvec2 basePixel(glm::max(pixel, glm::vec2(0.0f)));
    return glm::min(glm::uvec2(basePixel.x >> level, basePixel.y >> level), levelExtent - 1u);
}

// Smallest t at which the ray leaves the cell's pixel rectangle; infinite
// when the ray has no screen-space motion.
inline float cellExitT(const ScreenRay& ray, glm::uvec2 cell, uint32_t level, glm::uvec2 renderExtent) {
    const glm::uvec2 levelExtent = depth_pyramid::levelExtent(renderExtent, level);
    float exitT = std::numeric_limits<float>::infinity();
    for (int axis = 0; axis < 2; ++axis) {
        const float begin = static_cast<float>(cell[axis] << level);
        const float end = cell[axis] + 1 == levelExtent[axis] ? static_cast<float>(renderExtent[axis])
                                                              : static_cast<float>((cell[axis] + 1) << level);
        if (ray.direction[axis] > 0.0f) {
            exitT = std::min(exitT, (end - ray.origin[axis]) / ray.direction[axis]);
        } else if (ray.direction[axis] < 0.0f) {
            exitT = std::min(exitT, (begin - ray.origin[axis]) / ray.direction[axis]);
        }
    }
    return exitT;
}

// Stackless hierarchical traversal of the closest-depth pyramid. The ray
// skips a whole cell while it stays in front of the closest depth the cell
// covers (greater reversed-Z depth) and climbs a level; when the ray reaches
// that depth inside the cell it descends, until a level-0 texel confirms the
// hit. A level-0 candidate that the ray passed too far behind (thickness) or
// that faces away from the ray is skipped, and the march continues.
//
// closestDepth(level, texel) returns the pyramid's g channel;
// acceptSurface(texel) reports whether the level-0 surface faces the ray.
template <typename ClosestDepth, typename AcceptSurface>
TraceResult traceHiZ(const ScreenRay& ray, glm::uvec2 renderExtent, uint32_t levelCount, const PerspectiveTerms& terms,
                     const TraceSettings& settings, ClosestDepth&& closestDepth, AcceptSurface&& acceptSurface) {
    TraceResult result;
    const float screenStep = std::max(std::abs(ray.direction.x), std::abs(ray.direction.y));
    // Moves a hundredth of a pixel past a cell boundary.
    const float crossT = screenStep > 0.0f ? 0.01f / screenStep : 0.0f;
    const uint32_t topLevel = levelCount > 0 ? levelCount - 1 : 0;
    uint32_t level = 0;
    float t = cellExitT(ray, pyramidCell(glm::vec2(ray.origin), 0, renderExtent), 0, renderExtent) + crossT;
    for (; result.iterations < settings.maximumIterations; ++result.iterations) {
        if (!(t <= ray.maximumT)) {
            return result;
        }
        const glm::vec3 position = ray.origin + ray.direction * t;
        const glm::uvec2 cell = pyramidCell(glm::vec2(position), level, renderExtent);
        const float closest = closestDepth(level, cell);
        const float exitT = cellExitT(ray, cell, level, renderExtent);
        if (position.z <= closest) {
            // At or behind the closest surface the cell covers.
            if (level > 0) {
                --level;
                continue;
            }
            const float surfaceDistance = -viewZFromDepth(closest, terms);
            const float rayDistance = -viewZFromDepth(position.z, terms);
            const float thickness = std::max(settings.thickness, surfaceDistance * settings.thicknessPerDistance);
            if (rayDistance - surfaceDistance <= thickness && acceptSurface(cell)) {
                result.hit = true;
                result.position = position;
                result.t = t;
                return result;
            }
            t = exitT + crossT;
            continue;
        }
        // In front of every surface in the cell: find where the ray, moving
        // away from the camera, reaches the closest depth.
        const float surfaceT = ray.direction.z < 0.0f ? (closest - ray.origin.z) / ray.direction.z
                                                      : std::numeric_limits<float>::infinity();
        if (surfaceT < exitT) {
            t = std::max(t, surfaceT);
            if (level > 0) {
                --level;
                continue;
            }
            // The ray meets this texel's surface inside the texel.
            if (t <= ray.maximumT && acceptSurface(cell)) {
                result.hit = true;
                result.position = ray.origin + ray.direction * t;
                result.t = t;
                return result;
            }
            t = exitT + crossT;
            continue;
        }
        t = exitT + crossT;
        level = std::min(level + 1, topLevel);
    }
    return result;
}

// Fades reflections of perceptually rough surfaces: 1 below start, 0 from end.
inline float roughnessFade(float roughness, float start, float end) {
    const float x = std::clamp((roughness - start) / std::max(end - start, 1e-4f), 0.0f, 1.0f);
    return 1.0f - x * x * (3.0f - 2.0f * x);
}

// Fades hits near the render-extent border; fadeFraction is the band width as
// a fraction of each axis.
inline float screenEdgeFade(glm::vec2 pixel, glm::vec2 renderExtent, float fadeFraction) {
    const glm::vec2 uv = pixel / renderExtent;
    const float border = std::min(std::min(uv.x, uv.y), std::min(1.0f - uv.x, 1.0f - uv.y));
    const float x = std::clamp(border / std::max(fadeFraction, 1e-4f), 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

// Confidence of a hit: fades towards the render-extent border, over the last
// quarter of the ray's reach (travelledFraction = view distance travelled /
// maximum distance), and over the last 30% of the iteration budget
// (iterationFraction = iterations used / maximum), so hits blend into misses.
inline float hitConfidence(glm::vec2 hitPixel, glm::vec2 renderExtent, float edgeFadeFraction, float travelledFraction,
                           float iterationFraction) {
    const auto fadeOut = [](float value, float start, float width) {
        const float x = std::clamp((value - start) / width, 0.0f, 1.0f);
        return 1.0f - x * x * (3.0f - 2.0f * x);
    };
    return screenEdgeFade(hitPixel, renderExtent, edgeFadeFraction) * fadeOut(travelledFraction, 0.75f, 0.25f) *
           fadeOut(iterationFraction, 0.7f, 0.3f);
}

// Schlick Fresnel with the roughness-limited grazing term mesh.frag uses
// (fresnelSchlickRoughness), per channel of the specular reflectance at normal
// incidence (F0) the prepass stores (SceneSpecularEncoding.hpp).
inline glm::vec3 fresnelSchlickRoughness(float normalDotView, glm::vec3 reflectance, float roughness) {
    reflectance = glm::clamp(reflectance, glm::vec3(0.0f), glm::vec3(1.0f));
    const glm::vec3 grazing = glm::max(glm::vec3(1.0f - roughness), reflectance);
    return reflectance + (grazing - reflectance) * std::pow(1.0f - std::clamp(normalDotView, 0.0f, 1.0f), 5.0f);
}

struct CompositeSettings {
    float roughnessFadeStart{0.35f};
    float roughnessFadeEnd{0.6f};
    float intensity{1.0f};
};

// Per-channel weight of the reflected radiance (see blendReflection); every
// channel is within [0, 1]. reflectance is the surface's F0, so metals reflect
// tinted by their base color and dielectrics (F0 = 0.04) reflect untinted.
inline glm::vec3 compositeWeight(float confidence, float normalDotView, float roughness, glm::vec3 reflectance,
                                 const CompositeSettings& settings) {
    const float scale = std::clamp(confidence, 0.0f, 1.0f) *
                        roughnessFade(roughness, settings.roughnessFadeStart, settings.roughnessFadeEnd) *
                        std::max(settings.intensity, 0.0f);
    const glm::vec3 weight = fresnelSchlickRoughness(normalDotView, reflectance, roughness) * scale;
    return glm::clamp(weight, glm::vec3(0.0f), glm::vec3(1.0f));
}

// Replaces a share of the lit scene with the reflected radiance:
// scene * (1 - max(weight)) + reflection * weight. The scalar share keeps the
// result free of color shifts the tint would cause in a per-channel mix (a
// metal's IBL specular would survive in the channels its F0 is low in); with a
// grey weight it equals mix(scene, reflection, weight).
inline glm::vec3 blendReflection(glm::vec3 scene, glm::vec3 reflection, glm::vec3 weight) {
    const float replaced = std::max(std::max(weight.r, weight.g), weight.b);
    return scene * (1.0f - replaced) + reflection * weight;
}

} // namespace screen_space_reflection
