#include "engine/graphics/DepthPyramidLayout.hpp"
#include "engine/graphics/RenderConfiguration.hpp"
#include "engine/graphics/SceneNormalEncoding.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
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

// One texel per level: x farthest (minimum), y closest (maximum), reversed Z.
using Pyramid = std::vector<std::vector<glm::vec2>>;
constexpr glm::vec2 emptyRange{1.0f, 0.0f};

glm::vec2 combine(glm::vec2 first, glm::vec2 second) {
    return {std::min(first.x, second.x), std::max(first.y, second.y)};
}

glm::vec2 reduceFromPrevious(const Pyramid& pyramid, glm::uvec2 base, uint32_t level, glm::uvec2 texel) {
    const glm::uvec2 outputExtent = depth_pyramid::levelExtent(base, level);
    const glm::uvec2 sourceExtent = depth_pyramid::levelExtent(base, level - 1);
    const depth_pyramid::SourceRange range = depth_pyramid::sourceTexels(texel, outputExtent, sourceExtent);
    glm::vec2 value = emptyRange;
    for (uint32_t y = range.begin.y; y < range.end.y; ++y) {
        for (uint32_t x = range.begin.x; x < range.end.x; ++x) {
            value = combine(value, pyramid[level - 1][static_cast<size_t>(y) * sourceExtent.x + x]);
        }
    }
    return value;
}

Pyramid levelZero(const std::vector<float>& depth, glm::uvec2 base) {
    Pyramid pyramid(depth_pyramid::levelCount(base));
    for (uint32_t level = 0; level < pyramid.size(); ++level) {
        const glm::uvec2 extent = depth_pyramid::levelExtent(base, level);
        pyramid[level].assign(static_cast<size_t>(extent.x) * extent.y, glm::vec2(-1.0f));
    }
    for (size_t pixel = 0; pixel < depth.size(); ++pixel) {
        pyramid[0][pixel] = glm::vec2(depth[pixel]);
    }
    return pyramid;
}

// The per-level build (shaders/depth_pyramid.comp).
Pyramid buildPerLevel(const std::vector<float>& depth, glm::uvec2 base) {
    Pyramid pyramid = levelZero(depth, base);
    for (uint32_t level = 1; level < pyramid.size(); ++level) {
        const glm::uvec2 extent = depth_pyramid::levelExtent(base, level);
        for (uint32_t y = 0; y < extent.y; ++y) {
            for (uint32_t x = 0; x < extent.x; ++x) {
                pyramid[level][static_cast<size_t>(y) * extent.x + x] =
                    reduceFromPrevious(pyramid, base, level, {x, y});
            }
        }
    }
    return pyramid;
}

// Mirrors shaders/depth_pyramid_single_pass.comp: tiles reduce aligned blocks
// of level 0, loading the clamped edge pixel past the base extent, then the
// last workgroup completes edge texels and reduces the levels past the tile.
Pyramid buildSinglePass(const std::vector<float>& depth, glm::uvec2 base) {
    Pyramid pyramid = levelZero(depth, base);
    const uint32_t levels = static_cast<uint32_t>(pyramid.size());
    const glm::uvec2 tiledExtent = depth_pyramid::singlePassWorkgroups(base) * depth_pyramid::singlePassTileSize;
    std::vector<glm::vec2> blocks(static_cast<size_t>(tiledExtent.x) * tiledExtent.y);
    for (uint32_t y = 0; y < tiledExtent.y; ++y) {
        for (uint32_t x = 0; x < tiledExtent.x; ++x) {
            const glm::uvec2 clamped = glm::min(glm::uvec2(x, y), base - 1u);
            blocks[static_cast<size_t>(y) * tiledExtent.x + x] =
                glm::vec2(depth[static_cast<size_t>(clamped.y) * base.x + clamped.x]);
        }
    }
    glm::uvec2 blockExtent = tiledExtent;
    for (uint32_t level = 1; level <= depth_pyramid::singlePassTileLevels; ++level) {
        const glm::uvec2 nextExtent = blockExtent / 2u;
        std::vector<glm::vec2> next(static_cast<size_t>(nextExtent.x) * nextExtent.y);
        const auto at = [&](uint32_t sourceX, uint32_t sourceY) {
            return blocks[static_cast<size_t>(sourceY) * blockExtent.x + sourceX];
        };
        for (uint32_t y = 0; y < nextExtent.y; ++y) {
            for (uint32_t x = 0; x < nextExtent.x; ++x) {
                next[static_cast<size_t>(y) * nextExtent.x + x] =
                    combine(combine(at(2 * x, 2 * y), at(2 * x + 1, 2 * y)),
                            combine(at(2 * x, 2 * y + 1), at(2 * x + 1, 2 * y + 1)));
            }
        }
        blocks = std::move(next);
        blockExtent = nextExtent;
        if (level >= levels) {
            continue;
        }
        const glm::uvec2 extent = depth_pyramid::levelExtent(base, level);
        for (uint32_t y = 0; y < extent.y; ++y) {
            for (uint32_t x = 0; x < extent.x; ++x) {
                pyramid[level][static_cast<size_t>(y) * extent.x + x] =
                    blocks[static_cast<size_t>(y) * blockExtent.x + x];
            }
        }
    }
    for (uint32_t level = 1; level < levels; ++level) {
        const glm::uvec2 extent = depth_pyramid::levelExtent(base, level);
        const bool fullLevel = level > depth_pyramid::singlePassTileLevels;
        const glm::bvec2 incomplete =
            fullLevel ? glm::bvec2(true) : depth_pyramid::singlePassEdgeIncomplete(base, level);
        for (uint32_t y = 0; y < extent.y; ++y) {
            for (uint32_t x = 0; x < extent.x; ++x) {
                const bool edge = (incomplete.x && x + 1 == extent.x) || (incomplete.y && y + 1 == extent.y);
                if (fullLevel || edge) {
                    pyramid[level][static_cast<size_t>(y) * extent.x + x] =
                        reduceFromPrevious(pyramid, base, level, {x, y});
                }
            }
        }
    }
    return pyramid;
}

