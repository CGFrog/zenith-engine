#include "engine/graphics/AmbientOcclusion.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

constexpr glm::uvec2 renderExtent{640, 360};

// Reversed-Z projection built like MeshPass::record, optionally jittered.
glm::mat4 sceneProjection(glm::vec2 jitterNdc = glm::vec2(0.0f)) {
    glm::mat4 projection =
        glm::perspectiveRH_ZO(glm::radians(60.0f), static_cast<float>(renderExtent.x) / renderExtent.y, 100.0f, 0.1f);
    projection[1][1] *= -1.0f;
    projection[2][0] += jitterNdc.x;
    projection[2][1] += jitterNdc.y;
    return projection;
}

float projectionScale(const glm::mat4& projection) {
    return 0.5f * static_cast<float>(renderExtent.y) * std::abs(projection[1][1]);
}

struct Plane {
    glm::vec3 normal;
    float offset; // dot(normal, x) = offset
};

// Ray-casts view-space planes; the nearest hit in front of the camera wins.
struct PlaneScene {
    glm::mat4 inverseProjection;
    std::vector<Plane> planes;

    // `center` is a continuous render-pixel position.
    std::optional<glm::vec3> hit(glm::vec2 center) const {
        const glm::vec3 nearPoint =
            ambient_occlusion::reconstructViewPosition(inverseProjection, center, renderExtent, 1.0f);
        const glm::vec3 direction = glm::normalize(nearPoint);
        std::optional<glm::vec3> nearest;
        for (const Plane& plane : planes) {
            const float denominator = glm::dot(plane.normal, direction);
            if (std::abs(denominator) < 1e-6f) {
                continue;
            }
            const float distance = plane.offset / denominator;
            if (distance > 0.0f && (!nearest || distance < glm::length(*nearest))) {
                nearest = direction * distance;
            }
        }
        return nearest;
    }
};

glm::ivec2 pixelOf(const glm::mat4& projection, glm::vec3 viewPosition) {
    const glm::vec4 clip = projection * glm::vec4(viewPosition, 1.0f);
    const glm::vec2 ndc = glm::vec2(clip) / clip.w;
    return glm::ivec2(glm::floor((ndc * 0.5f + 0.5f) * glm::vec2(renderExtent)));
}

float visibilityAt(const PlaneScene& scene, const glm::mat4& projection, glm::ivec2 pixel, glm::vec3 normal,
                   const ambient_occlusion::GtaoParameters& parameters) {
    const std::optional<glm::vec3> position = scene.hit(glm::vec2(pixel) + 0.5f);
    if (!position) {
        return -1.0f;
    }
    const ambient_occlusion::GtaoSample sample{.pixel = pixel,
                                               .renderExtent = renderExtent,
                                               .viewPosition = *position,
                                               .viewNormal = normal,
                                               .projectionScale = projectionScale(projection),
                                               .sliceNoise = 0.5f,
                                               .stepNoise = 0.5f};
    return ambient_occlusion::integrateVisibility(
        sample,
        [&](glm::ivec2 texel, uint32_t level) {
            return scene.hit(ambient_occlusion::horizonTexelCenter(texel, level));
        },
        parameters);
}

