#include "engine/graphics/Lighting.hpp"
#include "engine/graphics/RenderConfiguration.hpp"

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

constexpr float nearPlane = 0.1f;
constexpr float farPlane = 1000.0f;
// An odd internal extent, as dynamic resolution produces.
const glm::uvec2 renderExtent{1000, 563};

// Matches the main-view projection in MeshPass: reversed Z, Vulkan Y, and a
// temporal jitter offset.
glm::mat4 sceneProjection() {
    glm::mat4 projection = glm::perspectiveRH_ZO(
        glm::radians(60.0f), static_cast<float>(renderExtent.x) / static_cast<float>(renderExtent.y), farPlane,
        nearPlane);
    projection[1][1] *= -1.0f;
    projection[2][0] += 0.0007f;
    projection[2][1] -= 0.0011f;
    return projection;
}

ClusterGrid sceneGrid() {
    return makeClusterGrid(ClusterGridConfiguration{}, sceneProjection(), renderExtent, nearPlane, farPlane);
}

bool contains(const ClusterBounds& bounds, const glm::vec3& point, float tolerance) {
    return glm::all(glm::greaterThanEqual(point, bounds.minimum - tolerance)) &&
           glm::all(glm::lessThanEqual(point, bounds.maximum + tolerance));
}

bool listed(const ClusterLightLists& lists, uint32_t cluster, uint32_t light) {
    const glm::uvec2 range = lists.ranges[cluster];
    const auto begin = lists.lightIndices.begin() + range.x;
    return std::find(begin, begin + range.y, light) != begin + range.y;
}

// A random view-space point inside the view frustum, at most maxDepth away.
struct FrustumSample {
    glm::vec2 pixel;
    float depth;
    glm::vec3 viewPosition;
};

FrustumSample sampleFrustum(const ClusterGrid& grid, std::mt19937& random, float maxDepth) {
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const glm::vec2 pixel = glm::vec2(unit(random), unit(random)) * grid.renderExtent;
    const float depth = grid.nearDepth * std::pow(maxDepth / grid.nearDepth, unit(random));
    const glm::vec2 ndc = pixel / grid.renderExtent * 2.0f - 1.0f;
    const glm::vec2 ray = (ndc + grid.projectionOffset) / grid.projectionScale;
    return {pixel, depth, glm::vec3(ray * depth, -depth)};
}

std::vector<LocalLight> randomViewSpaceLights(std::mt19937& random, uint32_t count) {
    std::uniform_real_distribution<float> lateral(-30.0f, 30.0f);
    std::uniform_real_distribution<float> depth(-80.0f, 4.0f);
    std::uniform_real_distribution<float> range(0.5f, 12.0f);
    std::uniform_real_distribution<float> signedUnit(-1.0f, 1.0f);
    std::uniform_real_distribution<float> cone(0.15f, 1.2f);
    std::vector<LocalLight> lights(count);
    for (uint32_t index = 0; index < count; ++index) {
        LocalLight& light = lights[index];
        light.position = {lateral(random), lateral(random) * 0.5f, depth(random)};
        light.range = range(random);
        light.stableId = index + 1;
        if (index % 3 == 0) {
            light.type = LocalLightType::Spot;
            glm::vec3 direction{signedUnit(random), signedUnit(random), signedUnit(random)};
            light.direction =
                glm::dot(direction, direction) > 0.01f ? glm::normalize(direction) : glm::vec3(0.0f, 0.0f, -1.0f);
            const float outer = cone(random);
            light.outerConeCosine = std::cos(outer);
            light.innerConeCosine = std::cos(outer * 0.7f);
        }
    }
    return lights;
}

bool lit(const LocalLight& light, const glm::vec3& point) {
    const glm::vec3 toPoint = point - light.position;
    const float distance = glm::length(toPoint);
    if (distance >= light.range) {
        return false;
    }
    if (light.type != LocalLightType::Spot || distance < 0.0001f) {
        return true;
    }
    return glm::dot(toPoint / distance, light.direction) > light.outerConeCosine;
}

