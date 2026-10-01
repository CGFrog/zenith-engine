#include "engine/assets/ImageDecoder.hpp"
#include "engine/graphics/EnvironmentLighting.hpp"
#include "engine/graphics/Lighting.hpp"
#include "engine/scene/Scene.hpp"

#include <glm/gtc/packing.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
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

bool near(float actual, float expected, float tolerance) {
    return std::abs(actual - expected) <= tolerance;
}

bool near(const glm::vec3& actual, const glm::vec3& expected, float tolerance) {
    return near(actual.x, expected.x, tolerance) && near(actual.y, expected.y, tolerance) &&
           near(actual.z, expected.z, tolerance);
}

void testCubeFaceRoundTrip() {
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
    expect(roundTrips, "cube face directions map back to the same face and texel coordinate");
    expect(near(cubeFaceDirection(0, {0.5f, 0.5f}), {1.0f, 0.0f, 0.0f}, 1e-6f) &&
               near(cubeFaceDirection(2, {0.5f, 0.5f}), {0.0f, 1.0f, 0.0f}, 1e-6f) &&
               near(cubeFaceDirection(5, {0.5f, 0.5f}), {0.0f, 0.0f, -1.0f}, 1e-6f),
           "cube faces follow Vulkan's +X, -X, +Y, -Y, +Z, -Z layer order");
    // Vulkan samples +X with s = -z, t = -y, so the face's first texel row looks up.
    expect(cubeFaceDirection(0, {0.5f, 0.0f}).y > 0.0f && cubeFaceDirection(0, {0.0f, 0.5f}).z > 0.0f,
           "cube face texel axes match Vulkan's face selection table");
}

void testEquirectangularRoundTrip() {
    using namespace environment_lighting;
    const glm::vec3 directions[]{glm::normalize(glm::vec3(1.0f, 0.3f, 0.2f)),
                                 glm::normalize(glm::vec3(-0.4f, -0.7f, 0.5f)),
                                 glm::normalize(glm::vec3(0.1f, 0.2f, -0.9f))};
    bool roundTrips = true;
    for (const glm::vec3& direction : directions) {
        roundTrips =
            roundTrips && near(equirectangularDirection(equirectangularCoordinate(direction)), direction, 1e-5f);
    }
    expect(roundTrips, "equirectangular coordinates round-trip directions");
    expect(near(equirectangularCoordinate({0.0f, 1.0f, 0.0f}).y, 0.0f, 1e-6f) &&
               near(equirectangularCoordinate({1.0f, 0.0f, 0.0f}).x, 0.5f, 1e-6f),
           "equirectangular maps put +Y on the first row and +X at the center column");
}

void testIrradianceProjection() {
    using namespace environment_lighting;
    const glm::vec3 constant(0.8f, 0.4f, 0.2f);
    const IrradianceCoefficients uniform = projectIrradiance([&](const glm::vec3&) { return constant; }, 16);
    expect(near(evaluateIrradiance(uniform, {0.0f, 1.0f, 0.0f}), constant, 1e-3f) &&
               near(evaluateIrradiance(uniform, {0.3f, -0.5f, 0.8f}), constant, 1e-3f),
           "a constant environment's irradiance equals its radiance in every direction");

    const IrradianceCoefficients upperHemisphere =
        projectIrradiance([](const glm::vec3& direction) { return glm::vec3(direction.y > 0.0f ? 1.0f : 0.0f); }, 32);
    expect(near(evaluateIrradiance(upperHemisphere, {0.0f, 1.0f, 0.0f}).x, 1.0f, 0.02f),
           "a surface facing a uniform sky receives the sky's full radiance");
    expect(near(evaluateIrradiance(upperHemisphere, {1.0f, 0.0f, 0.0f}).x, 0.5f, 0.02f),
           "a vertical surface receives half of a uniform upper hemisphere");
    expect(evaluateIrradiance(upperHemisphere, {0.0f, -1.0f, 0.0f}).x < 0.02f,
           "a surface facing a black ground receives almost nothing");
}

