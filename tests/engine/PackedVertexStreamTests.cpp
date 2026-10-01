#include "engine/assets/PackedVertexStreams.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <numeric>
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

std::vector<glm::vec3> randomDirections(uint32_t count) {
    std::mt19937 generator(1234u);
    std::normal_distribution<float> distribution;
    std::vector<glm::vec3> directions;
    directions.reserve(count + 8);
    // Axis-aligned and diagonal cases exercise the octahedral fold seams.
    for (const glm::vec3 axis : {glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, -1, 0),
                                 glm::vec3(0, 0, 1), glm::vec3(0, 0, -1), glm::vec3(1, 1, -1), glm::vec3(-1, -1, 0)}) {
        directions.push_back(glm::normalize(axis));
    }
    while (directions.size() < count) {
        const glm::vec3 candidate(distribution(generator), distribution(generator), distribution(generator));
        if (glm::length(candidate) > 1e-3f) {
            directions.push_back(glm::normalize(candidate));
        }
    }
    return directions;
}

void testNormalRoundTrip() {
    float worstError = 0.0f;
    for (const glm::vec3 direction : randomDirections(20000)) {
        const glm::vec3 decoded = vertex_packing::unpackNormal(vertex_packing::packNormal(direction));
        worstError = std::max(worstError, angleBetween(direction, decoded));
    }
    expect(worstError < 0.001f, "octahedral snorm16 normals round-trip within 0.001 rad");
    const glm::vec3 fallback = vertex_packing::unpackNormal(vertex_packing::packNormal(glm::vec3(0.0f)));
    expect(std::abs(glm::length(fallback) - 1.0f) < 1e-4f, "zero-length normals decode to a unit vector");
}

void testTangentRoundTrip() {
    float worstError = 0.0f;
    bool signsPreserved = true;
    const std::vector<glm::vec3> directions = randomDirections(20000);
    for (size_t index = 0; index < directions.size(); ++index) {
        const float sign = index % 2 == 0 ? 1.0f : -1.0f;
        const glm::vec4 tangent(directions[index], sign);
        const glm::vec4 decoded = vertex_packing::unpackTangent(vertex_packing::packTangent(tangent));
        worstError = std::max(worstError, angleBetween(glm::vec3(tangent), glm::vec3(decoded)));
        signsPreserved = signsPreserved && decoded.w == sign;
    }
    expect(worstError < 0.001f, "octahedral tangents round-trip within 0.001 rad");
    expect(signsPreserved, "the bitangent sign survives packing for every direction");

    // The folded component's minimum must still carry the sign: an encoded y
    // of -1 occurs for directions such as -Y.
    for (const float sign : {1.0f, -1.0f}) {
        const glm::vec4 decoded = vertex_packing::unpackTangent(vertex_packing::packTangent({0.0f, -1.0f, 0.0f, sign}));
        expect(decoded.w == sign, "the sign survives at the folded component's minimum");
        expect(angleBetween(glm::vec3(decoded), glm::vec3(0.0f, -1.0f, 0.0f)) < 0.001f,
               "the folded component's minimum decodes to the original direction");
    }
}

