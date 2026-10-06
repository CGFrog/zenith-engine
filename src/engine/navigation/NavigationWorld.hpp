#pragma once

// A navigation mesh over a scene's static geometry, built with Recast and queried with Detour, which stay private.

#include "NavMeshBuildSettings.hpp"
#include "engine/graphics/DebugDraw.hpp"
#include "engine/scene/Scene.hpp"

#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class AssetManager;
class dtNavMesh;
class NavigationCrowd;

struct NavigationResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

struct NavMeshStats {
    uint32_t inputTriangles{0};
    uint32_t polygons{0};
    uint32_t vertices{0};
};

// What NavigationWorld::findPath found.
enum class NavPath : uint8_t {
    // No path: a point is off the mesh, or the world has no mesh.
    None,
    // The path reaches the end point.
    Complete,
    // The end cannot be reached (another island, or behind a wall with no
    // way round); the path leads to the reachable point nearest it.
    Partial,
};

// The triangles a navigation mesh is built from, in world space.
struct NavigationGeometry {
    std::vector<glm::vec3> vertices;
    std::vector<uint32_t> indices;
};

// The static, solid colliders on the mask's layers as triangles, shaped the
// way PhysicsWorld builds their bodies: boxes, spheres and capsules as
// meshes, triangle-mesh colliders from their MeshAsset's full-detail ranges.
// Poses come from Scene::worldTransform, so world transforms need not be
// updated first.
NavigationGeometry gatherNavigationGeometry(const Scene& scene, const AssetManager& assets, uint32_t layerMask = ~0u);

// Appends a mesh's full-detail triangles (the ones physics collides with),
// placed by transform, for example a runtime mesh of generated floors that
// has no collider yet. A mirroring transform keeps the triangles facing out.
void appendMeshGeometry(NavigationGeometry& geometry, const MeshAsset& mesh, const glm::mat4& transform);

// A navigation mesh over a scene's static geometry, built with Recast and
// queried with Detour, which stay private. One mesh covers the whole scene
// (a single tile); bake again after the static geometry changes.
class NavigationWorld {
  public:
    NavigationWorld();
    ~NavigationWorld();
    NavigationWorld(const NavigationWorld&) = delete;
    NavigationWorld& operator=(const NavigationWorld&) = delete;

    // Builds the mesh from the scene's static colliders; replaces any mesh.
    // Fails, leaving no mesh, when nothing walkable is found.
    NavigationResult bake(const Scene& scene, const AssetManager& assets, const NavMeshBuildSettings& settings = {});
    NavigationResult bake(const NavigationGeometry& geometry, const NavMeshBuildSettings& settings = {});
    void clear();
    bool baked() const;
    const NavMeshStats& stats() const;
    const NavMeshBuildSettings& settings() const;

    // The corners of the shortest path from start to end along the mesh,
    // start included. A complete path ends at end; a partial one at the
    // reachable point nearest end. None (path empty) when either point is
    // off the mesh or nothing is baked.
    NavPath findPath(const glm::vec3& start, const glm::vec3& end, std::vector<glm::vec3>& path) const;
    // The nearest point on the mesh within extents of point (half sizes).
    std::optional<glm::vec3> nearestPoint(const glm::vec3& point, const glm::vec3& extents = {2.0f, 4.0f, 2.0f}) const;
    // Records the mesh's polygon edges.
    void drawDebug(DebugDraw& output, const DebugStyle& style = {.color = {0.2f, 0.8f, 1.0f, 1.0f}}) const;

    // Changes whenever the mesh is baked or cleared, so what holds onto the
    // mesh (NavigationCrowd) knows to start over.
    uint64_t generation() const;

  private:
    friend class NavigationCrowd;
    // Detour's mesh for the crowd; null unless baked.
    const dtNavMesh* nativeNavMesh() const;

    class Impl;
    std::unique_ptr<Impl> m_impl;
    uint64_t m_generation{0};
};
