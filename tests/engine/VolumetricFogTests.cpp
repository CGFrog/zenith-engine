#include "engine/graphics/RenderConfiguration.hpp"
#include "engine/graphics/VolumetricFog.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <iostream>
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

bool approximately(float actual, float expected, float relativeTolerance) {
    return std::abs(actual - expected) <= relativeTolerance * std::max(std::abs(expected), 1e-6f);
}

constexpr float nearPlane = 0.1f;
constexpr float farPlane = 1000.0f;
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

volumetric_fog::FroxelGrid sceneFroxels(VolumetricFogQuality quality) {
    const ClusterGrid clusters =
        makeClusterGrid(ClusterGridConfiguration{}, sceneProjection(), renderExtent, nearPlane, farPlane);
    return volumetric_fog::makeFroxelGrid(clusters, volumetric_fog::froxelSubdivision(quality));
}

void testQualityResolutions() {
    expect(volumetric_fog::froxelSubdivision(VolumetricFogQuality::Off) == glm::uvec3(0u),
           "Off allocates no froxels per cluster");
    expect(sceneFroxels(VolumetricFogQuality::Low).dimensions() == glm::uvec3(80u, 45u, 48u),
           "Low fog is 80x45x48 froxels");
    expect(sceneFroxels(VolumetricFogQuality::Medium).dimensions() == glm::uvec3(160u, 90u, 72u),
           "Medium fog is 160x90x72 froxels");
    expect(sceneFroxels(VolumetricFogQuality::High).dimensions() == glm::uvec3(160u, 90u, 96u),
           "High fog is 160x90x96 froxels");
    expect(!volumetric_fog::temporalReprojection(VolumetricFogQuality::Low) &&
               volumetric_fog::temporalReprojection(VolumetricFogQuality::Medium) &&
               volumetric_fog::temporalReprojection(VolumetricFogQuality::High),
           "only Medium and High reproject history");
}

void testDepthSliceMapping() {
    const volumetric_fog::FroxelGrid grid = sceneFroxels(VolumetricFogQuality::Medium);
    const ClusterGrid& clusters = grid.clusters;
    const uint32_t slices = grid.dimensions().z;
    bool roundTrips = true;
    bool clusterBoundaries = true;
    bool insideCluster = true;
    for (uint32_t slice = 0; slice <= slices; ++slice) {
        const float depth = grid.sliceDepth(static_cast<float>(slice));
        roundTrips = roundTrips && std::abs(grid.sliceCoordinate(depth) - static_cast<float>(slice)) < 1e-3f;
        if (slice % grid.subdivision.z == 0 && slice < slices) {
            clusterBoundaries =
                clusterBoundaries && approximately(depth, clusters.sliceStart(slice / grid.subdivision.z), 1e-4f);
        }
        if (slice < slices) {
            const float middle = grid.sliceDepth(static_cast<float>(slice) + 0.5f);
            insideCluster = insideCluster && clusters.sliceIndex(middle) == grid.clusterOf({0u, 0u, slice}).z;
        }
    }
    expect(roundTrips, "depth to slice and slice to depth are inverse inside the volume");
    expect(clusterBoundaries, "every cluster slice boundary is also a froxel slice boundary");
    expect(insideCluster, "each froxel slice lies inside the cluster slice its light list comes from");
    expect(approximately(grid.sliceDepth(static_cast<float>(slices)), clusters.sliceEndDepth, 1e-4f),
           "the volume ends where exponential cluster slicing ends");
    expect(grid.sliceCoordinate(0.0f) == 0.0f && grid.sliceCoordinate(farPlane) == static_cast<float>(slices),
           "depths outside the volume clamp to its first and last boundary");

    const glm::vec2 scaleBias = grid.sliceScaleBias();
    const float depth = 37.0f;
    expect(approximately(std::log(depth) * scaleBias.x + scaleBias.y, grid.sliceCoordinate(depth), 1e-4f),
           "the shader's log-scale-bias lookup matches the CPU slice coordinate");

    bool integratedLookup = true;
    for (uint32_t slice = 0; slice < slices; ++slice) {
        const float sliceEnd = grid.sliceDepth(static_cast<float>(slice + 1));
        const float expected = (static_cast<float>(slice) + 0.5f) / static_cast<float>(slices);
        integratedLookup = integratedLookup && std::abs(grid.integratedW(sliceEnd) - expected) < 1e-4f;
    }
    expect(integratedLookup, "a depth at the end of a froxel slice reads that slice's integrated texel center");
    expect(grid.integratedW(farPlane) == (static_cast<float>(slices) - 0.5f) / static_cast<float>(slices),
           "geometry and sky beyond the volume read the fully integrated last slice");
}