void testViewPositionRoundTrip() {
    const std::vector<glm::vec2> jitters{{0.0f, 0.0f}, {0.4f / renderExtent.x, -0.7f / renderExtent.y}};
    const std::vector<glm::vec3> positions{{0.0f, 0.0f, -1.0f}, {1.5f, -0.75f, -4.0f}, {-20.0f, 9.0f, -60.0f}};
    float worstError = 0.0f;
    for (const glm::vec2 jitter : jitters) {
        const glm::mat4 projection = sceneProjection(jitter);
        const glm::mat4 inverseProjection = glm::inverse(projection);
        for (const glm::vec3& position : positions) {
            const glm::vec4 clip = projection * glm::vec4(position, 1.0f);
            const glm::vec3 ndc = glm::vec3(clip) / clip.w;
            expect(ndc.z > 0.0f && ndc.z <= 1.0f, "reversed Z keeps visible depth in (0, 1]");
            const glm::vec2 pixel = (glm::vec2(ndc) * 0.5f + 0.5f) * glm::vec2(renderExtent);
            const glm::vec3 reconstructed =
                ambient_occlusion::reconstructViewPosition(inverseProjection, pixel, renderExtent, ndc.z);
            worstError = std::max(worstError, glm::length(reconstructed - position) / -position.z);
        }
    }
    expect(worstError < 1e-4f, "view positions round trip through pixel and reversed-Z depth");
    // Nearer surfaces have larger reversed-Z depth.
    const glm::mat4 inverseProjection = glm::inverse(sceneProjection());
    const glm::vec2 center = glm::vec2(renderExtent) * 0.5f;
    expect(ambient_occlusion::reconstructViewPosition(inverseProjection, center, renderExtent, 1.0f).z > -0.11f,
           "depth 1 reconstructs the near plane");
    // Top rows of the image lie above the view axis (+Y up in view space).
    expect(ambient_occlusion::reconstructViewPosition(inverseProjection, {center.x, 0.5f}, renderExtent, 0.5f).y > 0.0f,
           "the first pixel row is above the view axis");
}

void testSliceIntegral() {
    const float halfPi = 0.5f * ambient_occlusion::pi;
    expect(std::abs(ambient_occlusion::sliceArcVisibility(-halfPi, halfPi, 0.0f) - 1.0f) < 1e-5f,
           "an open slice facing the viewer is fully visible");
    expect(std::abs(ambient_occlusion::sliceArcVisibility(0.0f, halfPi, 0.0f) - 0.5f) < 1e-5f,
           "a slice closed on one side is half visible");
    expect(std::abs(ambient_occlusion::sliceArcVisibility(0.0f, 0.0f, 0.0f)) < 1e-5f, "a closed slice is occluded");
}

void testFlatSurfacesAreUnoccluded() {
    const glm::mat4 projection = sceneProjection();
    const ambient_occlusion::GtaoParameters parameters{.radius = 1.0f, .sliceCount = 8, .stepsPerSide = 8};
    // A wall facing the camera.
    const PlaneScene wall{glm::inverse(projection), {{{0.0f, 0.0f, 1.0f}, -5.0f}}};
    const float facing = visibilityAt(wall, projection, glm::ivec2(renderExtent / 2u), {0.0f, 0.0f, 1.0f}, parameters);
    expect(std::abs(facing - 1.0f) < 1e-3f, "a plane facing the camera is unoccluded");
    // A floor seen at an angle: tilted normals must still integrate to 1.
    const glm::vec3 floorNormal = glm::normalize(glm::vec3(0.0f, 1.0f, 0.3f));
    const PlaneScene floor{glm::inverse(projection), {{floorNormal, glm::dot(floorNormal, {0.0f, -1.5f, 0.0f})}}};
    for (const glm::ivec2 pixel : {glm::ivec2(320, 300), glm::ivec2(100, 340), glm::ivec2(600, 250)}) {
        const float tilted = visibilityAt(floor, projection, pixel, floorNormal, parameters);
        expect(tilted > 0.97f && tilted < 1.03f, "a tilted plane is unoccluded");
    }
    // Without occluders the weighted slice integral is exactly the cosine-
    // weighted hemisphere, also for view vectors far off the optical axis.
    ambient_occlusion::GtaoParameters open = parameters;
    open.sliceCount = 64;
    open.stepsPerSide = 0;
    const float corner = visibilityAt(floor, projection, glm::ivec2(4, 355), floorNormal, open);
    expect(std::abs(corner - 1.0f) < 2e-3f, "open slices integrate to full visibility off axis");
}

