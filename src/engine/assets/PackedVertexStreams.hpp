#pragma once

#include "Assets.hpp"

#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Compact, renderer-independent encoding of `Vertex` split into separately
// bindable streams. Draws that only need positions (depth, outlines) read the
// 12-byte position stream alone instead of the full interleaved vertex.
//
// Stream layouts (little-endian, tightly packed):
// - position:   float32 x3                                     (12 B)
// - attributes: snorm16 x2 octahedral normal,
//               snorm16 x2 octahedral tangent with the bitangent sign folded
//               into the second component (see packTangent),
//               float32 x2 texture coordinate                  (16 B)
// - skin:       uint16 x4 joint indices, unorm8 x4 weights      (12 B)
//               present only when some vertex has a nonzero weight.
//
// Texture coordinates stay 32-bit because tiled and world-space UVs exceed
// half-float precision; 16-bit joints cover every glTF joint index.

struct PackedVertexAttributes {
    std::array<int16_t, 2> normal{};
    std::array<int16_t, 2> tangent{};
    std::array<float, 2> texCoord{};
};

struct PackedSkinInfluence {
    std::array<uint16_t, 4> joints{};
    std::array<uint8_t, 4> weights{};
};

static_assert(sizeof(PackedVertexAttributes) == 16, "attribute stream stride is part of the GPU contract");
static_assert(sizeof(PackedSkinInfluence) == 12, "skin stream stride is part of the GPU contract");

struct PackedVertexStreams {
    std::vector<glm::vec3> positions;
    std::vector<PackedVertexAttributes> attributes;
    // Empty for meshes without skin influences.
    std::vector<PackedSkinInfluence> skin;

    bool skinned() const {
        return !skin.empty();
    }
};

namespace vertex_packing {

constexpr uint32_t positionStride = sizeof(glm::vec3);
constexpr uint32_t attributeStride = sizeof(PackedVertexAttributes);
constexpr uint32_t skinStride = sizeof(PackedSkinInfluence);
// Joint indices are stored in 16 bits.
constexpr uint32_t maximumJointIndex = 65535;

// Octahedral mapping of a direction onto [-1, 1]^2. Zero-length input maps to +Z.
glm::vec2 encodeOctahedral(glm::vec3 direction);
glm::vec3 decodeOctahedral(glm::vec2 encoded);

// Signed-normalized 16-bit conversion matching Vulkan's SNORM decode.
int16_t packSnorm16(float value);
float unpackSnorm16(int16_t value);

std::array<int16_t, 2> packNormal(glm::vec3 normal);
glm::vec3 unpackNormal(std::array<int16_t, 2> packed);

// The bitangent sign (tangent.w) is folded into the second octahedral
// component: its magnitude carries the remapped coordinate in [1/32767, 1] and
// its sign carries tangent.w, so the sign survives even at the coordinate's
// minimum. This keeps the attribute stream at 16 bytes.
std::array<int16_t, 2> packTangent(glm::vec4 tangent);
glm::vec4 unpackTangent(std::array<int16_t, 2> packed);

// Normalizes the weights and quantizes them to unorm8 so they sum to exactly
// 255 (or 0 when every weight is zero). Joints whose weight is zero are
// written as 0. Fails when a weighted joint index exceeds maximumJointIndex.
std::optional<PackedSkinInfluence> packSkinInfluence(glm::uvec4 joints, glm::vec4 weights, std::string& error);

bool hasSkinInfluences(std::span<const Vertex> vertices);

// Splits and quantizes the vertices. Fails with a descriptive error when the
// mesh cannot be represented (joint indices beyond maximumJointIndex).
std::optional<PackedVertexStreams> packVertexStreams(std::span<const Vertex> vertices, std::string& error);

// GPU bytes the packed streams occupy, without packing them.
uint64_t packedByteSize(std::span<const Vertex> vertices);

} // namespace vertex_packing
