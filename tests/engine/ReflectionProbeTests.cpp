#include "engine/graphics/Lighting.hpp"
#include "engine/graphics/ReflectionProbes.hpp"
#include "engine/graphics/RenderConfiguration.hpp"
#include "engine/scene/Scene.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

bool near(float actual, float expected, float tolerance) {
    return std::abs(actual - expected) <= tolerance;
}

bool near(const glm::vec3& actual, const glm::vec3& expected, float tolerance) {
    return glm::all(glm::lessThanEqual(glm::abs(actual - expected), glm::vec3(tolerance)));
}

constexpr float nearPlane = 0.1f;
constexpr float farPlane = 200.0f;
const glm::uvec2 renderExtent{960, 540};

// Matches the main-view projection in MeshPass: reversed Z and Vulkan Y.
glm::mat4 sceneProjection() {
    glm::mat4 projection = glm::perspectiveRH_ZO(
        glm::radians(60.0f), static_cast<float>(renderExtent.x) / static_cast<float>(renderExtent.y), farPlane,
        nearPlane);
    projection[1][1] *= -1.0f;
    return projection;
}

// Independent slab test: distance along a ray from inside a box to its exit,
// skipping axes the ray does not move along.
float exitDistance(const glm::vec3& origin, const glm::vec3& direction, const glm::vec3& minimum,
                   const glm::vec3& maximum) {
    float result = 1.0e30f;
    for (int axis = 0; axis < 3; ++axis) {
        if (direction[axis] > 0.0f) {
            result = std::min(result, (maximum[axis] - origin[axis]) / direction[axis]);
        } else if (direction[axis] < 0.0f) {
            result = std::min(result, (minimum[axis] - origin[axis]) / direction[axis]);
        }
    }
    return result;
}

glm::vec3 randomDirection(std::mt19937& random) {
    std::normal_distribution<float> normal(0.0f, 1.0f);
    glm::vec3 direction{normal(random), normal(random), normal(random)};
    return glm::normalize(direction);
}

void testBoxProjection() {
    const glm::vec3 center{1.0f, 2.0f, -3.0f};
    const glm::vec3 halfExtents{2.0f, 1.0f, 3.0f};
    expect(near(boxProjectReflection(center + glm::vec3(1.0f, 0.0f, 0.0f), {1.0f, 0.0f, 0.0f}, center, halfExtents),
                {1.0f, 0.0f, 0.0f}, 1.0e-5f),
           "a ray along an axis projects onto that axis");
    const glm::vec3 diagonal = glm::normalize(glm::vec3(1.0f, 1.0f, 0.0f));
    expect(near(boxProjectReflection(center + glm::vec3(1.0f, 0.0f, 0.0f), diagonal, center, halfExtents),
                glm::normalize(glm::vec3(2.0f, 1.0f, 0.0f)), 1.0e-5f),
           "an off-center ray points from the capture center to where it leaves the box");
    expect(near(boxProjectReflection(center, diagonal, center, halfExtents), diagonal, 1.0e-5f),
           "rays from the capture center are unchanged");
    expect(near(boxProjectReflection(center + glm::vec3(0.0f, 0.0f, 2.0f), {0.0f, 0.0f, -1.0f}, center, halfExtents),
                {0.0f, 0.0f, -1.0f}, 1.0e-5f),
           "zero direction components stay finite");
    // A point in the blend region projects as if it stood on the box surface.
    expect(near(boxProjectReflection(center + glm::vec3(4.0f, 0.5f, 0.0f), diagonal, center, halfExtents),
                boxProjectReflection(center + glm::vec3(2.0f, 0.5f, 0.0f), diagonal, center, halfExtents), 1.0e-5f),
           "points outside the box are clamped onto it first");

    std::mt19937 random(7);
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
    bool matches = true;
    for (int sample = 0; sample < 2000; ++sample) {
        const glm::vec3 position = center + glm::vec3(unit(random), unit(random), unit(random)) * halfExtents;
        const glm::vec3 direction = randomDirection(random);
        const float distance = exitDistance(position, direction, center - halfExtents, center + halfExtents);
        const glm::vec3 expected = glm::normalize(position + direction * distance - center);
        matches = matches && near(boxProjectReflection(position, direction, center, halfExtents), expected, 1.0e-3f);
    }
    expect(matches, "box projection matches an independent slab intersection");
}

Entity addProbe(Scene& scene, const char* name, const glm::vec3& center, const glm::vec3& halfExtents,
                float blendDistance, int32_t priority) {
    const Entity entity = scene.createEntity(name);
    scene.registry().get<TransformComponent>(entity).world = glm::translate(glm::mat4(1.0f), center);
    scene.registry().emplace<ReflectionProbeComponent>(
        entity,
        ReflectionProbeComponent{.halfExtents = halfExtents, .blendDistance = blendDistance, .priority = priority});
    return entity;
}