void testCornerOcclusion() {
    const glm::mat4 projection = sceneProjection();
    // Camera 1.5 above a floor, a wall 8 units ahead; view-space planes.
    const glm::vec3 floorNormal{0.0f, 1.0f, 0.0f};
    const glm::vec3 wallNormal{0.0f, 0.0f, 1.0f};
    const PlaneScene corner{glm::inverse(projection), {{floorNormal, -1.5f}, {wallNormal, -8.0f}}};
    const ambient_occlusion::GtaoParameters parameters{
        .radius = 2.0f, .falloffFraction = 0.2f, .sliceCount = 32, .stepsPerSide = 32};
    // A wall through the shaded point halves its cosine-weighted hemisphere.
    const glm::ivec2 nearCorner = pixelOf(projection, {0.0f, -1.5f, -7.9f});
    const float crease = visibilityAt(corner, projection, nearCorner, floorNormal, parameters);
    // The shaded point sits 0.1 from the wall, so slightly more than half is open.
    expect(crease >= 0.48f && crease < 0.58f, "a 90 degree corner occludes half the hemisphere");
    // Away from the wall the floor is open again.
    const glm::ivec2 open = pixelOf(projection, {0.0f, -1.5f, -3.5f});
    const float openFloor = visibilityAt(corner, projection, open, floorNormal, parameters);
    expect(openFloor > 0.97f, "the floor beyond the search radius is unoccluded");
    expect(crease < openFloor, "occlusion grows towards the crease");
    const float applied = ambient_occlusion::applyOcclusionPower(crease, parameters);
    expect(applied <= crease && applied >= 0.0f, "the occlusion power only darkens");
}

void testSpecularOcclusion() {
    for (const float roughness : {0.045f, 0.3f, 0.7f, 1.0f}) {
        for (const float nDotV : {0.0f, 0.25f, 0.5f, 1.0f}) {
            expect(std::abs(ambient_occlusion::specularOcclusion(nDotV, 1.0f, roughness) - 1.0f) < 1e-5f,
                   "unoccluded surfaces keep all specular");
            expect(ambient_occlusion::specularOcclusion(nDotV, 0.0f, roughness) == 0.0f,
                   "fully occluded surfaces lose all specular");
            float previous = 0.0f;
            for (int step = 0; step <= 20; ++step) {
                const float occlusion = static_cast<float>(step) / 20.0f;
                const float value = ambient_occlusion::specularOcclusion(nDotV, occlusion, roughness);
                expect(value >= 0.0f && value <= 1.0f, "specular occlusion stays in [0, 1]");
                expect(value + 1e-6f >= previous, "specular occlusion never decreases with ambient occlusion");
                previous = value;
            }
        }
    }
}

