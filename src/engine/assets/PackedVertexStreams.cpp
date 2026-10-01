#include "PackedVertexStreams.hpp"

#include <algorithm>
#include <cmath>

namespace vertex_packing {
namespace {
constexpr float snorm16Scale = 32767.0f;
constexpr float minimumFoldedMagnitude = 1.0f / snorm16Scale;
constexpr uint32_t unorm8Total = 255;
constexpr float minimumDirectionLength = 1e-8f;

float signNotZero(float value) {
    return value >= 0.0f ? 1.0f : -1.0f;
}

glm::vec3 normalizedOr(glm::vec3 direction, glm::vec3 fallback) {
    const float length = glm::length(direction);
    if (!(length > minimumDirectionLength) || !std::isfinite(length)) {
        return fallback;
    }
    return direction / length;
}
} // namespace

glm::vec2 encodeOctahedral(glm::vec3 direction) {
    const float manhattanLength = std::abs(direction.x) + std::abs(direction.y) + std::abs(direction.z);
    if (!(manhattanLength > minimumDirectionLength)) {
        return glm::vec2(0.0f);
    }
    glm::vec2 projected = glm::vec2(direction.x, direction.y) / manhattanLength;
    if (direction.z < 0.0f) {
        // Fold the lower hemisphere over the diagonals.
        projected = glm::vec2((1.0f - std::abs(projected.y)) * signNotZero(projected.x),
                              (1.0f - std::abs(projected.x)) * signNotZero(projected.y));
    }
    return projected;
}

glm::vec3 decodeOctahedral(glm::vec2 encoded) {
    glm::vec3 direction(encoded.x, encoded.y, 1.0f - std::abs(encoded.x) - std::abs(encoded.y));
    const float fold = std::max(-direction.z, 0.0f);
    direction.x += direction.x >= 0.0f ? -fold : fold;
    direction.y += direction.y >= 0.0f ? -fold : fold;
    return glm::normalize(direction);
}

int16_t packSnorm16(float value) {
    const float clamped = std::clamp(value, -1.0f, 1.0f);
    return static_cast<int16_t>(std::lround(clamped * snorm16Scale));
}

float unpackSnorm16(int16_t value) {
    return std::max(static_cast<float>(value) / snorm16Scale, -1.0f);
}

std::array<int16_t, 2> packNormal(glm::vec3 normal) {
    const glm::vec2 encoded = encodeOctahedral(normalizedOr(normal, glm::vec3(0.0f, 1.0f, 0.0f)));
    return {packSnorm16(encoded.x), packSnorm16(encoded.y)};
}

glm::vec3 unpackNormal(std::array<int16_t, 2> packed) {
    return decodeOctahedral(glm::vec2(unpackSnorm16(packed[0]), unpackSnorm16(packed[1])));
}

std::array<int16_t, 2> packTangent(glm::vec4 tangent) {
    const glm::vec2 encoded = encodeOctahedral(normalizedOr(glm::vec3(tangent), glm::vec3(1.0f, 0.0f, 0.0f)));
    // Remap y from [-1, 1] to a magnitude in [minimumFoldedMagnitude, 1] so a
    // zero coordinate never erases the sign.
    const float magnitude = minimumFoldedMagnitude + (encoded.y * 0.5f + 0.5f) * (1.0f - minimumFoldedMagnitude);
    const float bitangentSign = tangent.w < 0.0f ? -1.0f : 1.0f;
    return {packSnorm16(encoded.x), packSnorm16(magnitude * bitangentSign)};
}

glm::vec4 unpackTangent(std::array<int16_t, 2> packed) {
    const float folded = unpackSnorm16(packed[1]);
    const float magnitude = (std::abs(folded) - minimumFoldedMagnitude) / (1.0f - minimumFoldedMagnitude);
    const glm::vec2 encoded(unpackSnorm16(packed[0]), std::clamp(magnitude * 2.0f - 1.0f, -1.0f, 1.0f));
    return glm::vec4(decodeOctahedral(encoded), folded < 0.0f ? -1.0f : 1.0f);
}

std::optional<PackedSkinInfluence> packSkinInfluence(glm::uvec4 joints, glm::vec4 weights, std::string& error) {
    PackedSkinInfluence packed;
    std::array<float, 4> positive{};
    float total = 0.0f;
    for (int influence = 0; influence < 4; ++influence) {
        positive[influence] = std::isfinite(weights[influence]) ? std::max(weights[influence], 0.0f) : 0.0f;
        total += positive[influence];
    }
    if (!(total > 0.0f)) {
        return packed;
    }

    // Largest-remainder rounding keeps the quantized weights summing to 255.
    std::array<float, 4> remainders{};
    uint32_t assigned = 0;
    for (int influence = 0; influence < 4; ++influence) {
        const float scaled = positive[influence] / total * static_cast<float>(unorm8Total);
        const float whole = std::min(std::floor(scaled), static_cast<float>(unorm8Total));
        packed.weights[influence] = static_cast<uint8_t>(whole);
        remainders[influence] = scaled - whole;
        assigned += packed.weights[influence];
    }
    std::array<int, 4> order{0, 1, 2, 3};
    std::stable_sort(order.begin(), order.end(),
                     [&](int left, int right) { return remainders[left] > remainders[right]; });
    for (size_t step = 0; assigned < unorm8Total && step < order.size(); ++step) {
        ++packed.weights[order[step]];
        ++assigned;
    }

    for (int influence = 0; influence < 4; ++influence) {
        if (positive[influence] <= 0.0f) {
            continue;
        }
        if (joints[influence] > maximumJointIndex) {
            error = "Joint index " + std::to_string(joints[influence]) + " exceeds the packed vertex limit of " +
                    std::to_string(maximumJointIndex);
            return std::nullopt;
        }
        packed.joints[influence] = static_cast<uint16_t>(joints[influence]);
    }
    return packed;
}

bool hasSkinInfluences(std::span<const Vertex> vertices) {
    return std::any_of(vertices.begin(), vertices.end(), [](const Vertex& vertex) {
        return vertex.weights.x > 0.0f || vertex.weights.y > 0.0f || vertex.weights.z > 0.0f || vertex.weights.w > 0.0f;
    });
}

std::optional<PackedVertexStreams> packVertexStreams(std::span<const Vertex> vertices, std::string& error) {
    PackedVertexStreams streams;
    streams.positions.reserve(vertices.size());
    streams.attributes.reserve(vertices.size());
    const bool skinned = hasSkinInfluences(vertices);
    if (skinned) {
        streams.skin.reserve(vertices.size());
    }
    for (const Vertex& vertex : vertices) {
        streams.positions.push_back(vertex.position);
        streams.attributes.push_back({.normal = packNormal(vertex.normal),
                                      .tangent = packTangent(vertex.tangent),
                                      .texCoord = {vertex.texCoord.x, vertex.texCoord.y}});
        if (!skinned) {
            continue;
        }
        const auto influence = packSkinInfluence(vertex.joints, vertex.weights, error);
        if (!influence) {
            return std::nullopt;
        }
        streams.skin.push_back(*influence);
    }
    return streams;
}

uint64_t packedByteSize(std::span<const Vertex> vertices) {
    const uint64_t stride = positionStride + attributeStride + (hasSkinInfluences(vertices) ? skinStride : 0u);
    return stride * vertices.size();
}

} // namespace vertex_packing
