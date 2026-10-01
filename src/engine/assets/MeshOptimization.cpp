#include "MeshOptimization.hpp"

#include <meshoptimizer.h>

#include <algorithm>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace {
// Vertices [first, first + count) referenced by one primitive.
struct VertexRange {
    uint32_t first{0};
    uint32_t count{0};
};

constexpr uint32_t unreferencedVertex = std::numeric_limits<uint32_t>::max();
// Overdraw optimization may degrade vertex-cache efficiency by up to 5%.
constexpr float overdrawCacheThreshold = 1.05f;

bool isTriangleList(const MeshAsset& mesh, const MeshPrimitive& primitive) {
    return primitive.indexCount > 0 && primitive.indexCount % 3 == 0 &&
           static_cast<size_t>(primitive.firstIndex) + primitive.indexCount <= mesh.indices.size();
}

std::span<uint32_t> primitiveIndices(MeshAsset& mesh, const MeshPrimitive& primitive) {
    return {mesh.indices.data() + primitive.firstIndex, primitive.indexCount};
}

VertexRange referencedRange(MeshAsset& mesh, const MeshPrimitive& primitive) {
    const auto [minimum, maximum] = std::ranges::minmax(primitiveIndices(mesh, primitive));
    return {.first = minimum, .count = maximum - minimum + 1};
}

bool rangesDisjoint(std::vector<VertexRange> ranges) {
    std::ranges::sort(ranges, {}, &VertexRange::first);
    for (size_t index = 1; index < ranges.size(); ++index) {
        if (ranges[index].first < ranges[index - 1].first + ranges[index - 1].count) {
            return false;
        }
    }
    return true;
}

bool isSkinned(const MeshAsset& mesh, VertexRange range) {
    return std::any_of(mesh.vertices.begin() + range.first, mesh.vertices.begin() + range.first + range.count,
                       [](const Vertex& vertex) { return vertex.weights != glm::vec4(0.0f); });
}

const float* positions(const MeshAsset& mesh, VertexRange range) {
    return &mesh.vertices[range.first].position.x;
}

// meshoptimizer works on indices local to the primitive's vertex range.
std::vector<uint32_t> localIndices(MeshAsset& mesh, const MeshPrimitive& primitive, VertexRange range) {
    std::vector<uint32_t> local;
    local.reserve(primitive.indexCount);
    for (uint32_t index : primitiveIndices(mesh, primitive)) {
        local.push_back(index - range.first);
    }
    return local;
}

void storeLocalIndices(MeshAsset& mesh, const MeshPrimitive& primitive, VertexRange range,
                       std::span<const uint32_t> local) {
    std::ranges::transform(local, primitiveIndices(mesh, primitive).begin(),
                           [&range](uint32_t index) { return index + range.first; });
}

void optimizeTriangleOrder(MeshAsset& mesh, const MeshPrimitive& primitive, VertexRange range) {
    std::vector<uint32_t> local = localIndices(mesh, primitive, range);
    std::vector<uint32_t> cacheOrdered(local.size());
    meshopt_optimizeVertexCache(cacheOrdered.data(), local.data(), local.size(), range.count);
    meshopt_optimizeOverdraw(local.data(), cacheOrdered.data(), cacheOrdered.size(), positions(mesh, range),
                             range.count, sizeof(Vertex), overdrawCacheThreshold);
    storeLocalIndices(mesh, primitive, range, local);
}

template <typename Element>
void remapStream(std::vector<Element>& stream, VertexRange range, std::span<const uint32_t> remap) {
    // Morph-target streams stop short of primitives that have no targets; their
    // missing deltas are implicitly zero and need no reordering.
    if (stream.size() < static_cast<size_t>(range.first) + range.count) {
        return;
    }
    std::vector<Element> reordered(range.count);
    for (uint32_t vertex = 0; vertex < range.count; ++vertex) {
        reordered[remap[vertex]] = stream[range.first + vertex];
    }
    std::ranges::copy(reordered, stream.begin() + range.first);
}