void testOcclusionUvMapping() {
    for (const float scale : {0.5f, 1.0f}) {
        for (const glm::uvec2 output : {glm::uvec2(1280, 720), glm::uvec2(1001, 577), glm::uvec2(17, 9)}) {
            const glm::uvec2 storage = scale < 1.0f ? (output + 1u) / 2u : output;
            // Dynamic resolution renders into the top-left of the output.
            for (const float renderScale : {1.0f, 0.77f, 0.5f}) {
                const glm::uvec2 render = glm::max(glm::uvec2(glm::vec2(output) * renderScale), glm::uvec2(1u));
                const glm::uvec2 covered = ambient_occlusion::occlusionExtent(render, scale);
                const glm::uvec2 written = ambient_occlusion::dispatchExtent(render, scale, storage);
                expect(covered.x <= storage.x && covered.y <= storage.y, "occlusion texels fit their storage");
                expect(written.x >= covered.x && written.y >= covered.y, "dispatches cover every occlusion texel");
                // Each texel's source pixel center maps onto that texel's center.
                for (const glm::uvec2 texel : {glm::uvec2(0u), covered - 1u}) {
                    const glm::ivec2 source = ambient_occlusion::sourcePixel(texel, scale, render);
                    expect(source.x < static_cast<int>(render.x) && source.y < static_cast<int>(render.y),
                           "source pixels lie inside the render extent");
                    const glm::vec2 uv =
                        ambient_occlusion::occlusionUv(glm::vec2(source) + 0.5f, scale, storage) * glm::vec2(storage);
                    expect(glm::length(uv - (glm::vec2(texel) + 0.5f)) < 1e-3f,
                           "source pixel centers sample their own texel");
                }
                // The bilinear footprint of the last fragment stays within written texels.
                const glm::vec2 lastFragment = glm::vec2(render) - 0.5f;
                const glm::vec2 texelCoordinate =
                    ambient_occlusion::occlusionUv(lastFragment, scale, storage) * glm::vec2(storage);
                const glm::vec2 footprintEnd = glm::floor(texelCoordinate + 0.5f);
                const bool insideWritten =
                    footprintEnd.x < static_cast<float>(written.x) && footprintEnd.y < static_cast<float>(written.y);
                const bool clampedAtStorage =
                    footprintEnd.x >= static_cast<float>(storage.x) || footprintEnd.y >= static_cast<float>(storage.y);
                expect(insideWritten || clampedAtStorage, "bilinear lookups never read unwritten texels");
            }
        }
    }
}

void testDenoiseWeights() {
    // The 5-tap window must weight each phase of the noise tile equally so the
    // dither pattern cancels on flat regions.
    std::vector<float> phaseWeights(ambient_occlusion::noiseTileSize, 0.0f);
    for (int offset = -ambient_occlusion::denoiseRadius; offset <= ambient_occlusion::denoiseRadius; ++offset) {
        const int phase = (offset + ambient_occlusion::noiseTileSize) % ambient_occlusion::noiseTileSize;
        phaseWeights[phase] += ambient_occlusion::denoiseSpatialWeight(offset);
    }
    for (const float weight : phaseWeights) {
        expect(std::abs(weight - phaseWeights[0]) < 1e-6f, "every noise phase has the same denoise weight");
    }
    expect(ambient_occlusion::denoiseSpatialWeight(-1) == ambient_occlusion::denoiseSpatialWeight(1),
           "the denoise window is symmetric");
    expect(ambient_occlusion::denoiseDepthWeight(5.0f, 5.0f) == 1.0f, "equal depths blend fully");
    expect(ambient_occlusion::denoiseDepthWeight(5.0f, 5.1f) > 0.5f, "nearby depths blend");
    expect(ambient_occlusion::denoiseDepthWeight(5.0f, 8.0f) == 0.0f, "depth edges are preserved");
    expect(ambient_occlusion::denoiseDepthWeight(50.0f, 51.0f) > ambient_occlusion::denoiseDepthWeight(5.0f, 6.0f),
           "the depth tolerance scales with distance");
}

void testQualityProfiles() {
    const ambient_occlusion::GtaoQualityProfile full =
        ambient_occlusion::qualityProfile(AmbientOcclusionQuality::FullResolution);
    const ambient_occlusion::GtaoQualityProfile half =
        ambient_occlusion::qualityProfile(AmbientOcclusionQuality::HalfResolution);
    const ambient_occlusion::GtaoQualityProfile off = ambient_occlusion::qualityProfile(AmbientOcclusionQuality::Off);
    expect(full.resolutionScale == 1.0f && half.resolutionScale == 0.5f, "quality selects the occlusion resolution");
    expect(off.resolutionScale == 1.0f, "Off reports full resolution so the cleared target samples correctly");
    expect(full.parameters.horizonDepthLevel == 0, "full resolution samples full-resolution depth");
    // Half resolution reads the pyramid level whose texels match its own.
    expect(static_cast<float>(1u << half.parameters.horizonDepthLevel) * half.resolutionScale == 1.0f,
           "half-resolution horizons read the matching pyramid level");
    const auto samplesPerTexel = [](const ambient_occlusion::GtaoParameters& parameters) {
        return parameters.sliceCount * parameters.stepsPerSide * 2u;
    };
    expect(half.parameters.sliceCount >= 2 && half.parameters.stepsPerSide >= 3,
           "half resolution keeps enough slices and steps to find contact occlusion");
    expect(samplesPerTexel(half.parameters) < samplesPerTexel(full.parameters),
           "half resolution takes fewer horizon samples per texel");
    expect(half.parameters.radius == full.parameters.radius &&
               half.parameters.occlusionPower == full.parameters.occlusionPower,
           "both resolutions integrate the same occlusion");
}

