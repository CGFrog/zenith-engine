#include "engine/graphics/DepthPyramidLayout.hpp"
#include "engine/graphics/RenderConfiguration.hpp"
#include "engine/graphics/SceneSpecularEncoding.hpp"
#include "engine/graphics/ScreenSpaceReflectionTrace.hpp"
#include "engine/graphics/TemporalQuality.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <vector>

namespace ssr = screen_space_reflection;

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

constexpr float nearPlane = 0.1f;
constexpr float farPlane = 200.0f;
constexpr glm::uvec2 renderExtent{317, 181}; // odd sizes exercise the pyramid's leftover texels

// Built like MeshPass: reversed Z by swapping the planes, Y flipped.
glm::mat4 sceneProjection(glm::vec2 jitterNdc) {
    glm::mat4 projection = glm::perspectiveRH_ZO(
        glm::radians(60.0f), static_cast<float>(renderExtent.x) / static_cast<float>(renderExtent.y), farPlane,
        nearPlane);
    projection[1][1] *= -1.0f;
    return applyProjectionJitter(projection, jitterNdc);
}

struct Plane {
    glm::vec3 normal;
    float distance; // dot(normal, p) = distance on the plane
    // Bounds of the finite plane in view space.
    glm::vec3 minimum;
    glm::vec3 maximum;
};

// A synthetic view-space scene rendered into a depth image and normal image.
struct SyntheticScene {
    std::vector<Plane> planes;
    std::vector<float> depth;
    std::vector<glm::vec3> normals;
    std::vector<std::vector<glm::vec2>> pyramid; // per level: r farthest, g closest

    void render(const ssr::PerspectiveTerms& terms) {
        const glm::vec2 extent(renderExtent);
        depth.assign(static_cast<size_t>(renderExtent.x) * renderExtent.y, 0.0f);
        normals.assign(depth.size(), glm::vec3(0.0f));
        for (uint32_t y = 0; y < renderExtent.y; ++y) {
            for (uint32_t x = 0; x < renderExtent.x; ++x) {
                const glm::vec2 ndc = ssr::pixelToNdc(glm::vec2(x, y) + 0.5f, extent);
                const glm::vec3 direction = glm::normalize(ssr::viewPositionFromDepth(ndc, 0.5f, terms));
                float nearest = farPlane;
                for (const Plane& plane : planes) {
                    const float facing = glm::dot(plane.normal, direction);
                    if (std::abs(facing) < 1e-6f) {
                        continue;
                    }
                    const float distance = plane.distance / facing;
                    const glm::vec3 point = direction * distance;
                    const float tolerance = 1e-4f;
                    if (distance <= 0.0f || distance >= nearest ||
                        glm::any(glm::lessThan(point, plane.minimum - tolerance)) ||
                        glm::any(glm::greaterThan(point, plane.maximum + tolerance))) {
                        continue;
                    }
                    nearest = distance;
                    const size_t index = static_cast<size_t>(y) * renderExtent.x + x;
                    depth[index] = ssr::projectToNdc(point, terms).z;
                    normals[index] = plane.normal;
                }
            }
        }
        buildPyramid();
    }

    void buildPyramid() {
        const uint32_t levels = depth_pyramid::levelCount(renderExtent);
        pyramid.assign(levels, {});
        pyramid[0].resize(depth.size());
        for (size_t index = 0; index < depth.size(); ++index) {
            pyramid[0][index] = glm::vec2(depth[index]);
        }
        for (uint32_t level = 1; level < levels; ++level) {
            const glm::uvec2 source = depth_pyramid::levelExtent(renderExtent, level - 1);
            const glm::uvec2 output = depth_pyramid::levelExtent(renderExtent, level);
            pyramid[level].resize(static_cast<size_t>(output.x) * output.y);
            for (uint32_t y = 0; y < output.y; ++y) {
                for (uint32_t x = 0; x < output.x; ++x) {
                    const depth_pyramid::SourceRange range = depth_pyramid::sourceTexels({x, y}, output, source);
                    glm::vec2 reduced(1.0f, 0.0f);
                    for (uint32_t sy = range.begin.y; sy < range.end.y; ++sy) {
                        for (uint32_t sx = range.begin.x; sx < range.end.x; ++sx) {
                            const glm::vec2 value = pyramid[level - 1][static_cast<size_t>(sy) * source.x + sx];
                            reduced = {std::min(reduced.x, value.x), std::max(reduced.y, value.y)};
                        }
                    }
                    pyramid[level][static_cast<size_t>(y) * output.x + x] = reduced;
                }
            }
        }
    }

