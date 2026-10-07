#include "engine/graphics/GpuCullLayout.hpp"

#include "support/Doctest.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>

TEST_SUITE_BEGIN("GpuCullLayout");

namespace {

// A cascade built the way MeshPass fits directional shadows: a light view
// looking at the cascade center, a zero-to-one orthographic projection with Y
// flipped for Vulkan, and a texel-snapping offset.
glm::mat4 cascadeViewProjection(const glm::vec3& center, const glm::vec3& lightDirection, float radius,
                                float depthRange) {
    const glm::mat4 lightView =
        glm::lookAtRH(center + lightDirection * depthRange * 0.5f, center, glm::vec3(0.0f, 1.0f, 0.0f));
    glm::mat4 projection = glm::orthoRH_ZO(-radius, radius, -radius, radius, 0.1f, depthRange * 1.5f);
    projection[1][1] *= -1.0f;
    projection[3] += glm::vec4(0.0007f, -0.0003f, 0.0f, 0.0f);
    return projection * lightView;
}

// Independent reference: normalized planes against the plain radius, as the
// CPU shadow cull computed it before it shared the GPU predicate.
bool normalizedReferenceVisible(const glm::mat4& matrix, const glm::vec3& center, float radius, float& margin) {
    margin = INFINITY;
    bool visible = true;
    for (glm::vec4 plane : clipVolumePlanes(matrix)) {
        plane /= glm::length(glm::vec3(plane));
        const float distance = glm::dot(glm::vec3(plane), center) + plane.w + radius;
        margin = std::min(margin, std::abs(distance));
        visible = visible && distance >= 0.0f;
    }
    return visible;
}

TEST_CASE("GpuCullLayout: region layout") {
    const GpuCullRegionLayout layout{.limits = {.maximumObjects = 16384,
                                                .maximumInstances = 65536,
                                                .maximumBatches = 65536,
                                                .maximumGroups = 64,
                                                .frustumViews = 3}};
    CHECK_MESSAGE(layout.valid(), "the renderer's limits should form a valid layout");
    CHECK_MESSAGE(layout.regionCount() == 5, "the camera's two phases plus three cascades make five regions");
    CHECK_MESSAGE(
        (GpuCullRegion::phase(GpuCullPhase::Early).index == 0 && GpuCullRegion::phase(GpuCullPhase::Late).index == 1),
        "camera phases keep regions zero and one");
    CHECK_MESSAGE((GpuCullRegion::frustumView(0).index == 2 && GpuCullRegion::frustumView(2).index == 4),
                  "frustum views follow the camera regions");
    CHECK_MESSAGE((layout.contains(GpuCullRegion::frustumView(2)) && !layout.contains(GpuCullRegion::frustumView(3))),
                  "only configured frustum views are inside the layout");

    const GpuCullRegion cascade = GpuCullRegion::frustumView(1);
    CHECK_MESSAGE(layout.firstInstance(cascade) == 3ull * 65536, "instances start at region * maximum instances");
    CHECK_MESSAGE(layout.firstBatch(cascade) == 3ull * 65536, "commands start at region * maximum batches");
    CHECK_MESSAGE(layout.firstGroup(cascade) == 3ull * 64, "draw counts start at region * maximum groups");
    CHECK_MESSAGE((layout.instanceCapacity() == 5ull * 65536 && layout.batchCapacity() == 5ull * 65536 &&
                   layout.groupCapacity() == 5ull * 64),
                  "capacities cover every region");

    // Regions are disjoint: the last slot of one precedes the next region.
    for (uint32_t region = 0; region + 1 < layout.regionCount(); ++region) {
        const GpuCullRegion current{region};
        const GpuCullRegion next{region + 1};
        CHECK_MESSAGE((layout.firstInstance(current) + layout.limits.maximumInstances == layout.firstInstance(next) &&
                       layout.firstBatch(current) + layout.limits.maximumBatches == layout.firstBatch(next) &&
                       layout.firstGroup(current) + layout.limits.maximumGroups == layout.firstGroup(next)),
                      "adjacent regions should not overlap");
    }

    const GpuCullRegionLayout cameraOnly{
        .limits = {.maximumObjects = 1, .maximumInstances = 8, .maximumBatches = 8, .maximumGroups = 1}};
    CHECK_MESSAGE((cameraOnly.valid() && cameraOnly.regionCount() == 2), "frustum views are optional");

    const GpuCullRegionLayout overflowing{.limits = {.maximumObjects = 1,
                                                     .maximumInstances = UINT32_MAX / 2,
                                                     .maximumBatches = 8,
                                                     .maximumGroups = 1,
                                                     .frustumViews = 1}};
    CHECK_MESSAGE(!overflowing.valid(), "instance indices beyond 32 bits should be rejected");
    const GpuCullRegionLayout empty{.limits = {.maximumObjects = 1, .frustumViews = 3}};
    CHECK_MESSAGE(!empty.valid(), "zero capacities should be rejected");
}

TEST_CASE("GpuCullLayout: orthographic cascade planes") {
    const glm::vec3 lightDirection = glm::normalize(glm::vec3(0.4f, 0.8f, 0.5f));
    const glm::vec3 center(3.0f, 1.0f, -6.0f);
    const float radius = 12.0f;
    const float depthRange = 60.0f;
    const glm::mat4 cascade = cascadeViewProjection(center, lightDirection, radius, depthRange);

    CHECK_MESSAGE(sphereReachesClipVolume(clipVolumePlanes(cascade), center, 0.5f),
                  "the cascade center should be inside its own volume");
    const glm::vec3 lightRight = glm::normalize(glm::cross(lightDirection, glm::vec3(0.0f, 1.0f, 0.0f)));
    CHECK_MESSAGE(!sphereReachesClipVolume(clipVolumePlanes(cascade), center + lightRight * (radius + 2.0f), 1.0f),
                  "a sphere beyond the cascade's side should be culled");
    CHECK_MESSAGE(sphereReachesClipVolume(clipVolumePlanes(cascade), center + lightRight * (radius + 0.5f), 1.0f),
                  "a sphere straddling the cascade's side should be kept");
    CHECK_MESSAGE(!sphereReachesClipVolume(clipVolumePlanes(cascade), center + lightDirection * depthRange, 1.0f),
                  "a sphere behind the light's near plane should be culled");
    CHECK_MESSAGE(
        !sphereReachesClipVolume(clipVolumePlanes(cascade), center - lightDirection * (depthRange * 1.5f), 1.0f),
        "a sphere beyond the far plane should be culled");

    // The unnormalized predicate (the GPU mirror) agrees with the normalized
    // reference on random spheres away from any plane.
    std::mt19937 random(1234);
    std::uniform_real_distribution<float> coordinate(-40.0f, 40.0f);
    std::uniform_real_distribution<float> size(0.05f, 6.0f);
    uint32_t inside = 0;
    uint32_t outside = 0;
    uint32_t disagreements = 0;
    const std::array<glm::vec4, 6> planes = clipVolumePlanes(cascade);
    for (int sample = 0; sample < 20000; ++sample) {
        const glm::vec3 position = center + glm::vec3(coordinate(random), coordinate(random), coordinate(random));
        const float sphereRadius = size(random);
        float margin = 0.0f;
        const bool reference = normalizedReferenceVisible(cascade, position, sphereRadius, margin);
        if (margin < 0.001f) {
            continue;
        }
        if (sphereReachesClipVolume(planes, position, sphereRadius) != reference) {
            ++disagreements;
        }
        ++(reference ? inside : outside);
    }
    CHECK_MESSAGE(disagreements == 0, "the clip-volume test should match the normalized CPU reference");
    CHECK_MESSAGE((inside > 100 && outside > 100), "the samples should exercise both outcomes");
}

TEST_CASE("GpuCullLayout: perspective planes match too") {
    glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(60.0f), 16.0f / 9.0f, 500.0f, 0.1f);
    projection[1][1] *= -1.0f;
    const glm::mat4 view = glm::lookAtRH(glm::vec3(0.0f, 2.0f, 5.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const std::array<glm::vec4, 6> planes = clipVolumePlanes(projection * view);
    CHECK_MESSAGE(sphereReachesClipVolume(planes, glm::vec3(0.0f), 1.0f), "the look-at target should be visible");
    CHECK_MESSAGE(!sphereReachesClipVolume(planes, glm::vec3(0.0f, 2.0f, 20.0f), 1.0f),
                  "a sphere behind the camera should be culled");
}
} // namespace

TEST_SUITE_END();