void testSkinInfluence() {
    std::string error;
    std::mt19937 generator(99u);
    std::uniform_real_distribution<float> weightDistribution(0.0f, 1.0f);
    bool sumsExact = true;
    float worstWeightError = 0.0f;
    for (int sample = 0; sample < 5000; ++sample) {
        glm::vec4 weights(weightDistribution(generator), weightDistribution(generator), weightDistribution(generator),
                          weightDistribution(generator));
        if (sample % 3 == 0) {
            weights.w = 0.0f;
        }
        const auto packed = vertex_packing::packSkinInfluence(glm::uvec4(3, 17, 300, 65535), weights, error);
        if (!packed) {
            sumsExact = false;
            continue;
        }
        const uint32_t total = std::accumulate(packed->weights.begin(), packed->weights.end(), 0u,
                                               [](uint32_t sum, uint8_t value) { return sum + value; });
        sumsExact = sumsExact && total == 255;
        const glm::vec4 normalized = weights / (weights.x + weights.y + weights.z + weights.w);
        for (int influence = 0; influence < 4; ++influence) {
            worstWeightError =
                std::max(worstWeightError, std::abs(packed->weights[influence] / 255.0f - normalized[influence]));
        }
    }
    expect(sumsExact, "quantized weights always sum to exactly 255");
    expect(worstWeightError <= 1.0f / 255.0f, "each quantized weight stays within 1/255 of the normalized weight");

    const auto joints = vertex_packing::packSkinInfluence({3, 17, 300, 65535}, {0.25f, 0.25f, 0.25f, 0.25f}, error);
    expect(joints && joints->joints[0] == 3 && joints->joints[1] == 17 && joints->joints[2] == 300 &&
               joints->joints[3] == 65535,
           "16-bit joint indices are preserved");

    const auto unweighted = vertex_packing::packSkinInfluence({7, 9, 11, 13}, glm::vec4(0.0f), error);
    expect(unweighted && unweighted->weights == std::array<uint8_t, 4>{} &&
               unweighted->joints == std::array<uint16_t, 4>{},
           "vertices without weights pack to a zero influence");

    const auto unusedLargeJoint = vertex_packing::packSkinInfluence({1, 70000, 0, 0}, {1.0f, 0.0f, 0.0f, 0.0f}, error);
    expect(unusedLargeJoint && unusedLargeJoint->joints[1] == 0, "unweighted joints never fail the 16-bit limit");

    error.clear();
    const auto tooLarge = vertex_packing::packSkinInfluence({1, 65536, 0, 0}, {0.5f, 0.5f, 0.0f, 0.0f}, error);
    expect(!tooLarge && !error.empty(), "weighted joints beyond 65535 are rejected with an error");
}

void testMeshStreams() {
    std::vector<Vertex> staticVertices(5);
    for (size_t index = 0; index < staticVertices.size(); ++index) {
        staticVertices[index].position = glm::vec3(static_cast<float>(index), 2.0f, -1.0f);
        staticVertices[index].texCoord = glm::vec2(0.25f * static_cast<float>(index), 0.5f);
    }
    std::string error;
    const auto staticStreams = vertex_packing::packVertexStreams(staticVertices, error);
    expect(staticStreams && staticStreams->positions.size() == 5 && staticStreams->attributes.size() == 5 &&
               !staticStreams->skinned(),
           "static meshes produce position and attribute streams only");
    expect(staticStreams && staticStreams->positions[3] == glm::vec3(3.0f, 2.0f, -1.0f),
           "positions are stored at full precision");
    expect(staticStreams && staticStreams->attributes[3].texCoord == std::array<float, 2>{0.75f, 0.5f},
           "texture coordinates are stored at full precision");
    expect(vertex_packing::packedByteSize(staticVertices) == 5u * 28u, "static vertices occupy 28 GPU bytes each");

    std::vector<Vertex> skinnedVertices = staticVertices;
    skinnedVertices[2].joints = {1, 2, 0, 0};
    skinnedVertices[2].weights = {0.75f, 0.25f, 0.0f, 0.0f};
    const auto skinnedStreams = vertex_packing::packVertexStreams(skinnedVertices, error);
    expect(skinnedStreams && skinnedStreams->skinned() && skinnedStreams->skin.size() == 5,
           "any weighted vertex gives the whole mesh a skin stream");
    expect(skinnedStreams && skinnedStreams->skin[0].weights == std::array<uint8_t, 4>{},
           "unweighted vertices of a skinned mesh keep zero weights");
    expect(vertex_packing::packedByteSize(skinnedVertices) == 5u * 40u, "skinned vertices occupy 40 GPU bytes each");

    skinnedVertices[4].joints = {70000, 0, 0, 0};
    skinnedVertices[4].weights = {1.0f, 0.0f, 0.0f, 0.0f};
    error.clear();
    expect(!vertex_packing::packVertexStreams(skinnedVertices, error) && !error.empty(),
           "a mesh with unrepresentable joints fails to pack with an error");
}
} // namespace

int main() {
    testNormalRoundTrip();
    testTangentRoundTrip();
    testSkinInfluence();
    testMeshStreams();
    return failures == 0 ? 0 : 1;
}
