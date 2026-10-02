#include "engine/graphics/DepthPyramidLayout.hpp"
#include "engine/graphics/RenderConfiguration.hpp"
#include "engine/graphics/SceneNormalEncoding.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
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

float angleBetween(glm::vec3 first, glm::vec3 second) {
    return std::acos(std::clamp(glm::dot(glm::normalize(first), glm::normalize(second)), -1.0f, 1.0f));
}

void testOctahedralRoundTrip() {
    // Axes, diagonals, and the folded lower hemisphere's edges.
    const std::vector<glm::vec3> special{{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},    {0, -1, 0},      {0, 0, 1},
                                         {0, 0, -1}, {1, 1, 1},  {-1, -1, -1}, {1, -1, -0.01f}, {-0.3f, 0.2f, -0.9f}};
    float worstExact = 0.0f;
    for (const glm::vec3& normal : special) {
        const glm::vec2 encoded = scene_normal::encodeOctahedral(glm::normalize(normal));
        expect(std::abs(encoded.x) <= 1.0f && std::abs(encoded.y) <= 1.0f, "octahedral codes stay in [-1, 1]");
        worstExact = std::max(worstExact, angleBetween(scene_normal::decodeOctahedral(encoded), normal));
    }
    expect(worstExact < 1e-5f, "unquantized octahedral round trip is exact");

    // Sweep the sphere through the stored 10-bit representation.
    float worstStored = 0.0f;
    constexpr int steps = 96;
    for (int latitude = 0; latitude <= steps; ++latitude) {
        const float polar = glm::pi<float>() * static_cast<float>(latitude) / steps;
        for (int longitude = 0; longitude < steps * 2; ++longitude) {
            const float azimuth = glm::pi<float>() * static_cast<float>(longitude) / steps;
            const glm::vec3 normal(std::sin(polar) * std::cos(azimuth), std::sin(polar) * std::sin(azimuth),
                                   std::cos(polar));
            worstStored = std::max(worstStored, angleBetween(scene_normal::storeAndLoadNormal(normal), normal));
        }
    }
    // 10 bits per axis keep every direction within about a quarter degree.
    expect(worstStored < glm::radians(0.3f), "10-bit octahedral normals stay within 0.3 degrees");
}

void testPyramidLevelExtents() {
    expect(depth_pyramid::levelCount({1, 1}) == 1, "a single pixel has one level");
    expect(depth_pyramid::levelCount({1920, 1080}) == 11, "1080p has 11 levels");
    expect(depth_pyramid::levelCount({1024, 1}) == 11, "the longer axis sets the level count");
    expect(depth_pyramid::levelCount({1023, 7}) == 10, "odd sizes truncate the level count");
    expect(depth_pyramid::levelExtent({1283, 721}, 0) == glm::uvec2(1283, 721), "level 0 is the render extent");
    expect(depth_pyramid::levelExtent({1283, 721}, 1) == glm::uvec2(641, 360), "levels halve with truncation");
    expect(depth_pyramid::levelExtent({1283, 721}, 10) == glm::uvec2(1, 1), "levels never drop below one texel");
    expect(depth_pyramid::levelExtent({1283, 3}, 4) == glm::uvec2(80, 1), "a short axis clamps at one texel");
    // Dynamic resolution uses fewer levels than the storage provides.
    expect(depth_pyramid::levelCount({1280, 720}) <= depth_pyramid::levelCount({1920, 1080}),
           "a smaller render extent never needs more levels than its storage");
}