void testSliceBoundaries() {
    const ClusterGrid grid = sceneGrid();
    const uint32_t slices = grid.configuration.depthSlices;
    expect(grid.clusterCount() == 16u * 9u * 24u, "the default grid is 16x9 tiles by 24 depth slices");
    expect(std::abs(grid.sliceStart(0) - nearPlane) < 1e-6f && grid.sliceStart(slices) == farPlane,
           "slices cover the view depth range from the near plane to the far plane");
    expect(std::abs(grid.sliceEndDepth - 500.0f) < 1e-3f &&
               std::abs(grid.sliceStart(slices - 1) - nearPlane * std::pow(5000.0f, 23.0f / 24.0f)) < 1e-2f,
           "exponential slicing ends at the configured depth and the last slice extends to the far plane");
    bool monotone = true;
    bool exponential = true;
    bool lookupMatches = true;
    const float ratio = grid.sliceStart(1) / grid.sliceStart(0);
    for (uint32_t slice = 0; slice < slices; ++slice) {
        monotone = monotone && grid.sliceStart(slice) < grid.sliceStart(slice + 1);
        if (slice + 1 < slices) {
            exponential =
                exponential && std::abs(grid.sliceStart(slice + 1) / grid.sliceStart(slice) - ratio) < ratio * 1e-4f;
        }
        const float start = grid.sliceStart(slice);
        const float end = grid.sliceStart(slice + 1);
        lookupMatches = lookupMatches && grid.sliceIndex(start * 1.001f) == slice &&
                        grid.sliceIndex(end * 0.999f) == slice && grid.sliceIndex(std::sqrt(start * end)) == slice;
    }
    expect(monotone && exponential, "slice boundaries increase with a constant ratio");
    expect(lookupMatches, "the shader slice lookup inverts the slice boundaries");
    expect(grid.sliceIndex(0.0f) == 0 && grid.sliceIndex(farPlane * 2.0f) == slices - 1,
           "depths outside the view range clamp to the first and last slices");

    const ClusterGrid shortView =
        makeClusterGrid(ClusterGridConfiguration{}, sceneProjection(), renderExtent, nearPlane, 50.0f);
    expect(shortView.sliceEndDepth == 50.0f && shortView.sliceStart(shortView.configuration.depthSlices) == 50.0f,
           "a far plane nearer than the slicing limit bounds the slices");
}

void testFragmentClusterMirror() {
    const ClusterGrid grid = sceneGrid();
    const glm::mat4 projection = sceneProjection();
    std::mt19937 random(7);
    bool depthMatches = true;
    bool clusterContainsPoint = true;
    for (int sample = 0; sample < 4000; ++sample) {
        const FrustumSample point = sampleFrustum(grid, random, farPlane);
        // Reconstruct what mesh.frag sees: gl_FragCoord.xy and reversed-Z depth.
        const glm::vec4 clip = projection * glm::vec4(point.viewPosition, 1.0f);
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        const float viewDepth = projection[3][2] / (ndc.z + projection[2][2]);
        depthMatches =
            depthMatches && std::abs(viewDepth - point.depth) <= point.depth * 1e-3f && ndc.z > 0.0f && ndc.z <= 1.0f;
        const glm::vec2 pixel = (glm::vec2(ndc) * 0.5f + 0.5f) * grid.renderExtent;
        const uint32_t cluster = grid.clusterIndexAt(pixel, viewDepth);
        const uint32_t columns = grid.configuration.columns;
        const uint32_t rows = grid.configuration.rows;
        const ClusterBounds bounds =
            grid.bounds(cluster % columns, (cluster / columns) % rows, cluster / (columns * rows));
        clusterContainsPoint = clusterContainsPoint && contains(bounds, point.viewPosition, point.depth * 1e-3f);
    }
    expect(depthMatches, "view depth reconstructs from reversed-Z fragment depth");
    expect(clusterContainsPoint, "a fragment's cluster bounds contain the fragment's view position");
    expect(grid.clusterIndexAt({0.0f, 0.0f}, nearPlane) == 0 &&
               grid.clusterIndexAt(glm::vec2(renderExtent), farPlane) == grid.clusterCount() - 1,
           "cluster indices run column-major within rows within slices and clamp at the extent");
}