void testEnvironmentBrdf() {
    using namespace environment_lighting;
    const glm::vec2 mirror = integrateEnvironmentBrdf(1.0f, 0.02f, 512);
    expect(near(mirror.x, 1.0f, 0.02f) && near(mirror.y, 0.0f, 0.02f),
           "a smooth surface viewed head-on reflects exactly F0");
    const glm::vec2 rough = integrateEnvironmentBrdf(0.5f, 1.0f, 512);
    expect(rough.x > 0.0f && rough.y > 0.0f && rough.x + rough.y < 1.0f,
           "rough reflection loses energy but stays positive");
    const glm::vec2 grazing = integrateEnvironmentBrdf(0.1f, 0.1f, 512);
    expect(grazing.y > integrateEnvironmentBrdf(1.0f, 0.1f, 512).y, "Fresnel bias grows toward grazing angles");
    expect(prefilteredMipRoughness(0, 6) == 0.0f && prefilteredMipRoughness(5, 6) == 1.0f,
           "prefiltered mips span mirror to fully rough");
}

void testProceduralSky() {
    using namespace environment_lighting;
    const glm::vec3 sky(0.2f, 0.3f, 0.6f);
    const glm::vec3 ground(0.1f, 0.08f, 0.05f);
    expect(near(proceduralSkyRadiance({0.0f, 1.0f, 0.0f}, sky, ground), sky, 1e-6f) &&
               near(proceduralSkyRadiance({0.0f, -1.0f, 0.0f}, sky, ground), ground, 1e-6f),
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

void testHdrDecode() {
    // (1, 0.5, 0.25) and (4, 2, 1) in shared-exponent form.
    const std::vector<uint8_t> bytes = radianceImage(2, 1, {128, 64, 32, 129, 128, 64, 32, 131});
    const ImageDecodeResult fromMemory = decodeImageMemory(bytes.data(), bytes.size(), "generated.hdr");
    expect(static_cast<bool>(fromMemory) && fromMemory.texture.width == 2 && fromMemory.texture.height == 1 &&
               fromMemory.texture.rgba.empty() && fromMemory.texture.rgbaHalf.size() == 8,
           "Radiance images decode to linear half-float texels");
    if (fromMemory && fromMemory.texture.rgbaHalf.size() == 8) {
        std::vector<float> texels;
        for (uint16_t half : fromMemory.texture.rgbaHalf) {
            texels.push_back(glm::unpackHalf1x16(half));
        }
        expect(near(texels[0], 1.0f, 1e-5f) && near(texels[1], 0.5f, 1e-5f) && near(texels[2], 0.25f, 1e-5f) &&
                   near(texels[3], 1.0f, 1e-5f),
               "HDR decoding preserves in-range radiance and opaque alpha");
        expect(near(texels[4], 4.0f, 1e-5f) && near(texels[5], 2.0f, 1e-5f),
               "HDR decoding keeps radiance above one unclamped");
    }

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "zenith_environment_lighting_test.hdr";
    {
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    const ImageDecodeResult fromFile = decodeImageFile(path);
    std::filesystem::remove(path);
    expect(static_cast<bool>(fromFile) && fromFile.texture.rgbaHalf == fromMemory.texture.rgbaHalf,
           "HDR files decode like in-memory HDR images");

    EnvironmentLightingCache cache;
    const PrefilteredEnvironment& averaged = cache.preprocess(fromMemory.texture, 2);
    expect(averaged.diffuseGround.x > 1.0f, "the CPU environment cache averages half-float texels without clamping");
}

void testProbeInfluence() {
    Scene scene;
    const Entity probe = scene.createEntity("Probe");
    scene.registry().emplace<ReflectionProbeComponent>(
        probe, ReflectionProbeComponent{.halfExtents = glm::vec3(1.0f), .blendDistance = 2.0f});
    scene.updateWorldTransforms();
    const auto inside = blendReflectionProbes(scene, glm::vec3(0.0f));
    const auto fading = blendReflectionProbes(scene, glm::vec3(2.0f, 0.0f, 0.0f));
    expect(inside.size() == 1 && near(inside[0].influence, 1.0f, 1e-6f) && near(inside[0].weight, 1.0f, 1e-6f),
           "a probe fully influences points inside its box");
    expect(fading.size() == 1 && near(fading[0].influence, 0.5f, 1e-5f) && near(fading[0].weight, 1.0f, 1e-6f),
           "probe influence fades across the blend distance while normalized weights still sum to one");
}
} // namespace

int main() {
    testProbeInfluence();
    testCubeFaceRoundTrip();
    testEquirectangularRoundTrip();
    testIrradianceProjection();
    testEnvironmentBrdf();
    testProceduralSky();
    testHdrDecode();
    if (failures != 0) {
        std::cerr << failures << " environment lighting test(s) failed\n";
        return 1;
    }
    std::cout << "Environment lighting tests passed\n";
    return 0;
}
