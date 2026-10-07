#include "engine/assets/MeshOptimization.hpp"
#include "engine/graphics/MeshLodSelection.hpp"
#include "engine/graphics/RenderConfiguration.hpp"
#include "engine/graphics/RenderSubmission.hpp"

#include "support/Doctest.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <set>
#include <string>
#include <tuple>
#include <vector>

TEST_SUITE_BEGIN("MeshLod");

namespace {

// Morph deltas and skin data are derived from the position so tests can check
// that every per-vertex stream moved with its vertex.
glm::vec3 positionDelta(const glm::vec3& position) {
    return position * 2.0f + glm::vec3(0.5f);
}

glm::uvec4 jointsFor(const glm::vec3& position) {
    const auto encode = [](float value) { return static_cast<uint32_t>(std::lround((value + 2.0f) * 1000.0f)); };
    return {encode(position.x), encode(position.y), encode(position.z), 7u};
}

// Appends a UV sphere as one primitive with its own vertex range. An optional
// unreferenced vertex sits in the middle of the range.
void appendSphere(MeshAsset& mesh, glm::vec3 center, float radius, uint32_t rings, uint32_t segments,
                  bool addUnreferencedVertex) {
    const uint32_t firstVertex = static_cast<uint32_t>(mesh.vertices.size());
    const uint32_t firstIndex = static_cast<uint32_t>(mesh.indices.size());
    const auto vertexIndex = [&](uint32_t ring, uint32_t segment) {
        return firstVertex + ring * (segments + 1) + segment;
    };
    for (uint32_t ring = 0; ring <= rings; ++ring) {
        const float polar = glm::pi<float>() * static_cast<float>(ring) / static_cast<float>(rings);
        for (uint32_t segment = 0; segment <= segments; ++segment) {
            const float azimuth = glm::two_pi<float>() * static_cast<float>(segment) / static_cast<float>(segments);
            const glm::vec3 normal{std::sin(polar) * std::cos(azimuth), std::cos(polar),
                                   std::sin(polar) * std::sin(azimuth)};
            Vertex vertex;
            vertex.position = center + normal * radius;
            vertex.normal = normal;
            vertex.texCoord = {static_cast<float>(segment) / static_cast<float>(segments),
                               static_cast<float>(ring) / static_cast<float>(rings)};
            mesh.vertices.push_back(vertex);
        }
    }
    for (uint32_t ring = 0; ring < rings; ++ring) {
        for (uint32_t segment = 0; segment < segments; ++segment) {
            const uint32_t a = vertexIndex(ring, segment);
            const uint32_t b = vertexIndex(ring + 1, segment);
            const uint32_t c = vertexIndex(ring + 1, segment + 1);
            const uint32_t d = vertexIndex(ring, segment + 1);
            if (ring != 0) {
                mesh.indices.insert(mesh.indices.end(), {a, b, d});
            }
            if (ring + 1 != rings) {
                mesh.indices.insert(mesh.indices.end(), {d, b, c});
            }
        }
    }
    if (addUnreferencedVertex) {
        Vertex unused;
        unused.position = {9.0f, 9.0f, 9.0f};
        mesh.vertices.insert(mesh.vertices.begin() + firstVertex + 5, unused);
        for (size_t index = firstIndex; index < mesh.indices.size(); ++index) {
            if (mesh.indices[index] >= firstVertex + 5) {
                ++mesh.indices[index];
            }
        }
    }
    mesh.primitives.push_back(
        {.firstIndex = firstIndex, .indexCount = static_cast<uint32_t>(mesh.indices.size() - firstIndex)});
}

using VertexKey = std::tuple<float, float, float, float, float>;
using TriangleKey = std::array<VertexKey, 3>;

VertexKey vertexKey(const Vertex& vertex) {
    return {vertex.position.x, vertex.position.y, vertex.position.z, vertex.texCoord.x, vertex.texCoord.y};
}

// Triangles compared by vertex content, rotated to a canonical first corner so
// winding is preserved but index order and vertex order are not significant.
std::multiset<TriangleKey> triangleSet(const MeshAsset& mesh, uint32_t firstIndex, uint32_t indexCount) {
    std::multiset<TriangleKey> triangles;
    for (uint32_t index = firstIndex; index + 2 < firstIndex + indexCount; index += 3) {
        TriangleKey triangle{vertexKey(mesh.vertices[mesh.indices[index]]),
                             vertexKey(mesh.vertices[mesh.indices[index + 1]]),
                             vertexKey(mesh.vertices[mesh.indices[index + 2]])};
        std::rotate(triangle.begin(), std::min_element(triangle.begin(), triangle.end()), triangle.end());
        triangles.insert(triangle);
    }
    return triangles;
}

std::multiset<VertexKey> vertexSet(const MeshAsset& mesh) {
    std::multiset<VertexKey> vertices;
    for (const Vertex& vertex : mesh.vertices) {
        vertices.insert(vertexKey(vertex));
    }
    return vertices;
}

bool lodRangesValid(const MeshAsset& mesh, const MeshPrimitive& primitive, uint32_t firstVertex, uint32_t vertexCount) {
    for (uint32_t level = 0; level < primitive.lodCount(); ++level) {
        const MeshLod lod = primitive.lod(level);
        if (lod.indexCount == 0 || lod.indexCount % 3 != 0 ||
            static_cast<size_t>(lod.firstIndex) + lod.indexCount > mesh.indices.size()) {
            return false;
        }
        for (uint32_t index = lod.firstIndex; index < lod.firstIndex + lod.indexCount; ++index) {
            if (mesh.indices[index] < firstVertex || mesh.indices[index] >= firstVertex + vertexCount) {
                return false;
            }
        }
    }
    return true;
}

TEST_CASE("MeshLod: optimization preserves geometry") {
    MeshAsset mesh;
    appendSphere(mesh, glm::vec3(0.0f), 1.0f, 24, 48, true);
    const uint32_t secondFirstVertex = static_cast<uint32_t>(mesh.vertices.size());
    appendSphere(mesh, glm::vec3(3.0f, 0.0f, 0.0f), 0.5f, 16, 32, false);
    const uint32_t secondVertexCount = static_cast<uint32_t>(mesh.vertices.size()) - secondFirstVertex;
    const std::multiset<TriangleKey> firstBefore =
        triangleSet(mesh, mesh.primitives[0].firstIndex, mesh.primitives[0].indexCount);
    const std::multiset<TriangleKey> secondBefore =
        triangleSet(mesh, mesh.primitives[1].firstIndex, mesh.primitives[1].indexCount);
    const std::multiset<VertexKey> verticesBefore = vertexSet(mesh);
    const size_t vertexCount = mesh.vertices.size();
    const std::vector<MeshPrimitive> primitivesBefore = mesh.primitives;

    optimizeMesh(mesh);

    CHECK_MESSAGE((mesh.vertices.size() == vertexCount && vertexSet(mesh) == verticesBefore),
                  "optimization keeps every vertex, including unreferenced ones");
    CHECK_MESSAGE((mesh.primitives[0].firstIndex == primitivesBefore[0].firstIndex &&
                   mesh.primitives[0].indexCount == primitivesBefore[0].indexCount),
                  "the full-detail range keeps its position and size");
    CHECK_MESSAGE((triangleSet(mesh, mesh.primitives[0].firstIndex, mesh.primitives[0].indexCount) == firstBefore &&
                   triangleSet(mesh, mesh.primitives[1].firstIndex, mesh.primitives[1].indexCount) == secondBefore),
                  "reordering preserves each primitive's triangles and winding");
    CHECK_MESSAGE((lodRangesValid(mesh, mesh.primitives[0], 0, secondFirstVertex) &&
                   lodRangesValid(mesh, mesh.primitives[1], secondFirstVertex, secondVertexCount)),
                  "every LOD stays inside its primitive's vertex range");

    const MeshPrimitive& sphere = mesh.primitives[0];
    CHECK_MESSAGE((sphere.lodCount() > 1 && sphere.lodCount() <= maximumMeshLods),
                  "a dense curved primitive receives simplified levels within the LOD limit");
    bool decreasing = true;
    for (uint32_t level = 1; level < sphere.lodCount(); ++level) {
        decreasing = decreasing && sphere.lod(level).indexCount < sphere.lod(level - 1).indexCount &&
                     sphere.lod(level).geometricError >= sphere.lod(level - 1).geometricError;
    }
    CHECK_MESSAGE(decreasing, "LOD index counts decrease while geometric errors never decrease");
    CHECK_MESSAGE((sphere.lod(sphere.lodCount() - 1).geometricError > 0.0f &&
                   sphere.lod(sphere.lodCount() - 1).geometricError < 0.1f),
                  "the coarsest level records a bounded object-space error");
    CHECK_MESSAGE(sphere.lod(maximumMeshLods + 3).firstIndex == sphere.lod(sphere.lodCount() - 1).firstIndex,
                  "levels past the coarsest clamp to it");
}

TEST_CASE("MeshLod: deformable streams stay consistent") {
    MeshAsset morphed;
    appendSphere(morphed, glm::vec3(0.0f), 1.0f, 12, 24, false);
    MorphTarget target;
    for (const Vertex& vertex : morphed.vertices) {
        target.positionDeltas.push_back(positionDelta(vertex.position));
        target.normalDeltas.push_back(vertex.normal);
        target.tangentDeltas.push_back(glm::vec3(vertex.texCoord, 1.0f));
    }
    morphed.morphTargets.push_back(target);
    optimizeMesh(morphed);
    bool morphConsistent = true;
    for (size_t index = 0; index < morphed.vertices.size(); ++index) {
        const Vertex& vertex = morphed.vertices[index];
        const MorphTarget& moved = morphed.morphTargets.front();
        morphConsistent = morphConsistent && moved.positionDeltas[index] == positionDelta(vertex.position) &&
                          moved.normalDeltas[index] == vertex.normal &&
                          moved.tangentDeltas[index] == glm::vec3(vertex.texCoord, 1.0f);
    }
    CHECK_MESSAGE(morphConsistent, "morph-target deltas are remapped with their vertices");
    CHECK_MESSAGE(morphed.primitives.front().lodCount() == 1, "meshes with morph targets keep only full detail");

    MeshAsset skinned;
    appendSphere(skinned, glm::vec3(0.0f), 1.0f, 12, 24, false);
    for (Vertex& vertex : skinned.vertices) {
        vertex.joints = jointsFor(vertex.position);
        vertex.weights = {1.0f, 0.0f, 0.0f, 0.0f};
    }
    optimizeMesh(skinned);
    CHECK_MESSAGE(std::ranges::all_of(skinned.vertices,
                                      [](const Vertex& vertex) { return vertex.joints == jointsFor(vertex.position); }),
                  "joint indices move with their vertices");
    CHECK_MESSAGE(skinned.primitives.front().lodCount() == 1, "skinned primitives keep only full detail");

    MeshAsset small;
    appendSphere(small, glm::vec3(0.0f), 1.0f, 3, 4, false);
    optimizeMesh(small);
    CHECK_MESSAGE(small.primitives.front().lodCount() == 1, "small primitives are not simplified");
}

TEST_CASE("MeshLod: lod selection") {
    MeshPrimitive primitive{.firstIndex = 0, .indexCount = 300};
    primitive.simplifiedLods[0] = {.firstIndex = 300, .indexCount = 150, .geometricError = 0.01f};
    primitive.simplifiedLods[1] = {.firstIndex = 450, .indexCount = 60, .geometricError = 0.04f};
    primitive.simplifiedLodCount = 2;
    const float projectionYScale = 1.0f / std::tan(glm::radians(30.0f));
    const MeshLodView view = makeMeshLodView(glm::vec3(0.0f), projectionYScale, 1080, 1.0f);
    // One world unit at distance one covers 1080 / 2 / tan(30 degrees) pixels.
    CHECK_MESSAGE(std::abs(view.errorScale - 935.3074f) < 0.01f, "the error scale follows the projection and viewport");

    const auto levelAt = [&](float distance, float radius, float scale) {
        return selectMeshLod(primitive, view, glm::vec3(0.0f, 0.0f, -distance), radius, scale);
    };
    CHECK_MESSAGE(levelAt(0.0f, 1.0f, 1.0f) == 0, "a camera inside the bounds draws full detail");
    CHECK_MESSAGE((levelAt(9.0f, 0.0f, 1.0f) == 0 && levelAt(9.5f, 0.0f, 1.0f) == 1),
                  "the first simplified level appears once its error projects within one pixel");
    CHECK_MESSAGE((levelAt(10.5f, 1.0f, 1.0f) == 1 && levelAt(9.5f, 1.0f, 1.0f) == 0),
                  "distance is measured to the nearest point of the bounding sphere");
    CHECK_MESSAGE((levelAt(38.0f, 0.0f, 1.0f) == 2 && levelAt(1.0e6f, 0.0f, 1.0f) == 2),
                  "far objects use the coarsest level");
    CHECK_MESSAGE((levelAt(18.0f, 0.0f, 2.0f) == 0 && levelAt(18.0f, 0.0f, 1.0f) == 1),
                  "larger world scales keep finer levels at the same distance");
    uint32_t previous = 0;
    bool monotonic = true;
    for (float distance = 0.0f; distance < 100.0f; distance += 0.25f) {
        const uint32_t level = levelAt(distance, 0.5f, 1.0f);
        monotonic = monotonic && level >= previous;
        previous = level;
    }
    CHECK_MESSAGE(monotonic, "LOD selection never becomes finer with distance");

    const MeshLodView disabled = makeMeshLodView(glm::vec3(0.0f), projectionYScale, 1080, 0.0f);
    CHECK_MESSAGE((disabled.errorScale == 0.0f &&
                   selectMeshLod(primitive, disabled, glm::vec3(0.0f, 0.0f, -1.0e6f), 0.0f, 1.0f) == 0),
                  "a zero threshold always selects full detail");
    MeshPrimitive lossless = primitive;
    lossless.simplifiedLods[0].geometricError = 0.0f;
    lossless.simplifiedLods[1].geometricError = 0.0f;
    CHECK_MESSAGE(selectMeshLod(lossless, view, glm::vec3(0.0f), 1.0f, 1.0f) == 2,
                  "zero-error levels are selected even inside the bounds");
    CHECK_MESSAGE(maximumAxisScale(glm::mat4(glm::vec4(2.0f, 0.0f, 0.0f, 0.0f), glm::vec4(0.0f, 3.0f, 0.0f, 0.0f),
                                             glm::vec4(0.0f, 0.0f, 0.5f, 0.0f), glm::vec4(1.0f))) == 3.0f,
                  "the world scale is the largest axis scale");
}

TEST_CASE("MeshLod: indirect keys separate lods") {
    const IndirectDrawKey fullDetail{.pipeline = 0, .geometryPage = 0, .firstIndex = 0, .indexCount = 300};
    const IndirectDrawKey simplified{.pipeline = 0, .geometryPage = 0, .firstIndex = 300, .indexCount = 150};
    const std::vector<IndirectDrawKey> instances{fullDetail, simplified, fullDetail, simplified, simplified};
    IndirectDrawPlan plan;
    buildIndirectDrawPlan(instances, plan);
    CHECK_MESSAGE((plan.batches.size() == 2 && plan.groups.size() == 1),
                  "levels of one primitive form separate batches in one draw group");
    CHECK_MESSAGE((plan.batches[0].instanceCount == 2 && plan.batches[1].instanceCount == 3),
                  "instances of the same level share a batch");
}

TEST_CASE("MeshLod: render settings threshold") {
    RenderSettings settings;
    CHECK_MESSAGE(settings.lodErrorThresholdPixels == 1.0f, "LOD selection defaults to a one-pixel threshold");
    settings.lodErrorThresholdPixels = 2.5f;
    std::string serialized;
    RenderSettings loaded;
    CHECK_MESSAGE((serializeRenderSettings(settings, serialized).success &&
                   deserializeRenderSettings(serialized, loaded).success && loaded.lodErrorThresholdPixels == 2.5f),
                  "the LOD threshold round-trips through render settings");
    std::string invalid = serialized;
    invalid.replace(invalid.find("2.5"), 3, "-1.0");
    CHECK_MESSAGE(!deserializeRenderSettings(invalid, loaded).success, "negative LOD thresholds are rejected");
}
} // namespace

TEST_SUITE_END();