    float closest(uint32_t level, glm::uvec2 texel) const {
        const glm::uvec2 extent = depth_pyramid::levelExtent(renderExtent, level);
        return pyramid[level][static_cast<size_t>(texel.y) * extent.x + texel.x].y;
    }

    glm::vec3 normalAt(glm::uvec2 texel) const {
        return normals[static_cast<size_t>(texel.y) * renderExtent.x + texel.x];
    }
};

// A floor at y = -1 (towards z = -10) and a wall at z = -10 up to y = 2.
SyntheticScene floorAndWall() {
    SyntheticScene scene;
    scene.planes.push_back({{0, 1, 0}, -1.0f, {-100, -1, -10}, {100, -1, 0}});
    scene.planes.push_back({{0, 0, 1}, -10.0f, {-100, -1, -10}, {100, 2, -10}});
    return scene;
}

ssr::TraceResult trace(const SyntheticScene& scene, const ssr::ScreenRay& ray, const ssr::PerspectiveTerms& terms,
                       glm::vec3 viewDirection, const ssr::TraceSettings& settings = {}) {
    return ssr::traceHiZ(
        ray, renderExtent, static_cast<uint32_t>(scene.pyramid.size()), terms, settings,
        [&](uint32_t level, glm::uvec2 texel) { return scene.closest(level, texel); },
        [&](glm::uvec2 texel) { return glm::dot(scene.normalAt(texel), viewDirection) < 0.0f; });
}

glm::vec2 projectPixel(glm::vec3 viewPosition, const ssr::PerspectiveTerms& terms) {
    return ssr::ndcToPixel(glm::vec2(ssr::projectToNdc(viewPosition, terms)), glm::vec2(renderExtent));
}

void testViewPositionRoundTrip() {
    const ssr::PerspectiveTerms terms = ssr::perspectiveTerms(sceneProjection({0.0031f, -0.0047f}));
    float worst = 0.0f;
    for (float x = -0.95f; x <= 0.95f; x += 0.19f) {
        for (float y = -0.95f; y <= 0.95f; y += 0.19f) {
            for (float depth : {1.0f, 0.5f, 0.05f, 0.001f}) {
                const glm::vec3 view = ssr::viewPositionFromDepth({x, y}, depth, terms);
                const glm::vec3 projected = ssr::projectToNdc(view, terms);
                worst = std::max(worst, glm::length(projected - glm::vec3(x, y, depth)));
            }
        }
    }
    expect(worst < 1e-4f, "depth reconstruction round-trips through the jittered projection");
    expect(std::abs(ssr::viewZFromDepth(1.0f, terms) + nearPlane) < 1e-4f, "reversed-Z depth 1 is the near plane");
    expect(std::abs(ssr::viewZFromDepth(0.0f, terms) + farPlane) < 1e-2f, "reversed-Z depth 0 is the far plane");
    expect(ssr::projectToNdc({0, 0, -2}, terms).z > ssr::projectToNdc({0, 0, -20}, terms).z,
           "closer points have greater reversed-Z depth");
    const glm::vec2 pixel =
        ssr::ndcToPixel(ssr::pixelToNdc({12.5f, 99.5f}, glm::vec2(renderExtent)), glm::vec2(renderExtent));
    expect(glm::length(pixel - glm::vec2(12.5f, 99.5f)) < 1e-4f, "pixel and NDC conversions are inverse");
}

void testPyramidOrdering() {
    const ssr::PerspectiveTerms terms = ssr::perspectiveTerms(sceneProjection({0.0f, 0.0f}));
    SyntheticScene scene = floorAndWall();
    scene.render(terms);
    bool ordered = true;
    for (const auto& level : scene.pyramid) {
        for (const glm::vec2& texel : level) {
            ordered = ordered && texel.y >= texel.x;
        }
    }
    expect(ordered, "pyramid closest depth (g) is never below the farthest (r) in reversed Z");
    const glm::vec2 top = scene.pyramid.back().front();
    expect(top.x == 0.0f, "the top level's farthest depth is the sky");
    expect(std::abs(top.y - scene.closest(0, {renderExtent.x / 2, renderExtent.y - 1})) < 1e-6f,
           "the top level's closest depth is the nearest floor texel");
}