void testScreenTiling() {
    const volumetric_fog::FroxelGrid grid = sceneFroxels(VolumetricFogQuality::High);
    const glm::uvec3 size = grid.dimensions();
    bool matches = true;
    for (uint32_t y = 0; y < renderExtent.y; y += 7) {
        for (uint32_t x = 0; x < renderExtent.x; x += 11) {
            const glm::vec2 pixel = glm::vec2(x, y) + 0.5f;
            // The froxel a fragment samples covers uv = pixel / render extent.
            const glm::uvec2 froxel =
                glm::min(glm::uvec2(pixel / grid.clusters.renderExtent * glm::vec2(size)), glm::uvec2(size) - 1u);
            const glm::uvec3 cluster = grid.clusterOf({froxel, 0u});
            matches = matches && grid.clusters.clusterIndexAt(pixel, nearPlane) ==
                                     grid.clusters.clusterIndex(cluster.x, cluster.y, cluster.z);
        }
    }
    expect(matches, "froxel columns and rows subdivide the cluster tiles of the same pixels");
}

std::vector<float> columnSegments(const volumetric_fog::FroxelGrid& grid) {
    std::vector<float> lengths;
    for (uint32_t slice = 0; slice < grid.dimensions().z; ++slice) {
        lengths.push_back(grid.sliceDepth(static_cast<float>(slice + 1)) - grid.sliceDepth(static_cast<float>(slice)));
    }
    return lengths;
}

void testHomogeneousIntegration() {
    const volumetric_fog::FroxelGrid grid = sceneFroxels(VolumetricFogQuality::High);
    const std::vector<float> lengths = columnSegments(grid);
    const float extinction = 0.03f;
    const glm::vec3 inScattering(0.02f, 0.015f, 0.01f);
    const std::vector<volumetric_fog::FroxelMedium> media(lengths.size(), {inScattering, extinction});
    const std::vector<volumetric_fog::IntegratedFroxel> column = volumetric_fog::integrateFroxelColumn(media, lengths);
    expect(column.size() == lengths.size(), "integration produces one texel per froxel slice");
    bool analytic = true;
    float distance = 0.0f;
    for (size_t slice = 0; slice < column.size(); ++slice) {
        distance += lengths[slice];
        const float transmittance = std::exp(-extinction * distance);
        const glm::vec3 scattering = inScattering / extinction * (1.0f - transmittance);
        analytic = analytic && approximately(column[slice].transmittance, transmittance, 1e-4f) &&
                   approximately(column[slice].scattering.r, scattering.r, 1e-4f) &&
                   approximately(column[slice].scattering.b, scattering.b, 1e-4f);
    }
    expect(analytic, "front-to-back integration matches the homogeneous-medium closed form");

    const std::vector<volumetric_fog::FroxelMedium> vacuum(lengths.size(), {inScattering, 0.0f});
    const std::vector<volumetric_fog::IntegratedFroxel> emissive =
        volumetric_fog::integrateFroxelColumn(vacuum, lengths);
    expect(emissive.back().transmittance == 1.0f &&
               approximately(emissive.back().scattering.g, inScattering.g * (grid.clusters.sliceEndDepth - nearPlane),
                             1e-3f),
           "without extinction scattering accumulates linearly and nothing is absorbed");
}

void testIntegrationOrder() {
    const std::vector<float> lengths{10.0f, 10.0f};
    const std::vector<volumetric_fog::FroxelMedium> denseFirst{{glm::vec3(0.0f), 0.2f}, {glm::vec3(1.0f), 0.0f}};
    const std::vector<volumetric_fog::FroxelMedium> denseLast{{glm::vec3(1.0f), 0.0f}, {glm::vec3(0.0f), 0.2f}};
    const auto first = volumetric_fog::integrateFroxelColumn(denseFirst, lengths);
    const auto last = volumetric_fog::integrateFroxelColumn(denseLast, lengths);
    expect(approximately(first.back().transmittance, std::exp(-2.0f), 1e-5f) &&
               approximately(last.back().transmittance, std::exp(-2.0f), 1e-5f),
           "transmittance multiplies along the column regardless of order");
    expect(approximately(first.back().scattering.r, 10.0f * std::exp(-2.0f), 1e-4f) &&
               approximately(last.back().scattering.r, 10.0f, 1e-5f),
           "light scattered behind an absorbing slice is attenuated by it, light in front is not");
}