float weightSum(const ReflectionProbeShading& shading) {
    float total = shading.environmentWeight;
    for (const ReflectionProbeWeight& probe : shading.probes) {
        total += probe.weight;
    }
    return total;
}

void testWeightNormalization() {
    Scene scene;
    addProbe(scene, "Left", {-2.0f, 0.0f, 0.0f}, glm::vec3(2.0f), 1.5f, 0);
    addProbe(scene, "Right", {2.0f, 0.0f, 0.0f}, glm::vec3(2.0f), 1.5f, 0);
    addProbe(scene, "Important", {0.0f, 0.0f, 6.0f}, glm::vec3(1.0f), 4.0f, 5);
    const std::vector<ReflectionProbeVolume> probes = gatherReflectionProbes(scene);
    expect(probes.size() == 3, "every probe is gathered");

    std::mt19937 random(11);
    std::uniform_real_distribution<float> coordinate(-6.0f, 10.0f);
    bool sceneMatches = true;
    bool normalized = true;
    for (int sample = 0; sample < 500; ++sample) {
        const glm::vec3 point{coordinate(random), coordinate(random) * 0.3f, coordinate(random)};
        const std::vector<ProbeBlend> fromScene = blendReflectionProbes(scene, point);
        const std::vector<ReflectionProbeWeight> fromVolumes = blendReflectionProbeVolumes(probes, point);
        sceneMatches = sceneMatches && fromScene.size() == fromVolumes.size();
        float total = 0.0f;
        for (size_t index = 0; sceneMatches && index < fromScene.size(); ++index) {
            sceneMatches = fromScene[index].entity == probes[fromVolumes[index].probe].entity &&
                           fromScene[index].weight == fromVolumes[index].weight &&
                           fromScene[index].influence == fromVolumes[index].influence;
            total += fromVolumes[index].weight;
        }
        const bool anyInfluence = std::ranges::any_of(
            fromVolumes, [](const ReflectionProbeWeight& weight) { return weight.influence > 0.0f; });
        normalized = normalized && (!anyInfluence || near(total, 1.0f, 1.0e-5f));
        const ReflectionProbeShading shading = shadeReflectionProbes(probes, point);
        normalized = normalized && near(weightSum(shading), 1.0f, 1.0e-5f);
    }
    expect(sceneMatches, "blendReflectionProbes matches the volume blend it delegates to");
    expect(normalized, "probe weights normalize, and probe plus environment weights sum to one");

    // The left and right boxes touch at x = 0, so both fully influence the
    // seam and share it evenly.
    const ReflectionProbeShading seam = shadeReflectionProbes(probes, {0.0f, 0.0f, 0.0f});
    expect(seam.probes.size() == 2 && near(seam.probes[0].weight, 0.5f, 1.0e-5f) && seam.environmentWeight == 0.0f,
           "points inside the boxes leave nothing to the scene environment");
    expect(seam.probes[0].probe == 0 && seam.probes[1].probe == 1, "equal influences keep entity order");

    // Halfway across the left probe's blend distance, outside the other boxes.
    const ReflectionProbeShading fading = shadeReflectionProbes(probes, {-4.75f, 0.0f, 0.0f});
    expect(fading.probes.size() == 1 && near(fading.probes[0].weight, 0.5f, 1.0e-5f) &&
               near(fading.environmentWeight, 0.5f, 1.0e-5f),
           "across the blend distance the scene environment fades in");

    const ReflectionProbeShading outside = shadeReflectionProbes(probes, {0.0f, 30.0f, 0.0f});
    expect(outside.probes.empty() && outside.environmentWeight == 1.0f,
           "points beyond every probe use only the scene environment");

    // Priority wins the ordering even with the least influence.
    const std::vector<ReflectionProbeWeight> prioritized = blendReflectionProbeVolumes(probes, {1.0f, 0.0f, 2.5f});
    expect(prioritized.size() == 3 && prioritized[0].probe == 2 && prioritized[0].influence < prioritized[1].influence,
           "higher priority probes sort first");

    std::vector<ReflectionProbeVolume> crowded;
    for (int index = 0; index < 6; ++index) {
        crowded.push_back({.halfExtents = glm::vec3(1.0f), .blendDistance = 1.0f, .priority = index});
    }
    const std::vector<ReflectionProbeWeight> kept = blendReflectionProbeVolumes(crowded, glm::vec3(0.0f));
    expect(kept.size() == maximumBlendedReflectionProbes && kept.front().probe == 5 && kept.back().probe == 2,
           "only the highest-priority probes up to the blend limit are kept");
}