// Mirror ray from the floor up to the wall.
void testRayHitsWall() {
    const ssr::PerspectiveTerms terms = ssr::perspectiveTerms(sceneProjection({0.0f, 0.0f}));
    SyntheticScene scene = floorAndWall();
    scene.render(terms);
    const glm::vec3 origin(0.0f, -1.0f, -6.0f);
    const glm::vec3 toCamera = glm::normalize(-origin);
    const glm::vec3 direction = glm::reflect(-toCamera, glm::vec3(0, 1, 0));
    const std::optional<ssr::ScreenRay> ray =
        ssr::makeScreenRay(origin + glm::vec3(0, 0.01f, 0), direction, 60.0f, terms, glm::vec2(renderExtent));
    expect(ray.has_value(), "a ray away from the camera produces a screen segment");
    if (!ray) {
        return;
    }
    const ssr::TraceResult result = trace(scene, *ray, terms, direction);
    // The mirror direction is (0, 1, -6) normalized: it meets z = -10 at y = -1/3.
    const glm::vec2 expected = projectPixel({0.0f, -1.0f / 3.0f, -10.0f}, terms);
    expect(result.hit, "the floor reflection ray hits the wall");
    expect(glm::length(glm::vec2(result.position) - expected) < 1.5f, "the hit lands at the expected texel");
    expect(result.iterations < ssr::TraceSettings{}.maximumIterations, "the trace finishes within its budget");
    const glm::uvec2 texel = ssr::pyramidCell(glm::vec2(result.position), 0, renderExtent);
    expect(scene.normalAt(texel).z > 0.5f, "the hit surface is the wall, not the reflecting floor");
}

void testRayAboveSceneMisses() {
    const ssr::PerspectiveTerms terms = ssr::perspectiveTerms(sceneProjection({0.0f, 0.0f}));
    SyntheticScene scene = floorAndWall();
    scene.render(terms);
    const glm::vec3 origin(0.0f, -1.0f, -6.0f);
    // Steeply up: passes over the wall's top edge (y = 2 at z = -10).
    const glm::vec3 direction = glm::normalize(glm::vec3(0.0f, 1.0f, -0.4f));
    const std::optional<ssr::ScreenRay> ray =
        ssr::makeScreenRay(origin + glm::vec3(0, 0.01f, 0), direction, 60.0f, terms, glm::vec2(renderExtent));
    expect(ray.has_value(), "a ray above the scene still produces a screen segment");
    if (!ray) {
        return;
    }
    expect(!trace(scene, *ray, terms, direction).hit, "a ray above every surface misses");
}

// A bar close to the camera covers part of the ray's screen path; the ray
// passes far behind it, so only a large thickness accepts it as the hit.
void testThicknessRejectsOccluderInFront() {
    const ssr::PerspectiveTerms terms = ssr::perspectiveTerms(sceneProjection({0.0f, 0.0f}));
    SyntheticScene scene = floorAndWall();
    scene.planes.push_back({{0, 0, 1}, -3.0f, {-5, -0.36f, -3}, {5, -0.24f, -3}});
    scene.render(terms);
    const glm::vec3 origin(0.0f, -1.0f, -6.0f);
    const glm::vec3 direction = glm::reflect(-glm::normalize(-origin), glm::vec3(0, 1, 0));
    const std::optional<ssr::ScreenRay> ray =
        ssr::makeScreenRay(origin + glm::vec3(0, 0.01f, 0), direction, 60.0f, terms, glm::vec2(renderExtent));
    if (!ray) {
        expect(false, "occluder test ray exists");
        return;
    }
    const glm::vec2 wallHit = projectPixel({0.0f, -1.0f / 3.0f, -10.0f}, terms);
    const ssr::TraceResult thin = trace(scene, *ray, terms, direction);
    expect(thin.hit && glm::length(glm::vec2(thin.position) - wallHit) < 1.5f,
           "a thin occluder in front of the ray is skipped and the wall is hit");
    ssr::TraceSettings thick;
    thick.thickness = 100.0f;
    const ssr::TraceResult blocked = trace(scene, *ray, terms, direction, thick);
    expect(blocked.hit && glm::length(glm::vec2(blocked.position) - wallHit) > 3.0f &&
               std::abs(ssr::viewZFromDepth(
                            scene.closest(0, ssr::pyramidCell(glm::vec2(blocked.position), 0, renderExtent)), terms) +
                        3.0f) < 0.01f,
           "with unlimited thickness the closer occluder is reported");
}

