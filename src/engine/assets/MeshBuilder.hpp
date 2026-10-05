#pragma once

#include "Assets.hpp"
#include "MeshOptimization.hpp"

#include <cstdint>
#include <glm/glm.hpp>
#include <span>
#include <string>
#include <vector>

struct MeshBuildOptions {
    // Runs optimizeMesh, as imports do: vertex-cache, overdraw and fetch
    // ordering plus simplified LOD ranges for primitives large enough to
    // have them. Vertex indices returned while building no longer apply to
    // the built mesh.
    bool optimize{true};
    MeshOptimizationSettings optimization{};
};

struct MeshBuildResult {
    MeshAsset mesh;
    std::string error;

    explicit operator bool() const {
        return error.empty();
    }
};

// Assembles a MeshAsset in code, for AssetManager::createMesh: procedural
// walls, floors and stairs, merged room geometry, debug or minimap meshes.
// Triangles wind counter-clockwise when seen from their front (glTF's
// convention). The built mesh has normals (generated for vertices given none),
// tangents from the texture coordinates, and bounds. Mistakes such as an index
// past the vertices are kept and reported by build.
class MeshBuilder {
  public:
    // Starts a primitive drawn with material; the vertices and triangles
    // added next belong to it.
    MeshBuilder& beginPrimitive(MaterialHandle material);
    // Adds a vertex and returns its index in the whole mesh. A zero normal is
    // generated from the triangles that use the vertex.
    uint32_t vertex(const glm::vec3& position, const glm::vec3& normal = glm::vec3(0.0f),
                    const glm::vec2& texCoord = glm::vec2(0.0f));
    MeshBuilder& triangle(uint32_t a, uint32_t b, uint32_t c);
    // Two triangles, a-b-c and a-c-d.
    MeshBuilder& quad(uint32_t a, uint32_t b, uint32_t c, uint32_t d);
    // A flat rectangle with corners origin, origin + u, origin + u + v and
    // origin + v, facing along cross(u, v), with texture coordinates 0..1.
    MeshBuilder& rectangle(const glm::vec3& origin, const glm::vec3& u, const glm::vec3& v);
    // An axis-aligned box facing outward, each face a rectangle.
    MeshBuilder& box(const glm::vec3& center, const glm::vec3& halfExtents);
    // Appends indexed triangles in the current primitive; indices count from
    // the first appended position. normals and texCoords are empty or have
    // one entry per position.
    MeshBuilder& append(std::span<const glm::vec3> positions, std::span<const glm::vec3> normals,
                        std::span<const glm::vec2> texCoords, std::span<const uint32_t> indices);

    uint32_t vertexCount() const {
        return static_cast<uint32_t>(m_vertices.size());
    }

    // Fails when nothing was added, a primitive has no triangles, or a
    // triangle addresses a missing vertex. The builder is left unchanged.
    MeshBuildResult build(const MeshBuildOptions& options = {}) const;

  private:
    void fail(std::string error);

    std::vector<Vertex> m_vertices;
    // Whether each vertex's normal is generated at build.
    std::vector<bool> m_generateNormal;
    std::vector<uint32_t> m_indices;
    std::vector<MeshPrimitive> m_primitives;
    std::string m_error;
};

// Area-weighted vertex normals for vertices [firstVertex, firstVertex +
// vertexCount) from the triangles in mesh.indices from firstIndex on, which
// must address only those vertices.
void generateMeshNormals(MeshAsset& mesh, uint32_t firstVertex, uint32_t vertexCount, uint32_t firstIndex);
// Tangents (handedness in w) from the texture coordinates of the same range,
// orthogonal to the normals; vertices without usable UVs get an arbitrary
// tangent perpendicular to the normal.
void generateMeshTangents(MeshAsset& mesh, uint32_t firstVertex, uint32_t vertexCount, uint32_t firstIndex);