void testHenyeyGreenstein() {
    constexpr float pi = 3.14159265358979f;
    bool normalized = true;
    for (float anisotropy : {-0.8f, -0.3f, 0.0f, 0.3f, 0.6f, 0.9f}) {
        // Integrate over the sphere in the cosine: 2 pi * integral of p over [-1, 1].
        constexpr int steps = 20000;
        double integral = 0.0;
        for (int step = 0; step < steps; ++step) {
            const float cosine = -1.0f + (static_cast<float>(step) + 0.5f) * 2.0f / steps;
            integral += volumetric_fog::henyeyGreenstein(cosine, anisotropy) * (2.0 / steps);
        }
        normalized = normalized && std::abs(integral * 2.0 * pi - 1.0) < 2e-3;
    }
    expect(normalized, "the Henyey-Greenstein phase function integrates to one over the sphere");
    expect(approximately(volumetric_fog::henyeyGreenstein(0.3f, 0.0f), 1.0f / (4.0f * pi), 1e-5f),
           "zero anisotropy is isotropic scattering");
    expect(volumetric_fog::henyeyGreenstein(1.0f, 0.6f) > volumetric_fog::henyeyGreenstein(-1.0f, 0.6f),
           "positive anisotropy scatters forward");
}

void testHeightFog() {
    const VolumetricFogComponent fog{.density = 0.05f, .heightFalloff = 0.5f, .baseHeight = 2.0f};
    expect(volumetric_fog::heightFogExtinction(fog, -10.0f) == 0.05f &&
               volumetric_fog::heightFogExtinction(fog, 2.0f) == 0.05f,
           "density holds at and below the base height");
    expect(approximately(volumetric_fog::heightFogExtinction(fog, 6.0f), 0.05f * std::exp(-2.0f), 1e-5f),
           "density decays exponentially above the base height");
}

void testSettings() {
    RenderSettings settings;
    expect(settings.volumetricFog == VolumetricFogQuality::Off, "volumetric fog is off by default");
    settings.volumetricFog = VolumetricFogQuality::Medium;
    settings.budgets.volumetricFog = {0.1, 0.75};
    std::string document;
    RenderSettings restored;
    expect(serializeRenderSettings(settings, document).success &&
               deserializeRenderSettings(document, restored).success &&
               restored.volumetricFog == VolumetricFogQuality::Medium &&
               restored.budgets.volumetricFog.gpuMilliseconds == 0.75,
           "the fog quality and budget round trip through the render-settings document");
    RenderSettings legacy;
    expect(deserializeRenderSettings(R"({"version":1,"antialiasing":1,"shadows":1,"lighting":1,"bloom":0,)"
                                     R"("environment":0,"localLights":1})",
                                     legacy)
                   .success &&
               legacy.volumetricFog == VolumetricFogQuality::Off,
           "documents without a fog quality load with fog off");
    RenderSettings invalid;
    expect(!deserializeRenderSettings(R"({"version":1,"antialiasing":1,"shadows":1,"lighting":1,"bloom":0,)"
                                      R"("environment":0,"localLights":1,"volumetricFog":4})",
                                      invalid)
                .success,
           "unknown fog qualities are rejected");

    RenderCapabilities capabilities;
    capabilities.volumetricFog = false;
    const ValidatedRenderSettings validated = validateRenderSettings(settings, capabilities);
    expect(validated.settings.volumetricFog == VolumetricFogQuality::Off && !validated.fallbacks.empty(),
           "devices without 3D storage support fall back to fog off with an explanation");
}
} // namespace

int main() {
    testQualityResolutions();
    testDepthSliceMapping();
    testScreenTiling();
    testHomogeneousIntegration();
    testIntegrationOrder();
    testHenyeyGreenstein();
    testHeightFog();
    testSettings();
    if (failures == 0) {
        std::cout << "Volumetric fog tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