void testRayClipsToNearPlane() {
    const ssr::PerspectiveTerms terms = ssr::perspectiveTerms(sceneProjection({0.0f, 0.0f}));
    const glm::vec3 origin(0.3f, -0.5f, -4.0f);
    const glm::vec3 direction = glm::normalize(glm::vec3(0.1f, 0.2f, 1.0f));
    const std::optional<ssr::ScreenRay> ray =
        ssr::makeScreenRay(origin, direction, 60.0f, terms, glm::vec2(renderExtent));
    expect(ray.has_value(), "a ray towards the camera keeps a segment");
    if (ray) {
        const float endDepth = ray->origin.z + ray->direction.z;
        expect(endDepth <= 1.0f && endDepth > ray->origin.z, "a ray towards the camera stops before the near plane");
        expect(ray->viewLength < 4.0f, "the clipped length is shorter than the distance to the camera");
    }
    expect(!ssr::makeScreenRay(origin, direction, 0.0f, terms, glm::vec2(renderExtent)).has_value(),
           "a zero-length ray has no segment");
}

void testFadesAndCompositeWeight() {
    expect(ssr::roughnessFade(0.0f, 0.35f, 0.6f) == 1.0f, "smooth surfaces keep full reflections");
    expect(ssr::roughnessFade(1.0f, 0.35f, 0.6f) == 0.0f, "rough surfaces lose reflections");
    float previous = 1.0f;
    bool monotonic = true;
    for (float roughness = 0.0f; roughness <= 1.0f; roughness += 0.01f) {
        const float fade = ssr::roughnessFade(roughness, 0.35f, 0.6f);
        monotonic = monotonic && fade <= previous + 1e-6f && fade >= 0.0f && fade <= 1.0f;
        previous = fade;
    }
    expect(monotonic, "the roughness fade decreases within [0, 1]");

    const glm::vec2 extent(renderExtent);
    expect(ssr::screenEdgeFade(extent * 0.5f, extent, 0.08f) == 1.0f, "hits at the screen center are not faded");
    expect(ssr::screenEdgeFade({0.0f, extent.y * 0.5f}, extent, 0.08f) == 0.0f, "hits at the border fade out");
    bool edgeBounded = true;
    for (float x = -10.0f; x <= extent.x + 10.0f; x += 7.0f) {
        const float fade = ssr::screenEdgeFade({x, 20.0f}, extent, 0.08f);
        edgeBounded = edgeBounded && fade >= 0.0f && fade <= 1.0f;
    }
    expect(edgeBounded, "the edge fade stays within [0, 1]");
    bool confidenceBounded = true;
    for (float travelled = -0.5f; travelled <= 1.5f; travelled += 0.1f) {
        for (float iterations = -0.5f; iterations <= 1.5f; iterations += 0.1f) {
            const float confidence = ssr::hitConfidence(extent * 0.3f, extent, 0.08f, travelled, iterations);
            confidenceBounded = confidenceBounded && confidence >= 0.0f && confidence <= 1.0f;
        }
    }
    expect(confidenceBounded, "hit confidence stays within [0, 1]");
    expect(ssr::hitConfidence(extent * 0.5f, extent, 0.08f, 0.2f, 0.2f) == 1.0f,
           "short, cheap hits at the screen center have full confidence");
    expect(ssr::hitConfidence(extent * 0.5f, extent, 0.08f, 0.2f, 1.0f) == 0.0f,
           "hits at the end of the iteration budget fade out");
    expect(ssr::hitConfidence(extent * 0.5f, extent, 0.08f, 1.0f, 0.2f) == 0.0f,
           "hits at the end of the ray's reach fade out");

    bool weightBounded = true;
    float grazingWeight = 0.0f;
    const ssr::CompositeSettings bright{.roughnessFadeStart = 0.35f, .roughnessFadeEnd = 0.6f, .intensity = 50.0f};
    const glm::vec3 dielectric = scene_specular::reflectanceAtNormal(glm::vec3(0.8f, 0.2f, 0.1f), 0.0f);
    const glm::vec3 overbright(2.0f, -1.0f, 0.5f);
    const auto inUnitRange = [](glm::vec3 weight) {
        return glm::all(glm::greaterThanEqual(weight, glm::vec3(0.0f))) &&
               glm::all(glm::lessThanEqual(weight, glm::vec3(1.0f)));
    };
    for (float confidence : {-1.0f, 0.0f, 0.5f, 1.0f, 4.0f}) {
        for (float normalDotView = -0.2f; normalDotView <= 1.2f; normalDotView += 0.05f) {
            for (float roughness = 0.0f; roughness <= 1.0f; roughness += 0.1f) {
                for (const ssr::CompositeSettings& settings : {ssr::CompositeSettings{}, bright}) {
                    for (const glm::vec3& reflectance : {dielectric, overbright}) {
                        weightBounded =
                            weightBounded && inUnitRange(ssr::compositeWeight(confidence, normalDotView, roughness,
                                                                              reflectance, settings));
                    }
                }
            }
        }
        grazingWeight = std::max(grazingWeight, ssr::compositeWeight(confidence, 0.0f, 0.045f, dielectric, {}).x);
    }
    expect(weightBounded, "the composite weight never leaves [0, 1]");
    expect(ssr::compositeWeight(0.0f, 0.5f, 0.1f, dielectric, {}) == glm::vec3(0.0f),
           "a miss leaves the scene unchanged");
    const glm::vec3 headOnDielectric = ssr::compositeWeight(1.0f, 1.0f, 0.045f, dielectric, {});
    expect(glm::all(glm::lessThan(glm::abs(headOnDielectric - glm::vec3(0.04f)), glm::vec3(1e-4f))),
           "head-on dielectric reflections use an untinted F0 = 0.04");
    expect(grazingWeight > 0.9f, "smooth grazing reflections approach full weight");
}