void testHorizonTexels() {
    const glm::uvec2 extent{9, 5};
    expect(!ambient_occlusion::horizonTexel({-0.5f, 2.0f}, 1, extent), "positions left of the image are skipped");
    expect(!ambient_occlusion::horizonTexel({4.0f, 5.0f}, 1, extent), "positions below the image are skipped");
    expect(*ambient_occlusion::horizonTexel({3.5f, 2.5f}, 0, extent) == glm::ivec2(3, 2),
           "level 0 texels are render pixels");
    expect(*ambient_occlusion::horizonTexel({3.5f, 2.5f}, 1, extent) == glm::ivec2(1, 1),
           "level 1 texels cover 2x2 pixels");
    // The last odd column and row fold into the last texel, like the pyramid.
    expect(*ambient_occlusion::horizonTexel({8.5f, 4.5f}, 1, extent) == glm::ivec2(3, 1),
           "leftover pixels of odd extents use the last texel");
    expect(ambient_occlusion::horizonTexelCenter({1, 1}, 1) == glm::vec2(3.0f),
           "a level texel stands for the center of its pixels");
    expect(ambient_occlusion::horizonTexelCenter({3, 2}, 0) == glm::vec2(3.5f, 2.5f),
           "level 0 texels stand for their pixel center");
    expect(ambient_occlusion::horizonUsesClosestDepth({1, 0}) != ambient_occlusion::horizonUsesClosestDepth({0, 0}) &&
               ambient_occlusion::horizonUsesClosestDepth({1, 1}) == ambient_occlusion::horizonUsesClosestDepth({0, 0}),
           "depth channels alternate in a checkerboard");
    // The first step never lands in the texel holding the center pixel, whose
    // source pixel is the top-left one of its 2x2 block at half resolution.
    for (const uint32_t level : {1u, 2u}) {
        const float minimum = ambient_occlusion::minimumStepPixels(level);
        const glm::uvec2 large{64, 64};
        for (int index = 0; index < 64; ++index) {
            const float angle = static_cast<float>(index) / 64.0f * 2.0f * ambient_occlusion::pi;
            const glm::vec2 direction(std::cos(angle), std::sin(angle));
            for (const glm::vec2 center : {glm::vec2(16.5f, 16.5f), glm::vec2(17.5f, 17.5f)}) {
                const glm::ivec2 own = *ambient_occlusion::horizonTexel(center, level, large);
                const glm::ivec2 first = *ambient_occlusion::horizonTexel(center + direction * minimum, level, large);
                expect(first != own, "the first horizon step leaves the center's own texel");
            }
        }
    }
    expect(ambient_occlusion::minimumStepPixels(0) == 1.0f, "full resolution keeps its one-pixel minimum step");
}

