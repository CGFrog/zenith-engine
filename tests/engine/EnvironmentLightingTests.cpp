#include "engine/assets/ImageDecoder.hpp"
#include "engine/graphics/EnvironmentLighting.hpp"
#include "engine/graphics/Lighting.hpp"
#include "engine/scene/Scene.hpp"

#include "support/Doctest.hpp"

#include <glm/gtc/packing.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

TEST_SUITE_BEGIN("EnvironmentLighting");

namespace {

bool near(float actual, float expected, float tolerance) {
    return std::abs(actual - expected) <= tolerance;
}

bool near(const glm::vec3& actual, const glm::vec3& expected, float tolerance) {
    return near(actual.x, expected.x, tolerance) && near(actual.y, expected.y, tolerance) &&
           near(actual.z, expected.z, tolerance);
}

TEST_CASE("EnvironmentLighting: cube face round trip") {
    using namespace environment_lighting;
    const glm::vec2 coordinates[]{{0.5f, 0.5f}, {0.1f, 0.2f}, {0.9f, 0.3f}, {0.25f, 0.85f}};
    bool roundTrips = true;
    for (uint32_t face = 0; face < 6; ++face) {
        for (const glm::vec2& uv : coordinates) {
            const CubeFaceCoordinate mapped = cubeFaceCoordinate(cubeFaceDirection(face, uv));
            roundTrips =
                roundTrips && mapped.face == face && near(mapped.uv.x, uv.x, 1e-5f) && near(mapped.uv.y, uv.y, 1e-5f);
        }
    }
    CHECK_MESSAGE(roundTrips, "cube face directions map back to the same face and texel coordinate");
    CHECK_MESSAGE((near(cubeFaceDirection(0, {0.5f, 0.5f}), {1.0f, 0.0f, 0.0f}, 1e-6f) &&
                   near(cubeFaceDirection(2, {0.5f, 0.5f}), {0.0f, 1.0f, 0.0f}, 1e-6f) &&
                   near(cubeFaceDirection(5, {0.5f, 0.5f}), {0.0f, 0.0f, -1.0f}, 1e-6f)),
                  "cube faces follow Vulkan's +X, -X, +Y, -Y, +Z, -Z layer order");
    // Vulkan samples +X with s = -z, t = -y, so the face's first texel row looks up.
    CHECK_MESSAGE((cubeFaceDirection(0, {0.5f, 0.0f}).y > 0.0f && cubeFaceDirection(0, {0.0f, 0.5f}).z > 0.0f),
                  "cube face texel axes match Vulkan's face selection table");
}

TEST_CASE("EnvironmentLighting: equirectangular round trip") {
    using namespace environment_lighting;
    const glm::vec3 directions[]{glm::normalize(glm::vec3(1.0f, 0.3f, 0.2f)),
                                 glm::normalize(glm::vec3(-0.4f, -0.7f, 0.5f)),
                                 glm::normalize(glm::vec3(0.1f, 0.2f, -0.9f))};
    bool roundTrips = true;
    for (const glm::vec3& direction : directions) {
        roundTrips =
            roundTrips && near(equirectangularDirection(equirectangularCoordinate(direction)), direction, 1e-5f);
    }
    CHECK_MESSAGE(roundTrips, "equirectangular coordinates round-trip directions");
    CHECK_MESSAGE((near(equirectangularCoordinate({0.0f, 1.0f, 0.0f}).y, 0.0f, 1e-6f) &&
                   near(equirectangularCoordinate({1.0f, 0.0f, 0.0f}).x, 0.5f, 1e-6f)),
                  "equirectangular maps put +Y on the first row and +X at the center column");
}

TEST_CASE("EnvironmentLighting: irradiance projection") {
    using namespace environment_lighting;
    const glm::vec3 constant(0.8f, 0.4f, 0.2f);
    const IrradianceCoefficients uniform = projectIrradiance([&](const glm::vec3&) { return constant; }, 16);
    CHECK_MESSAGE((near(evaluateIrradiance(uniform, {0.0f, 1.0f, 0.0f}), constant, 1e-3f) &&
                   near(evaluateIrradiance(uniform, {0.3f, -0.5f, 0.8f}), constant, 1e-3f)),
                  "a constant environment's irradiance equals its radiance in every direction");

    const IrradianceCoefficients upperHemisphere =
        projectIrradiance([](const glm::vec3& direction) { return glm::vec3(direction.y > 0.0f ? 1.0f : 0.0f); }, 32);
    CHECK_MESSAGE(near(evaluateIrradiance(upperHemisphere, {0.0f, 1.0f, 0.0f}).x, 1.0f, 0.02f),
                  "a surface facing a uniform sky receives the sky's full radiance");
    CHECK_MESSAGE(near(evaluateIrradiance(upperHemisphere, {1.0f, 0.0f, 0.0f}).x, 0.5f, 0.02f),
                  "a vertical surface receives half of a uniform upper hemisphere");
    CHECK_MESSAGE(evaluateIrradiance(upperHemisphere, {0.0f, -1.0f, 0.0f}).x < 0.02f,
                  "a surface facing a black ground receives almost nothing");
}

TEST_CASE("EnvironmentLighting: environment brdf") {
    using namespace environment_lighting;
    const glm::vec2 mirror = integrateEnvironmentBrdf(1.0f, 0.02f, 512);
    CHECK_MESSAGE((near(mirror.x, 1.0f, 0.02f) && near(mirror.y, 0.0f, 0.02f)),
                  "a smooth surface viewed head-on reflects exactly F0");
    const glm::vec2 rough = integrateEnvironmentBrdf(0.5f, 1.0f, 512);
    CHECK_MESSAGE((rough.x > 0.0f && rough.y > 0.0f && rough.x + rough.y < 1.0f),
                  "rough reflection loses energy but stays positive");
    const glm::vec2 grazing = integrateEnvironmentBrdf(0.1f, 0.1f, 512);
    CHECK_MESSAGE(grazing.y > integrateEnvironmentBrdf(1.0f, 0.1f, 512).y, "Fresnel bias grows toward grazing angles");
    CHECK_MESSAGE((prefilteredMipRoughness(0, 6) == 0.0f && prefilteredMipRoughness(5, 6) == 1.0f),
                  "prefiltered mips span mirror to fully rough");
}

TEST_CASE("EnvironmentLighting: procedural sky") {
    using namespace environment_lighting;
    const glm::vec3 sky(0.2f, 0.3f, 0.6f);
    const glm::vec3 ground(0.1f, 0.08f, 0.05f);
    CHECK_MESSAGE((near(proceduralSkyRadiance({0.0f, 1.0f, 0.0f}, sky, ground), sky, 1e-6f) &&
                   near(proceduralSkyRadiance({0.0f, -1.0f, 0.0f}, sky, ground), ground, 1e-6f)),
                  "the analytic sky reaches the authored sky and ground colors at the poles");
}

// Writes a flat (uncompressed) Radiance image: widths below 8 never use RLE.
std::vector<uint8_t> radianceImage(uint32_t width, uint32_t height, const std::vector<uint8_t>& rgbe) {
    const std::string header =
        "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " + std::to_string(height) + " +X " + std::to_string(width) + "\n";
    std::vector<uint8_t> bytes(header.begin(), header.end());
    bytes.insert(bytes.end(), rgbe.begin(), rgbe.end());
    return bytes;
}

TEST_CASE("EnvironmentLighting: hdr decode") {
    // (1, 0.5, 0.25) and (4, 2, 1) in shared-exponent form.
    const std::vector<uint8_t> bytes = radianceImage(2, 1, {128, 64, 32, 129, 128, 64, 32, 131});
    const ImageDecodeResult fromMemory = decodeImageMemory(bytes.data(), bytes.size(), "generated.hdr");
    CHECK_MESSAGE((static_cast<bool>(fromMemory) && fromMemory.texture.width == 2 && fromMemory.texture.height == 1 &&
                   fromMemory.texture.rgba.empty() && fromMemory.texture.rgbaHalf.size() == 8),
                  "Radiance images decode to linear half-float texels");
    if (fromMemory && fromMemory.texture.rgbaHalf.size() == 8) {
        std::vector<float> texels;
        for (uint16_t half : fromMemory.texture.rgbaHalf) {
            texels.push_back(glm::unpackHalf1x16(half));
        }
        CHECK_MESSAGE((near(texels[0], 1.0f, 1e-5f) && near(texels[1], 0.5f, 1e-5f) && near(texels[2], 0.25f, 1e-5f) &&
                       near(texels[3], 1.0f, 1e-5f)),
                      "HDR decoding preserves in-range radiance and opaque alpha");
        CHECK_MESSAGE((near(texels[4], 4.0f, 1e-5f) && near(texels[5], 2.0f, 1e-5f)),
                      "HDR decoding keeps radiance above one unclamped");
    }

    const testsupport::TempDirectory pathDirectory("zenith_environment_lighting_test");
    const std::filesystem::path path = pathDirectory / "zenith_environment_lighting_test.hdr";
    {
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    const ImageDecodeResult fromFile = decodeImageFile(path);
    std::filesystem::remove(path);
    CHECK_MESSAGE((static_cast<bool>(fromFile) && fromFile.texture.rgbaHalf == fromMemory.texture.rgbaHalf),
                  "HDR files decode like in-memory HDR images");

    EnvironmentLightingCache cache;
    const PrefilteredEnvironment& averaged = cache.preprocess(fromMemory.texture, 2);
    CHECK_MESSAGE(averaged.diffuseGround.x > 1.0f,
                  "the CPU environment cache averages half-float texels without clamping");
}

TEST_CASE("EnvironmentLighting: probe influence") {
    Scene scene;
    const Entity probe = scene.createEntity("Probe");
    scene.registry().emplace<ReflectionProbeComponent>(
        probe, ReflectionProbeComponent{.halfExtents = glm::vec3(1.0f), .blendDistance = 2.0f});
    scene.updateWorldTransforms();
    const auto inside = blendReflectionProbes(scene, glm::vec3(0.0f));
    const auto fading = blendReflectionProbes(scene, glm::vec3(2.0f, 0.0f, 0.0f));
    CHECK_MESSAGE((inside.size() == 1 && near(inside[0].influence, 1.0f, 1e-6f) && near(inside[0].weight, 1.0f, 1e-6f)),
                  "a probe fully influences points inside its box");
    CHECK_MESSAGE((fading.size() == 1 && near(fading[0].influence, 0.5f, 1e-5f) && near(fading[0].weight, 1.0f, 1e-6f)),
                  "probe influence fades across the blend distance while normalized weights still sum to one");
}
} // namespace

TEST_SUITE_END();