void testSinglePassLayout() {
    expect(depth_pyramid::singlePassWorkgroups({1280, 720}) == glm::uvec2(20, 12), "720p needs 20 x 12 tiles");
    expect(depth_pyramid::singlePassWorkgroups({64, 64}) == glm::uvec2(1, 1), "one tile covers 64 x 64");
    expect(depth_pyramid::singlePassWorkgroups({65, 1}) == glm::uvec2(2, 1), "a leftover column needs a tile");
    expect(depth_pyramid::singlePassWorkgroups({1, 1}) == glm::uvec2(1, 1), "a single pixel needs one tile");
    expect(!glm::any(depth_pyramid::singlePassEdgeIncomplete({1280, 720}, 4)), "720p tiles level 4 exactly");
    expect(depth_pyramid::singlePassEdgeIncomplete({1280, 720}, 5) == glm::bvec2(false, true),
           "720 rows do not tile level 5");
    expect(depth_pyramid::singlePassEdgeIncomplete({1283, 721}, 1) == glm::bvec2(true, true),
           "odd axes leave the first level's edge incomplete");
    expect(!glm::any(depth_pyramid::singlePassEdgeIncomplete({1283, 721}, 0)), "level 0 is always complete");
}

// The single-pass build must reproduce the per-level build exactly, and every
// texel must bound the base pixels it covers, or occlusion culling turns unsafe.
void testSinglePassMatchesPerLevel(glm::uvec2 base, uint32_t seed) {
    std::mt19937 random(seed);
    std::uniform_real_distribution<float> distribution(0.0f, 1.0f);
    std::vector<float> depth(static_cast<size_t>(base.x) * base.y);
    for (float& value : depth) {
        value = distribution(random);
    }
    const Pyramid perLevel = buildPerLevel(depth, base);
    const Pyramid singlePass = buildSinglePass(depth, base);
    expect(perLevel == singlePass, "the single-pass build matches the per-level build texel for texel");

    bool conservative = true;
    for (uint32_t level = 0; level < perLevel.size(); ++level) {
        const glm::uvec2 extent = depth_pyramid::levelExtent(base, level);
        std::vector<glm::vec2> expected(perLevel[level].size(), emptyRange);
        for (uint32_t y = 0; y < base.y; ++y) {
            for (uint32_t x = 0; x < base.x; ++x) {
                const glm::uvec2 texel = glm::min(glm::uvec2(x >> level, y >> level), extent - 1u);
                glm::vec2& range = expected[static_cast<size_t>(texel.y) * extent.x + texel.x];
                range = combine(range, glm::vec2(depth[static_cast<size_t>(y) * base.x + x]));
            }
        }
        conservative = conservative && expected == singlePass[level];
    }
    expect(conservative, "each single-pass texel holds the exact depth range of its base pixels");
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
    testSinglePassLayout();
    uint32_t seed = 1;
    for (const glm::uvec2 base :
         {glm::uvec2(1, 1), glm::uvec2(3, 2), glm::uvec2(63, 65), glm::uvec2(64, 64), glm::uvec2(129, 1),
          glm::uvec2(200, 7), glm::uvec2(317, 181), glm::uvec2(1280, 720), glm::uvec2(1283, 721)}) {
        testSinglePassMatchesPerLevel(base, seed++);
    }
    testDepthPrepassSetting();
    if (failures == 0) {
        std::cout << "Depth prepass tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