void testResidentSelection() {
    std::vector<ReflectionProbeVolume> probes;
    for (int index = 0; index < 12; ++index) {
        probes.push_back({.center = {static_cast<float>(index) * 10.0f, 0.0f, 0.0f},
                          .halfExtents = glm::vec3(2.0f),
                          .blendDistance = 1.0f,
                          .priority = 0});
    }
    // Probe 11 contains the camera; the others recede along +x.
    probes[11].center = glm::vec3(0.0f, 0.0f, 50.0f);
    const glm::vec3 camera{0.0f, 0.0f, 50.0f};
    const std::vector<uint32_t> selected =
        selectResidentReflectionProbes(probes, camera, maximumResidentReflectionProbes);
    expect(selected.size() == maximumResidentReflectionProbes, "selection fills the resident limit");
    expect(std::ranges::is_sorted(selected), "selected probes keep span order");
    expect(std::ranges::find(selected, 11u) != selected.end(), "the probe around the camera stays resident");
    expect(std::ranges::find(selected, 10u) == selected.end() && std::ranges::find(selected, 9u) == selected.end(),
           "the farthest probes are dropped");
    expect(selectResidentReflectionProbes(probes, camera, 20).size() == probes.size(),
           "small scenes keep every probe resident");
}

// A random world-space point inside the view frustum up to maximumDepth.
glm::vec3 sampleFrustum(const ClusterGrid& grid, const glm::mat4& inverseView, std::mt19937& random, float maximumDepth,
                        glm::vec2& pixel, float& depth) {
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    pixel = glm::vec2(unit(random), unit(random)) * grid.renderExtent;
    depth = grid.nearDepth * std::pow(maximumDepth / grid.nearDepth, unit(random));
    const glm::vec2 ndc = pixel / grid.renderExtent * 2.0f - 1.0f;
    const glm::vec2 ray = (ndc + grid.projectionOffset) / grid.projectionScale;
    return glm::vec3(inverseView * glm::vec4(ray * depth, -depth, 1.0f));
}

