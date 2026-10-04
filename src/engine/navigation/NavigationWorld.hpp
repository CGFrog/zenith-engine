#pragma once

#include "engine/graphics/DebugDraw.hpp"
#include "engine/scene/Scene.hpp"

#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class AssetManager;

// How a navigation mesh is built, in metres and degrees. The agent values
// describe the walkers the mesh is for: where they fit, what they climb.
struct NavMeshBuildSettings {
    float cellSize{0.2f};
    float cellHeight{0.1f};
    float agentHeight{1.8f};
    float agentRadius{0.4f};
    float agentMaxClimb{0.35f};
    float agentMaxSlopeDegrees{45.0f};
    // Smallest region kept, and size below which regions merge, in cells.
    float regionMinimumArea{8.0f};
    float regionMergeArea{20.0f};
    float edgeMaxLength{12.0f};
    float edgeMaxError{1.3f};
    float detailSampleDistance{6.0f};
    float detailSampleMaxError{1.0f};
    // The physics layers whose static colliders are walkable geometry.
    uint32_t layerMask{~0u};
};

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

// The triangles a navigation mesh is built from, in world space.
struct NavigationGeometry {
    std::vector<glm::vec3> vertices;
    std::vector<uint32_t> indices;
};

// The static, solid colliders on the mask's layers as triangles, shaped the
// way PhysicsWorld builds their bodies: boxes, spheres and capsules as
// meshes, triangle-mesh colliders from their MeshAsset's full-detail ranges.
NavigationGeometry gatherNavigationGeometry(const Scene& scene, const AssetManager& assets, uint32_t layerMask = ~0u);

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
    // start and end included. False (path empty) when either point is off the
    // mesh or end cannot be reached.
    bool findPath(const glm::vec3& start, const glm::vec3& end, std::vector<glm::vec3>& path) const;
    // The nearest point on the mesh within extents of point (half sizes).
    std::optional<glm::vec3> nearestPoint(const glm::vec3& point, const glm::vec3& extents = {2.0f, 4.0f, 2.0f}) const;
    // Records the mesh's polygon edges.
    void drawDebug(DebugDraw& output, const DebugStyle& style = {.color = {0.2f, 0.8f, 1.0f, 1.0f}}) const;

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