// Every base pixel must fall in exactly the texel the readers expect, and the
// recursive reduction must cover it, or occlusion culling turns unsafe.
void testPyramidCoverage(glm::uvec2 base) {
    const uint32_t levels = depth_pyramid::levelCount(base);
    // owner[level][pixel] = texel index that reduced the pixel at that level.
    std::vector<uint32_t> owner(static_cast<size_t>(base.x) * base.y);
    for (uint32_t pixel = 0; pixel < owner.size(); ++pixel) {
        owner[pixel] = pixel;
    }
    glm::uvec2 sourceExtent = base;
    bool consistent = true;
    for (uint32_t level = 1; level < levels; ++level) {
        const glm::uvec2 outputExtent = depth_pyramid::levelExtent(base, level);
        std::vector<int64_t> parentOf(static_cast<size_t>(sourceExtent.x) * sourceExtent.y, -1);
        for (uint32_t y = 0; y < outputExtent.y; ++y) {
            for (uint32_t x = 0; x < outputExtent.x; ++x) {
                const depth_pyramid::SourceRange range =
                    depth_pyramid::sourceTexels({x, y}, outputExtent, sourceExtent);
                for (uint32_t sourceY = range.begin.y; sourceY < range.end.y; ++sourceY) {
                    for (uint32_t sourceX = range.begin.x; sourceX < range.end.x; ++sourceX) {
                        int64_t& parent = parentOf[static_cast<size_t>(sourceY) * sourceExtent.x + sourceX];
                        consistent = consistent && parent == -1;
                        parent = static_cast<int64_t>(y) * outputExtent.x + x;
                    }
                }
            }
        }
        for (uint32_t pixel = 0; pixel < owner.size(); ++pixel) {
            const int64_t parent = parentOf[owner[pixel]];
            consistent = consistent && parent >= 0;
            owner[pixel] = static_cast<uint32_t>(std::max<int64_t>(parent, 0));
            const glm::uvec2 coordinate(pixel % base.x, pixel / base.x);
            const glm::uvec2 expected =
                glm::min(glm::uvec2(coordinate.x >> level, coordinate.y >> level), outputExtent - 1u);
            consistent = consistent && owner[pixel] == expected.y * outputExtent.x + expected.x;
        }
        sourceExtent = outputExtent;
    }
    expect(consistent, "each pyramid texel covers exactly the base pixels min(p >> level, extent - 1) names");
}

void testDepthPrepassSetting() {
    RenderSettings defaults;
    expect(defaults.depthPrepass, "the depth prepass is on by default");

    RenderSettings disabled;
    disabled.depthPrepass = false;
    std::string encoded;
    RenderSettings decoded;
    expect(serializeRenderSettings(disabled, encoded) && deserializeRenderSettings(encoded, decoded),
           "settings with the prepass disabled round-trip");
    expect(!decoded.depthPrepass, "a disabled prepass survives serialization");

    // Documents written before the setting existed enable the prepass.
    const std::string field = "\"depthPrepass\": false,";
    const size_t fieldStart = encoded.find(field);
    expect(fieldStart != std::string::npos, "serialized settings name the prepass toggle");
    if (fieldStart != std::string::npos) {
        std::string legacyDocument = encoded;
        legacyDocument.erase(fieldStart, field.size());
        RenderSettings legacy;
        legacy.depthPrepass = false;
        expect(deserializeRenderSettings(legacyDocument, legacy) && legacy.depthPrepass,
               "documents without the field enable the prepass");
        std::string invalidDocument = encoded;
        invalidDocument.replace(fieldStart, field.size(), "\"depthPrepass\": \"yes\",");
        RenderSettings rejected;
        expect(!deserializeRenderSettings(invalidDocument, rejected), "a non-boolean prepass value is rejected");
    }
}
} // namespace

int main() {
    testOctahedralRoundTrip();
    testPyramidLevelExtents();
    for (const glm::uvec2 base : {glm::uvec2(1, 1), glm::uvec2(2, 1), glm::uvec2(3, 3), glm::uvec2(17, 5),
                                  glm::uvec2(31, 64), glm::uvec2(97, 41), glm::uvec2(128, 3)}) {
        testPyramidCoverage(base);
    }
    testDepthPrepassSetting();
    if (failures == 0) {
        std::cout << "Depth prepass tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