void testAssignmentMatchesBruteForce() {
    const ClusterGrid grid = sceneGrid();
    std::mt19937 random(11);
    const std::vector<LocalLight> lights = randomViewSpaceLights(random, 96);
    const ClusterLightLists lists = assignLightsToClusters(grid, lights);
    expect(lists.discardedAssignments == 0, "a moderate light count fits every cluster");

    bool conservative = true;
    for (int sample = 0; sample < 20000; ++sample) {
        const FrustumSample point = sampleFrustum(grid, random, 120.0f);
        const uint32_t cluster = grid.clusterIndexAt(point.pixel, point.depth);
        for (uint32_t light = 0; light < lights.size(); ++light) {
            if (lit(lights[light], point.viewPosition) && !listed(lists, cluster, light)) {
                conservative = false;
            }
        }
    }
    expect(conservative, "every light that reaches a point is listed in that point's cluster");

    bool exact = true;
    uint64_t assignments = 0;
    const ClusterGridConfiguration& size = grid.configuration;
    for (uint32_t slice = 0; slice < size.depthSlices; ++slice) {
        for (uint32_t row = 0; row < size.rows; ++row) {
            for (uint32_t column = 0; column < size.columns; ++column) {
                const uint32_t cluster = grid.clusterIndex(column, row, slice);
                const ClusterBounds bounds = grid.bounds(column, row, slice);
                std::vector<uint32_t> expected;
                for (uint32_t light = 0; light < lights.size(); ++light) {
                    if (localLightAffectsCluster(lights[light], bounds)) {
                        expected.push_back(light);
                    }
                }
                const glm::uvec2 range = lists.ranges[cluster];
                exact = exact && range.x == cluster * maximumLightsPerCluster &&
                        std::equal(expected.begin(), expected.end(), lists.lightIndices.begin() + range.x,
                                   lists.lightIndices.begin() + range.x + range.y) &&
                        expected.size() == range.y;
                assignments += range.y;
            }
        }
    }
    expect(exact, "cluster lists hold exactly the intersecting lights in ascending order");
    expect(assignments < static_cast<uint64_t>(grid.clusterCount()) * lights.size() / 4,
           "clusters cull most lights instead of listing every light everywhere");
}

void testSpotConeCulling() {
    const ClusterGrid grid = sceneGrid();
    LocalLight sideways{.type = LocalLightType::Spot,
                        .position = {0.0f, 0.0f, -10.0f},
                        .direction = {1.0f, 0.0f, 0.0f},
                        .range = 8.0f,
                        .innerConeCosine = std::cos(0.3f),
                        .outerConeCosine = std::cos(0.4f)};
    LocalLight point = sideways;
    point.type = LocalLightType::Point;
    const LocalLight lights[]{sideways, point};
    const ClusterLightLists lists = assignLightsToClusters(grid, lights);
    uint32_t spotClusters = 0;
    uint32_t pointClusters = 0;
    bool spotWithinPoint = true;
    for (uint32_t cluster = 0; cluster < grid.clusterCount(); ++cluster) {
        const bool spotListed = listed(lists, cluster, 0);
        const bool pointListed = listed(lists, cluster, 1);
        spotClusters += spotListed ? 1u : 0u;
        pointClusters += pointListed ? 1u : 0u;
        spotWithinPoint = spotWithinPoint && (!spotListed || pointListed);
    }
    expect(spotWithinPoint && spotClusters < pointClusters / 2,
           "a spot cone culls clusters its range sphere would reach");
    const uint32_t behind = grid.clusterIndexAt(grid.renderExtent * glm::vec2(0.25f, 0.5f), 10.0f);
    const uint32_t ahead = grid.clusterIndexAt(grid.renderExtent * glm::vec2(0.75f, 0.5f), 10.0f);
    expect(listed(lists, ahead, 0) && !listed(lists, behind, 0),
           "a spot reaches clusters along its axis but not those behind it");

    std::mt19937 random(3);
    bool conservative = true;
    for (int sample = 0; sample < 20000; ++sample) {
        const FrustumSample sampled = sampleFrustum(grid, random, 20.0f);
        if (lit(sideways, sampled.viewPosition) &&
            !listed(lists, grid.clusterIndexAt(sampled.pixel, sampled.depth), 0)) {
            conservative = false;
        }
    }
    expect(conservative, "spot cone culling never drops a lit point");
}

void testClusterCapacity() {
    const ClusterGrid grid = sceneGrid();
    std::vector<LocalLight> lights(maximumLightsPerCluster + 40);
    for (uint32_t index = 0; index < lights.size(); ++index) {
        lights[index].position = {0.0f, 0.0f, -5.0f};
        lights[index].range = 2.0f;
        lights[index].stableId = index + 1;
    }
    const ClusterLightLists lists = assignLightsToClusters(grid, lights);
    const uint32_t center = grid.clusterIndexAt(grid.renderExtent * 0.5f, 5.0f);
    expect(lists.ranges[center].y == maximumLightsPerCluster && lists.maximumLightsInCluster == lights.size() &&
               lists.discardedAssignments > 0 && lists.lightIndices[lists.ranges[center].x] == 0,
           "full clusters keep their first lights and report the dropped assignments");
}