void optimizeVertexOrder(MeshAsset& mesh, const MeshPrimitive& primitive, VertexRange range) {
    std::vector<uint32_t> local = localIndices(mesh, primitive, range);
    std::vector<uint32_t> remap(range.count);
    uint32_t nextSlot =
        static_cast<uint32_t>(meshopt_optimizeVertexFetchRemap(remap.data(), local.data(), local.size(), range.count));
    for (uint32_t& slot : remap) {
        if (slot == unreferencedVertex) {
            slot = nextSlot++;
        }
    }
    remapStream(mesh.vertices, range, remap);
    for (MorphTarget& target : mesh.morphTargets) {
        remapStream(target.positionDeltas, range, remap);
        remapStream(target.normalDeltas, range, remap);
        remapStream(target.tangentDeltas, range, remap);
    }
    for (uint32_t& index : local) {
        index = remap[index];
    }
    storeLocalIndices(mesh, primitive, range, local);
}

void generateLods(MeshAsset& mesh, MeshPrimitive& primitive, VertexRange range,
                  const MeshOptimizationSettings& settings) {
    const std::vector<uint32_t> fullDetail = localIndices(mesh, primitive, range);
    const float extent = meshopt_simplifyScale(positions(mesh, range), range.count, sizeof(Vertex));
    std::vector<uint32_t> simplified(fullDetail.size());
    std::vector<uint32_t> ordered(fullDetail.size());
    size_t previousCount = fullDetail.size();
    float previousError = 0.0f;
    primitive.simplifiedLodCount = 0;
    while (primitive.lodCount() < maximumMeshLods) {
        const size_t targetCount =
            static_cast<size_t>(static_cast<float>(previousCount) * settings.lodTargetRatio) / 3 * 3;
        if (targetCount < 3) {
            break;
        }
        // Simplifying from full detail keeps each level's error relative to
        // the original surface rather than accumulating across levels.
        float relativeError = 0.0f;
        const size_t count = meshopt_simplify(
            simplified.data(), fullDetail.data(), fullDetail.size(), positions(mesh, range), range.count,
            sizeof(Vertex), targetCount, settings.maximumLodRelativeError, meshopt_SimplifyLockBorder, &relativeError);
        if (count == 0 ||
            static_cast<float>(count) > static_cast<float>(previousCount) * settings.maximumLodRetainedRatio) {
            break;
        }
        meshopt_optimizeVertexCache(ordered.data(), simplified.data(), count, range.count);
        const float error = std::max(previousError, relativeError * extent);
        primitive.simplifiedLods[primitive.simplifiedLodCount++] = {.firstIndex =
                                                                        static_cast<uint32_t>(mesh.indices.size()),
                                                                    .indexCount = static_cast<uint32_t>(count),
                                                                    .geometricError = error};
        for (size_t index = 0; index < count; ++index) {
            mesh.indices.push_back(ordered[index] + range.first);
        }
        previousCount = count;
        previousError = error;
    }
}
} // namespace

void optimizeMesh(MeshAsset& mesh, const MeshOptimizationSettings& settings) {
    std::vector<MeshPrimitive*> triangles;
    std::vector<VertexRange> ranges;
    for (MeshPrimitive& primitive : mesh.primitives) {
        primitive.simplifiedLodCount = 0;
        if (!isTriangleList(mesh, primitive)) {
            continue;
        }
        const VertexRange range = referencedRange(mesh, primitive);
        if (static_cast<size_t>(range.first) + range.count <= mesh.vertices.size()) {
            triangles.push_back(&primitive);
            ranges.push_back(range);
        }
    }
    // Shared vertices would be moved twice, so reorder only disjoint ranges.
    const bool reorderVertices = rangesDisjoint(ranges);
    const bool hasMorphTargets = !mesh.morphTargets.empty();
    for (size_t index = 0; index < triangles.size(); ++index) {
        MeshPrimitive& primitive = *triangles[index];
        optimizeTriangleOrder(mesh, primitive, ranges[index]);
        if (reorderVertices) {
            optimizeVertexOrder(mesh, primitive, ranges[index]);
        }
        if (!hasMorphTargets && primitive.indexCount >= settings.minimumLodSourceIndices &&
            !isSkinned(mesh, ranges[index])) {
            generateLods(mesh, primitive, ranges[index], settings);
        }
    }
}