void testClusterAssignment() {
    const glm::mat4 view =
        glm::lookAtRH(glm::vec3(1.0f, 2.5f, 6.0f), glm::vec3(-2.0f, 0.5f, -8.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 inverseView = glm::inverse(view);
    const ClusterGrid grid =
        makeClusterGrid(ClusterGridConfiguration{}, sceneProjection(), renderExtent, nearPlane, farPlane);
    std::mt19937 random(23);
    std::uniform_real_distribution<float> lateral(-10.0f, 10.0f);
    std::uniform_real_distribution<float> forward(-30.0f, 4.0f);
    std::uniform_real_distribution<float> extent(0.5f, 4.0f);
    std::uniform_real_distribution<float> blend(0.0f, 2.0f);
    std::vector<ReflectionProbeVolume> probes;
    for (uint32_t index = 0; index < maximumResidentReflectionProbes; ++index) {
        probes.push_back({.center = {lateral(random), lateral(random) * 0.3f, forward(random)},
                          .halfExtents = {extent(random), extent(random), extent(random)},
                          .blendDistance = blend(random)});
    }
    // One probe behind the camera touches no cluster.
    probes.back().center = glm::vec3(inverseView * glm::vec4(0.0f, 0.0f, 40.0f, 1.0f));
    probes.back().halfExtents = glm::vec3(1.0f);
    probes.back().blendDistance = 0.5f;

    std::vector<ClusterBounds> viewBounds;
    for (const ReflectionProbeVolume& probe : probes) {
        viewBounds.push_back(reflectionProbeViewBounds(probe, view));
    }
    bool boundsContain = true;
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
    for (const ReflectionProbeVolume& probe : probes) {
        const ClusterBounds bounds = reflectionProbeViewBounds(probe, view);
        for (int sample = 0; sample < 200; ++sample) {
            const glm::vec3 offset =
                glm::vec3(unit(random), unit(random), unit(random)) * (probe.halfExtents + probe.blendDistance);
            const glm::vec3 viewPoint = glm::vec3(view * glm::vec4(probe.center + offset, 1.0f));
            boundsContain = boundsContain && glm::all(glm::greaterThanEqual(viewPoint, bounds.minimum - 1.0e-4f)) &&
                            glm::all(glm::lessThanEqual(viewPoint, bounds.maximum + 1.0e-4f));
        }
    }
    expect(boundsContain, "view bounds contain the whole influence volume");

    const std::vector<uint32_t> masks = assignReflectionProbesToClusters(grid, viewBounds);
    expect(masks.size() == grid.clusterCount(), "every cluster gets a mask");

    // Brute force: any frustum point a probe reaches must find that probe in
    // the mask of the cluster it shades.
    bool conservative = true;
    uint32_t reachedSamples = 0;
    for (int sample = 0; sample < 20000; ++sample) {
        glm::vec2 pixel;
        float depth = 0.0f;
        const glm::vec3 point = sampleFrustum(grid, inverseView, random, 40.0f, pixel, depth);
        const uint32_t mask = masks[grid.clusterIndexAt(pixel, depth)];
        for (uint32_t probe = 0; probe < probes.size(); ++probe) {
            if (reflectionProbeReaches(probes[probe], point)) {
                ++reachedSamples;
                conservative = conservative && (mask & (1u << probe)) != 0;
            }
        }
    }
    expect(reachedSamples > 100, "the random probes cover part of the frustum");
    expect(conservative, "cluster masks never miss a probe that reaches a point in the cluster");

    const uint32_t behindBit = 1u << (probes.size() - 1);
    expect(std::ranges::none_of(masks, [&](uint32_t mask) { return (mask & behindBit) != 0; }),
           "a probe behind the camera is assigned to no cluster");
    // Each cluster's mask equals the direct per-cluster test.
    bool direct = true;
    for (uint32_t slice = 0; slice < grid.configuration.depthSlices; slice += 5) {
        for (uint32_t row = 0; row < grid.configuration.rows; ++row) {
            for (uint32_t column = 0; column < grid.configuration.columns; ++column) {
                uint32_t expected = 0;
                for (uint32_t probe = 0; probe < viewBounds.size(); ++probe) {
                    if (reflectionProbeAffectsCluster(viewBounds[probe], grid.bounds(column, row, slice))) {
                        expected |= 1u << probe;
                    }
                }
                direct = direct && masks[grid.clusterIndex(column, row, slice)] == expected;
            }
        }
    }
    expect(direct, "masks match the per-cluster overlap test");
}

void testSettings() {
    RenderSettings requested;
    expect(requested.reflectionProbeBlending == ReflectionProbeBlending::PerPixel, "per-pixel blending is default");
    requested.reflectionProbeBlending = ReflectionProbeBlending::CameraOnly;
    requested.budgets.reflectionProbeClusters = {0.0, 0.15};
    std::string document;
    expect(serializeRenderSettings(requested, document).success, "settings serialize");
    RenderSettings decoded;
    expect(deserializeRenderSettings(document, decoded).success &&
               decoded.reflectionProbeBlending == ReflectionProbeBlending::CameraOnly &&
               decoded.budgets.reflectionProbeClusters.gpuMilliseconds == 0.15,
           "the blending mode and its budget round trip");
    // A document written before the field existed.
    std::string legacyDocument = document;
    const size_t field = legacyDocument.find("\"reflectionProbeBlending\": 0,");
    expect(field != std::string::npos, "the blending mode is serialized by value");
    if (field != std::string::npos) {
        legacyDocument.erase(field, std::string("\"reflectionProbeBlending\": 0,").size());
    }
    RenderSettings legacy;
    expect(deserializeRenderSettings(legacyDocument, legacy).success &&
               legacy.reflectionProbeBlending == ReflectionProbeBlending::PerPixel,
           "documents without the field use per-pixel blending");
    std::string invalidDocument = document;
    if (field != std::string::npos) {
        invalidDocument.replace(field, std::string("\"reflectionProbeBlending\": 0,").size(),
                                "\"reflectionProbeBlending\": 7,");
    }
    RenderSettings invalid;
    expect(!deserializeRenderSettings(invalidDocument, invalid).success, "unknown blending modes are rejected");

    RenderCapabilities capabilities;
    capabilities.reflectionProbes = true;
    capabilities.perPixelReflectionProbes = false;
    RenderSettings perPixel;
    const ValidatedRenderSettings fallback = validateRenderSettings(perPixel, capabilities);
    expect(fallback.settings.reflectionProbeBlending == ReflectionProbeBlending::CameraOnly &&
               fallback.fallbacks.size() == 1,
           "unsupported per-pixel blending falls back to the camera blend");
    capabilities.perPixelReflectionProbes = true;
    expect(validateRenderSettings(perPixel, capabilities).fallbacks.empty(), "supported per-pixel blending is kept");
}
} // namespace

int main() {
    testBoxProjection();
    testWeightNormalization();
    testResidentSelection();
    testClusterAssignment();
    testSettings();
    if (failures == 0) {
        std::cout << "Reflection probe tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