void testLightSelection() {
    std::vector<LocalLight> lights(10);
    for (uint32_t index = 0; index < lights.size(); ++index) {
        lights[index].position = {static_cast<float>(index) * 10.0f, 0.0f, 0.0f};
        lights[index].range = 1.0f;
        lights[index].stableId = 100 - index;
    }
    expect(selectLocalLights(lights, glm::vec3(0.0f), 64).size() == lights.size(),
           "scenes within the light limit upload every light");
    const std::vector<LocalLight> nearest = selectLocalLights(lights, glm::vec3(0.0f), 3);
    expect(nearest.size() == 3 && nearest[0].stableId == 98 && nearest[1].stableId == 99 && nearest[2].stableId == 100,
           "over the limit the nearest lights are kept in stable-id order");

    LocalLight light{.type = LocalLightType::Spot, .position = {1.0f, 2.0f, 3.0f}, .direction = {0.0f, -1.0f, 0.0f}};
    const glm::mat4 view = glm::lookAtRH(glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const LocalLight viewSpace = transformLocalLight(light, view);
    expect(glm::length(viewSpace.position - glm::vec3(1.0f, 2.0f, -7.0f)) < 1e-5f &&
               glm::length(viewSpace.direction - glm::vec3(0.0f, -1.0f, 0.0f)) < 1e-5f,
           "lights transform into view space for cluster assignment");
}

std::string withLocalLighting(const std::string& document, const std::string& localLights, bool keepLimit) {
    std::string result = document;
    const std::string quality = "\"localLights\": 1";
    result.replace(result.find(quality), quality.size(), "\"localLights\": " + localLights);
    if (!keepLimit) {
        const std::string limit = "\"maximumLocalLights\": 2048,";
        result.erase(result.find(limit), limit.size());
    }
    return result;
}

void testSettingsMigration() {
    RenderSettings source;
    source.maximumLocalLights = 2048;
    std::string encoded;
    RenderSettings decoded;
    expect(serializeRenderSettings(source, encoded) && deserializeRenderSettings(encoded, decoded) &&
               decoded.localLights == LocalLightingQuality::Clustered && decoded.maximumLocalLights == 2048,
           "clustered lighting and its light limit round trip");

    for (const char* legacy : {"1", "2"}) {
        RenderSettings migrated;
        migrated.localLights = LocalLightingQuality::Disabled;
        expect(deserializeRenderSettings(withLocalLighting(encoded, legacy, false), migrated) &&
                   migrated.localLights == LocalLightingQuality::Clustered &&
                   migrated.maximumLocalLights == RenderSettings{}.maximumLocalLights,
               "retired tiled light modes load as clustered lighting with the default limit");
    }
    RenderSettings disabled;
    expect(deserializeRenderSettings(withLocalLighting(encoded, "0", true), disabled) &&
               disabled.localLights == LocalLightingQuality::Disabled,
           "disabled local lighting still loads");
    RenderSettings rejected;
    expect(!deserializeRenderSettings(withLocalLighting(encoded, "3", true), rejected),
           "unknown local-lighting values are rejected");
    std::string zeroLimit = encoded;
    zeroLimit.replace(zeroLimit.find("2048"), 4, "0");
    expect(!deserializeRenderSettings(zeroLimit, rejected), "a zero local-light limit is rejected");

    RenderCapabilities capabilities;
    capabilities.maximumLocalLights = 1024;
    const ValidatedRenderSettings clamped = validateRenderSettings(source, capabilities);
    expect(clamped.settings.maximumLocalLights == 1024 &&
               clamped.settings.localLights == LocalLightingQuality::Clustered && !clamped.fallbacks.empty(),
           "a light limit above the renderer's capacity is reduced with a reported fallback");
    capabilities.maximumLocalLights = 0;
    expect(validateRenderSettings(source, capabilities).settings.localLights == LocalLightingQuality::Disabled,
           "renderers without clustered lighting disable local lights");
}
} // namespace

int main() {
    testSliceBoundaries();
    testFragmentClusterMirror();
    testAssignmentMatchesBruteForce();
    testSpotConeCulling();
    testClusterCapacity();
    testLightSelection();
    testSettingsMigration();
    if (failures == 0) {
        std::cout << "Clustered lighting tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