// Metals store their base color as F0, so reflections take on the metal's
// tint; the stored representation keeps dielectric F0 close to 0.04.
void testMetalReflectionsAreTinted() {
    const glm::vec3 gold(1.0f, 0.78f, 0.34f);
    const glm::vec3 stored = scene_specular::storeAndLoadReflectance(scene_specular::reflectanceAtNormal(gold, 1.0f));
    expect(glm::all(glm::lessThan(glm::abs(stored - gold), glm::vec3(0.01f))), "metal F0 is the stored base color");
    const glm::vec3 storedDielectric =
        scene_specular::storeAndLoadReflectance(scene_specular::reflectanceAtNormal(gold, 0.0f));
    expect(glm::all(glm::lessThan(glm::abs(storedDielectric - glm::vec3(0.04f)), glm::vec3(0.002f))),
           "dielectric F0 survives the 8-bit sRGB store");

    const glm::vec3 headOn = ssr::compositeWeight(1.0f, 1.0f, 0.045f, stored, {});
    expect(glm::all(glm::lessThan(glm::abs(headOn - stored), glm::vec3(1e-4f))),
           "head-on metal reflections are weighted by the metal's F0");
    expect(headOn.r > headOn.g && headOn.g > headOn.b, "gold reflections are tinted towards red and green");
    expect(headOn.b > 0.04f, "metals reflect far more than dielectrics");
    const glm::vec3 grazing = ssr::compositeWeight(1.0f, 0.0f, 0.045f, stored, {});
    expect(glm::all(glm::greaterThan(grazing, glm::vec3(0.9f))), "metal reflections lose their tint at grazing angles");
    const glm::vec3 rough = ssr::compositeWeight(1.0f, 1.0f, 0.8f, stored, {});
    expect(rough == glm::vec3(0.0f), "rough metals fade out of screen-space reflections");

    // The lit metal holds its IBL specular, environment * F0. A confident
    // head-on hit replaces it with the reflected radiance * F0, leaving no
    // remainder of the (here much brighter) environment in any channel.
    const glm::vec3 environment(8.0f);
    const glm::vec3 reflected(0.5f, 0.4f, 0.3f);
    const glm::vec3 blendedMetal =
        ssr::blendReflection(environment * gold, reflected, ssr::compositeWeight(1.0f, 1.0f, 0.045f, gold, {}));
    expect(glm::all(glm::lessThan(glm::abs(blendedMetal - reflected * gold), glm::vec3(1e-4f))),
           "a confident metal reflection replaces the metal's IBL specular with tinted radiance");

    // Grey (dielectric) weights blend exactly like a scalar mix.
    const glm::vec3 scene(0.6f, 0.3f, 0.1f);
    const glm::vec3 dielectricWeight = ssr::compositeWeight(0.7f, 0.6f, 0.2f, glm::vec3(0.04f), {});
    const glm::vec3 blendedDielectric = ssr::blendReflection(scene, reflected, dielectricWeight);
    expect(glm::all(glm::lessThan(glm::abs(blendedDielectric - glm::mix(scene, reflected, dielectricWeight.x)),
                                  glm::vec3(1e-6f))),
           "dielectric reflections blend as before");
}

