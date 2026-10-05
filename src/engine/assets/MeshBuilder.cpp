#include "MeshBuilder.hpp"

#include <array>
#include <cmath>
#include <utility>

namespace {
glm::vec3 normalizedOrUp(const glm::vec3& value) {
    return glm::dot(value, value) > 0.0f ? glm::normalize(value) : glm::vec3(0.0f, 1.0f, 0.0f);
}
} // namespace

void generateMeshNormals(MeshAsset& mesh, uint32_t firstVertex, uint32_t vertexCount, uint32_t firstIndex) {
    for (uint32_t i = 0; i < vertexCount; ++i) {
        mesh.vertices[firstVertex + i].normal = glm::vec3(0.0f);
    }

    for (size_t i = firstIndex; i + 2 < mesh.indices.size(); i += 3) {
        const uint32_t a = mesh.indices[i];
        const uint32_t b = mesh.indices[i + 1];
        const uint32_t c = mesh.indices[i + 2];
        const glm::vec3 normal = glm::cross(mesh.vertices[b].position - mesh.vertices[a].position,
                                            mesh.vertices[c].position - mesh.vertices[a].position);

        mesh.vertices[a].normal += normal;
        mesh.vertices[b].normal += normal;
        mesh.vertices[c].normal += normal;
    }

    for (uint32_t i = 0; i < vertexCount; ++i) {
        glm::vec3& normal = mesh.vertices[firstVertex + i].normal;
        normal = normalizedOrUp(normal);
    }
}

void generateMeshTangents(MeshAsset& mesh, uint32_t firstVertex, uint32_t vertexCount, uint32_t firstIndex) {
    std::vector<glm::vec3> tangents(vertexCount);
    std::vector<glm::vec3> bitangents(vertexCount);
    for (size_t i = firstIndex; i + 2 < mesh.indices.size(); i += 3) {
        const uint32_t ia = mesh.indices[i];
        const uint32_t ib = mesh.indices[i + 1];
        const uint32_t ic = mesh.indices[i + 2];
        const Vertex& a = mesh.vertices[ia];
        const Vertex& b = mesh.vertices[ib];
        const Vertex& c = mesh.vertices[ic];
        const glm::vec3 edge1 = b.position - a.position;
        const glm::vec3 edge2 = c.position - a.position;
        const glm::vec2 uv1 = b.texCoord - a.texCoord;
        const glm::vec2 uv2 = c.texCoord - a.texCoord;
        const float determinant = uv1.x * uv2.y - uv1.y * uv2.x;
        if (std::abs(determinant) < 1.0e-8f) {
            continue;
        }
        const float inverse = 1.0f / determinant;
        const glm::vec3 tangent = (edge1 * uv2.y - edge2 * uv1.y) * inverse;
        const glm::vec3 bitangent = (edge2 * uv1.x - edge1 * uv2.x) * inverse;
        for (uint32_t index : {ia, ib, ic}) {
            tangents[index - firstVertex] += tangent;
            bitangents[index - firstVertex] += bitangent;
        }
    }
    for (uint32_t i = 0; i < vertexCount; ++i) {
        const glm::vec3 normal = mesh.vertices[firstVertex + i].normal;
        glm::vec3 tangent = tangents[i] - normal * glm::dot(normal, tangents[i]);
        if (glm::dot(tangent, tangent) < 1.0e-8f) {
            tangent = std::abs(normal.y) < 0.999f ? glm::normalize(glm::cross({0.0f, 1.0f, 0.0f}, normal))
                                                  : glm::vec3(1.0f, 0.0f, 0.0f);
        } else {
            tangent = glm::normalize(tangent);
        }
        const float handedness = glm::dot(glm::cross(normal, tangent), bitangents[i]) < 0.0f ? -1.0f : 1.0f;
        mesh.vertices[firstVertex + i].tangent = {tangent, handedness};
    }
}

void MeshBuilder::fail(std::string error) {
    if (m_error.empty()) {
        m_error = std::move(error);
    }
}

MeshBuilder& MeshBuilder::beginPrimitive(MaterialHandle material) {
    m_primitives.push_back({.firstIndex = static_cast<uint32_t>(m_indices.size()), .material = material});
    return *this;
}

uint32_t MeshBuilder::vertex(const glm::vec3& position, const glm::vec3& normal, const glm::vec2& texCoord) {
    if (m_primitives.empty()) {
        fail("a vertex was added before beginPrimitive");
    }
    const bool generated = glm::dot(normal, normal) == 0.0f;
    m_vertices.push_back(
        {.position = position, .normal = generated ? glm::vec3(0.0f, 1.0f, 0.0f) : normal, .texCoord = texCoord});
    m_generateNormal.push_back(generated);
    return static_cast<uint32_t>(m_vertices.size() - 1);
}

MeshBuilder& MeshBuilder::triangle(uint32_t a, uint32_t b, uint32_t c) {
    if (m_primitives.empty()) {
        fail("a triangle was added before beginPrimitive");
    }
    m_indices.insert(m_indices.end(), {a, b, c});
    return *this;
}

MeshBuilder& MeshBuilder::quad(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return triangle(a, b, c).triangle(a, c, d);
}