void testHalfResolutionMatchesFull() {
    const glm::mat4 projection = sceneProjection();
    const PlaneScene corner{glm::inverse(projection), {{{0.0f, 1.0f, 0.0f}, -1.5f}, {{0.0f, 0.0f, 1.0f}, -8.0f}}};
    const ambient_occlusion::GtaoParameters full{
        .radius = 2.0f, .falloffFraction = 0.2f, .sliceCount = 32, .stepsPerSide = 32};
    ambient_occlusion::GtaoParameters half = full;
    half.horizonDepthLevel = 1;
    for (const float distance : {7.9f, 7.6f, 3.5f}) {
        const glm::ivec2 pixel = pixelOf(projection, {0.0f, -1.5f, -distance});
        const float fullVisibility = visibilityAt(corner, projection, pixel, {0.0f, 1.0f, 0.0f}, full);
        const float halfVisibility = visibilityAt(corner, projection, pixel, {0.0f, 1.0f, 0.0f}, half);
        expect(std::abs(fullVisibility - halfVisibility) < 0.05f,
               "level-1 horizons integrate the same occlusion as full-resolution depth");
    }
}

void testUpsampleWindow() {
    using ambient_occlusion::upsampleWindowSize;
    for (const glm::ivec2 pixel : {glm::ivec2(0, 0), glm::ivec2(7, 4), glm::ivec2(100, 33)}) {
        const glm::ivec2 origin = ambient_occlusion::upsampleWindowOrigin(pixel);
        // Raw texel t is evaluated at pixel 2t: the window is centered on the
        // pixel to within half a texel.
        const glm::vec2 windowCenter = (glm::vec2(origin) + 0.5f * static_cast<float>(upsampleWindowSize - 1)) * 2.0f;
        const glm::vec2 offset = glm::abs(windowCenter - glm::vec2(pixel));
        expect(offset.x <= 1.0f && offset.y <= 1.0f, "the upsample window is centered on its pixel");
        // Every phase of the noise tile appears exactly once.
        std::vector<int> phases(ambient_occlusion::noiseTileSize * ambient_occlusion::noiseTileSize, 0);
        for (int y = 0; y < upsampleWindowSize; ++y) {
            for (int x = 0; x < upsampleWindowSize; ++x) {
                const glm::ivec2 texel = origin + glm::ivec2(x, y) + ambient_occlusion::noiseTileSize * 8;
                ++phases[(texel.y % ambient_occlusion::noiseTileSize) * ambient_occlusion::noiseTileSize +
                         texel.x % ambient_occlusion::noiseTileSize];
            }
        }
        expect(std::all_of(phases.begin(), phases.end(), [](int count) { return count == 1; }),
               "the upsample window holds every noise phase once");
    }
    // Neighbours across a depth edge barely count, but a window without any
    // matching depth still averages.
    const float matching = ambient_occlusion::upsampleDepthWeight(5.0f, 5.0f);
    const float edge = ambient_occlusion::upsampleDepthWeight(5.0f, 9.0f);
    const float sky = ambient_occlusion::upsampleDepthWeight(5.0f, 65504.0f);
    expect(matching > 1.0f - 1e-6f && edge > 0.0f && edge < 0.01f * matching,
           "depth edges are preserved when upsampling");
    expect(sky > 0.0f, "a window of sky texels keeps a positive total weight");
    // Background pixels next to an occluded foreground stay unoccluded.
    const float foregroundWeight = 8.0f * ambient_occlusion::upsampleDepthWeight(9.0f, 2.0f);
    const float backgroundWeight = 8.0f * ambient_occlusion::upsampleDepthWeight(9.0f, 9.0f);
    const float blended = (0.1f * foregroundWeight + 1.0f * backgroundWeight) / (foregroundWeight + backgroundWeight);
    expect(blended > 0.99f, "occlusion does not bleed onto the background");
}
} // namespace

int main() {
    testViewPositionRoundTrip();
    testSliceIntegral();
    testFlatSurfacesAreUnoccluded();
    testCornerOcclusion();
    testSpecularOcclusion();
    testOcclusionUvMapping();
    testDenoiseWeights();
    testQualityProfiles();
    testHorizonTexels();
    testHalfResolutionMatchesFull();
    testUpsampleWindow();
    if (failures != 0) {
        std::cerr << failures << " ambient occlusion test(s) failed\n";
        return 1;
    }
    std::cout << "Ambient occlusion tests passed\n";
    return 0;
}