// A shallow floor reflection: the ray hugs the floor in screen space, so
// coarse cells keep descending; it must still reach the far wall in budget.
void testGrazingFloorRayReachesWall() {
    const ssr::PerspectiveTerms terms = ssr::perspectiveTerms(sceneProjection({0.0021f, 0.0013f}));
    SyntheticScene scene;
    scene.planes.push_back({{0, 1, 0}, -1.7f, {-100, -1.7f, -30}, {100, -1.7f, 0}});
    scene.planes.push_back({{0, 0, 1}, -30.0f, {-100, -1.7f, -30}, {100, 5, -30}});
    scene.render(terms);
    const glm::vec3 origin(0.5f, -1.7f, -8.0f);
    const glm::vec3 direction = glm::reflect(-glm::normalize(-origin), glm::vec3(0, 1, 0));
    const std::optional<ssr::ScreenRay> ray =
        ssr::makeScreenRay(origin + glm::vec3(0, 0.016f, 0), direction, 60.0f, terms, glm::vec2(renderExtent));
    if (!ray) {
        expect(false, "grazing ray exists");
        return;
    }
    const ssr::TraceResult result = trace(scene, *ray, terms, direction);
    const float travel = (30.0f - 8.0f) / -direction.z;
    const glm::vec2 expected = projectPixel(origin + direction * travel, terms);
    expect(result.hit && glm::length(glm::vec2(result.position) - expected) < 2.0f,
           "a grazing floor reflection reaches the far wall");
    expect(result.iterations < ssr::TraceSettings{}.maximumIterations * 7 / 10,
           "the grazing trace stays clear of the iteration fade");
}

void testReflectionsRequireDepthPrepass() {
    RenderSettings requested;
    requested.reflections = ReflectionQuality::ScreenSpaceHalfResolution;
    const ValidatedRenderSettings withPrepass = validateRenderSettings(requested, RenderCapabilities{});
    expect(withPrepass.settings.reflections == ReflectionQuality::ScreenSpaceHalfResolution,
           "screen-space reflections stay enabled with the depth prepass");
    requested.depthPrepass = false;
    const ValidatedRenderSettings withoutPrepass = validateRenderSettings(requested, RenderCapabilities{});
    expect(withoutPrepass.settings.reflections == ReflectionQuality::ProbesOnly &&
               withoutPrepass.fallbacks.size() == withPrepass.fallbacks.size() + 1,
           "screen-space reflections fall back to probes and IBL without the depth prepass");
}
} // namespace

int main() {
    testReflectionsRequireDepthPrepass();
    testGrazingFloorRayReachesWall();
    testViewPositionRoundTrip();
    testPyramidOrdering();
    testRayHitsWall();
    testRayAboveSceneMisses();
    testThicknessRejectsOccluderInFront();
    testRayClipsToNearPlane();
    testFadesAndCompositeWeight();
    testMetalReflectionsAreTinted();
    if (failures != 0) {
        std::cerr << failures << " screen-space reflection test(s) failed\n";
        return 1;
    }
    std::cout << "Screen-space reflection tests passed\n";
    return 0;
}