MeshBuilder& MeshBuilder::rectangle(const glm::vec3& origin, const glm::vec3& u, const glm::vec3& v) {
    const glm::vec3 normal = normalizedOrUp(glm::cross(u, v));
    const uint32_t first = vertex(origin, normal, {0.0f, 1.0f});
    vertex(origin + u, normal, {1.0f, 1.0f});
    vertex(origin + u + v, normal, {1.0f, 0.0f});
    vertex(origin + v, normal, {0.0f, 0.0f});
    return quad(first, first + 1, first + 2, first + 3);
}

MeshBuilder& MeshBuilder::box(const glm::vec3& center, const glm::vec3& halfExtents) {
    // Each face's outward normal and two edge axes with u x v = normal.
    const std::array<std::array<glm::vec3, 3>, 6> faces{{{{{1, 0, 0}, {0, 0, -1}, {0, 1, 0}}},
                                                         {{{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}}},
                                                         {{{0, 1, 0}, {1, 0, 0}, {0, 0, -1}}},
                                                         {{{0, -1, 0}, {1, 0, 0}, {0, 0, 1}}},
                                                         {{{0, 0, 1}, {1, 0, 0}, {0, 1, 0}}},
                                                         {{{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}}}}};
    for (const auto& [normal, uAxis, vAxis] : faces) {
        const glm::vec3 u = uAxis * halfExtents * 2.0f;
        const glm::vec3 v = vAxis * halfExtents * 2.0f;
        rectangle(center + normal * halfExtents - u * 0.5f - v * 0.5f, u, v);
    }
    return *this;
}

MeshBuilder& MeshBuilder::append(std::span<const glm::vec3> positions, std::span<const glm::vec3> normals,
                                 std::span<const glm::vec2> texCoords, std::span<const uint32_t> indices) {
    if ((!normals.empty() && normals.size() != positions.size()) ||
        (!texCoords.empty() && texCoords.size() != positions.size())) {
        fail("appended normals and texture coordinates need one entry per position");
        return *this;
    }
    if (indices.size() % 3 != 0) {
        fail("appended indices are not whole triangles");
        return *this;
    }
    const uint32_t first = vertexCount();
    for (size_t index = 0; index < positions.size(); ++index) {
        vertex(positions[index], normals.empty() ? glm::vec3(0.0f) : normals[index],
               texCoords.empty() ? glm::vec2(0.0f) : texCoords[index]);
    }
    for (size_t index = 0; index < indices.size(); index += 3) {
        triangle(first + indices[index], first + indices[index + 1], first + indices[index + 2]);
    }
    return *this;
}

MeshBuildResult MeshBuilder::build(const MeshBuildOptions& options) const {
    if (!m_error.empty()) {
        return {.error = m_error};
    }
    if (m_primitives.empty() || m_vertices.empty()) {
        return {.error = "the mesh has no primitives or vertices"};
    }
    for (size_t index = 0; index < m_indices.size(); ++index) {
        if (m_indices[index] >= m_vertices.size()) {
            return {.error = "triangle index " + std::to_string(m_indices[index]) + " at position " +
                             std::to_string(index) + " addresses no vertex (there are " +
                             std::to_string(m_vertices.size()) + ")"};
        }
    }
    MeshBuildResult result;
    MeshAsset& mesh = result.mesh;
    mesh.vertices = m_vertices;
    mesh.indices = m_indices;
    mesh.primitives = m_primitives;
    for (size_t index = 0; index < mesh.primitives.size(); ++index) {
        MeshPrimitive& primitive = mesh.primitives[index];
        const size_t end =
            index + 1 < mesh.primitives.size() ? mesh.primitives[index + 1].firstIndex : m_indices.size();
        primitive.indexCount = static_cast<uint32_t>(end - primitive.firstIndex);
        if (primitive.indexCount == 0) {
            return {.error = "primitive " + std::to_string(index) + " has no triangles"};
        }
    }

    // Normals for the vertices given none, area-weighted over every triangle.
    std::vector<glm::vec3> generated(mesh.vertices.size(), glm::vec3(0.0f));
    for (size_t index = 0; index + 2 < mesh.indices.size(); index += 3) {
        const uint32_t a = mesh.indices[index];
        const uint32_t b = mesh.indices[index + 1];
        const uint32_t c = mesh.indices[index + 2];
        const glm::vec3 face = glm::cross(mesh.vertices[b].position - mesh.vertices[a].position,
                                          mesh.vertices[c].position - mesh.vertices[a].position);
        generated[a] += face;
        generated[b] += face;
        generated[c] += face;
    }
    for (size_t index = 0; index < mesh.vertices.size(); ++index) {
        mesh.vertices[index].normal =
            m_generateNormal[index] ? normalizedOrUp(generated[index]) : glm::normalize(mesh.vertices[index].normal);
    }
    generateMeshTangents(mesh, 0, static_cast<uint32_t>(mesh.vertices.size()), 0);

    if (options.optimize) {
        optimizeMesh(mesh, options.optimization);
    }
    mesh.bounds = calculateBoundingSphere(mesh.vertices);
    mesh.localBox = calculateBoundingBox(mesh.vertices);
    return result;
}
