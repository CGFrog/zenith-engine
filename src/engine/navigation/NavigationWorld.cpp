#define GLM_ENABLE_EXPERIMENTAL
#include "NavigationWorld.hpp"

#include "engine/assets/Assets.hpp"
#include "engine/physics/PhysicsComponents.hpp"

#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <Recast.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <utility>

namespace {
constexpr int maximumPathPolygons = 512;
constexpr int maximumQueryNodes = 4096;
// Every walkable polygon carries this flag; queries include it.
constexpr unsigned short walkableFlag = 1;

struct Pose {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
};

bool decompose(const glm::mat4& matrix, Pose& pose) {
    glm::vec3 skew;
    glm::vec4 perspective;
    if (!glm::decompose(matrix, pose.scale, pose.rotation, pose.position, skew, perspective)) {
        return false;
    }
    pose.rotation = glm::normalize(pose.rotation);
    pose.scale = glm::abs(pose.scale);
    return true;
}

// The triangles of a mesh's full-detail ranges (as physics collides with),
// indexing its vertices from firstVertex in geometry. flipWinding reverses
// each triangle, for a mirroring transform.
void appendMeshTriangles(NavigationGeometry& geometry, const MeshAsset& asset, uint32_t firstVertex, bool flipWinding) {
    const auto add = [&](uint32_t a, uint32_t b, uint32_t c) {
        const auto count = static_cast<uint32_t>(asset.vertices.size());
        if (a < count && b < count && c < count) {
            if (flipWinding) {
                std::swap(b, c);
            }
            geometry.indices.insert(geometry.indices.end(), {firstVertex + a, firstVertex + b, firstVertex + c});
        }
    };
    if (asset.indices.empty()) {
        for (uint32_t index = 0; index + 2 < asset.vertices.size(); index += 3) {
            add(index, index + 1, index + 2);
        }
    } else if (asset.primitives.empty()) {
        for (size_t index = 0; index + 2 < asset.indices.size(); index += 3) {
            add(asset.indices[index], asset.indices[index + 1], asset.indices[index + 2]);
        }
    } else {
        for (const MeshPrimitive& primitive : asset.primitives) {
            const size_t end = std::min<size_t>(static_cast<size_t>(primitive.firstIndex) + primitive.indexCount,
                                                asset.indices.size());
            for (size_t index = primitive.firstIndex; index + 2 < end; index += 3) {
                add(asset.indices[index], asset.indices[index + 1], asset.indices[index + 2]);
            }
        }
    }
}

class GeometryWriter {
  public:
    GeometryWriter(NavigationGeometry& geometry, const Pose& pose)
        : m_geometry(geometry)
        , m_pose(pose) {}

    // A vertex in the collider's space (already scaled), placed by the pose.
    uint32_t vertex(const glm::vec3& local) {
        m_geometry.vertices.push_back(m_pose.position + m_pose.rotation * local);
        return static_cast<uint32_t>(m_geometry.vertices.size() - 1);
    }

    void triangle(uint32_t a, uint32_t b, uint32_t c) {
        m_geometry.indices.insert(m_geometry.indices.end(), {a, b, c});
    }

    // A triangle wound so its normal points away from center (Recast reads
    // the winding to find upward-facing, walkable triangles).
    void outward(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& center) {
        const glm::vec3 normal = glm::cross(b - a, c - a);
        if (glm::dot(normal, (a + b + c) / 3.0f - center) < 0.0f) {
            triangle(vertex(a), vertex(c), vertex(b));
        } else {
            triangle(vertex(a), vertex(b), vertex(c));
        }
    }

    void box(const glm::vec3& halfExtents) {
        // Each face's outward normal and two edges with u x v = normal.
        const std::array<std::array<glm::vec3, 3>, 6> faces{{{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}},
                                                             {{{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}}},
                                                             {{{0, 1, 0}, {0, 0, 1}, {1, 0, 0}}},
                                                             {{{0, -1, 0}, {1, 0, 0}, {0, 0, 1}}},
                                                             {{{0, 0, 1}, {1, 0, 0}, {0, 1, 0}}},
                                                             {{{0, 0, -1}, {0, 1, 0}, {1, 0, 0}}}}};
        for (const auto& [normal, uAxis, vAxis] : faces) {
            const glm::vec3 center = normal * halfExtents;
            const glm::vec3 u = uAxis * halfExtents;
            const glm::vec3 v = vAxis * halfExtents;
            const uint32_t p0 = vertex(center - u - v);
            const uint32_t p1 = vertex(center + u - v);
            const uint32_t p2 = vertex(center + u + v);
            const uint32_t p3 = vertex(center - u + v);
            triangle(p0, p1, p2);
            triangle(p0, p2, p3);
        }
    }

    // A capsule along Y (a sphere when halfHeight is zero), as a coarse
    // latitude-longitude mesh.
    void capsule(float radius, float halfHeight) {
        constexpr int segments = 12;
        constexpr int rings = 6;
        const auto point = [&](int ring, int segment) {
            const float latitude = glm::pi<float>() * (static_cast<float>(ring) / rings - 0.5f);
            const float longitude = glm::two_pi<float>() * static_cast<float>(segment) / segments;
            const float y = std::sin(latitude) * radius + (ring * 2 >= rings ? halfHeight : -halfHeight);
            return glm::vec3(std::cos(latitude) * std::cos(longitude) * radius, y,
                             std::cos(latitude) * std::sin(longitude) * radius);
        };
        for (int ring = 0; ring < rings; ++ring) {
            for (int segment = 0; segment < segments; ++segment) {
                const glm::vec3 a = point(ring, segment);
                const glm::vec3 b = point(ring, segment + 1);
                const glm::vec3 c = point(ring + 1, segment + 1);
                const glm::vec3 d = point(ring + 1, segment);
                const glm::vec3 center(0.0f, (a.y + c.y) * 0.5f, 0.0f);
                if (glm::length(glm::cross(b - a, c - a)) > 1.0e-6f) {
                    outward(a, b, c, center);
                }
                if (glm::length(glm::cross(c - a, d - a)) > 1.0e-6f) {
                    outward(a, c, d, center);
                }
            }
        }
    }

    void mesh(const MeshAsset& asset, const glm::vec3& scale) {
        const uint32_t first = static_cast<uint32_t>(m_geometry.vertices.size());
        for (const Vertex& source : asset.vertices) {
            vertex(source.position * scale);
        }
        appendMeshTriangles(m_geometry, asset, first, false);
    }

  private:
    NavigationGeometry& m_geometry;
    const Pose& m_pose;
};

template <typename Object, void (*Free)(Object*)> struct RecastDeleter {
    void operator()(Object* object) const {
        Free(object);
    }
};

template <typename Object, void (*Free)(Object*)>
using RecastPointer = std::unique_ptr<Object, RecastDeleter<Object, Free>>;

const float* floats(const glm::vec3& value) {
    return &value.x;
}
} // namespace

NavigationGeometry gatherNavigationGeometry(const Scene& scene, const AssetManager& assets, uint32_t layerMask) {
    NavigationGeometry geometry;
    const entt::registry& registry = scene.registry();
    for (const Entity entity :
         registry.view<const TransformComponent, const ColliderComponent, const RigidBodyComponent>()) {
        const auto& body = registry.get<const RigidBodyComponent>(entity);
        if (body.motionType != PhysicsMotionType::Static || body.sensor || body.layer.index >= maxPhysicsLayers ||
            (layerMask & (1u << body.layer.index)) == 0) {
            continue;
        }
        Pose pose;
        // Resolved from the hierarchy: a bake right after spawning or moving
        // entities must not wait for the next updateWorldTransforms.
        if (!decompose(scene.worldTransform(entity), pose)) {
            continue;
        }
        GeometryWriter writer(geometry, pose);
        const ColliderShape& shape = registry.get<const ColliderComponent>(entity).shape;
        if (const auto* box = std::get_if<BoxCollider>(&shape)) {
            writer.box(box->halfExtents * pose.scale);
        } else if (const auto* sphere = std::get_if<SphereCollider>(&shape)) {
            writer.capsule(sphere->radius * std::max({pose.scale.x, pose.scale.y, pose.scale.z}), 0.0f);
        } else if (const auto* capsule = std::get_if<CapsuleCollider>(&shape)) {
            writer.capsule(capsule->radius * std::max(pose.scale.x, pose.scale.z), capsule->halfHeight * pose.scale.y);
        } else if (const auto* triangles = std::get_if<TriangleMeshCollider>(&shape)) {
            if (const MeshAsset* mesh = assets.mesh(triangles->mesh)) {
                // The mesh's vertices are scaled before the pose places them.
                const Pose unscaled{.position = pose.position, .rotation = pose.rotation};
                GeometryWriter(geometry, unscaled).mesh(*mesh, pose.scale);
            }
        }
    }
    return geometry;
}

void appendMeshGeometry(NavigationGeometry& geometry, const MeshAsset& mesh, const glm::mat4& transform) {
    const auto first = static_cast<uint32_t>(geometry.vertices.size());
    geometry.vertices.reserve(geometry.vertices.size() + mesh.vertices.size());
    for (const Vertex& vertex : mesh.vertices) {
        geometry.vertices.emplace_back(transform * glm::vec4(vertex.position, 1.0f));
    }
    appendMeshTriangles(geometry, mesh, first, glm::determinant(glm::mat3(transform)) < 0.0f);
}

class NavigationWorld::Impl {
  public:
    RecastPointer<dtNavMesh, dtFreeNavMesh> navMesh;
    RecastPointer<dtNavMeshQuery, dtFreeNavMeshQuery> query;
    NavMeshStats stats;
    NavMeshBuildSettings settings;
};

NavigationWorld::NavigationWorld()
    : m_impl(std::make_unique<Impl>()) {}

NavigationWorld::~NavigationWorld() = default;

NavigationResult NavigationWorld::bake(const Scene& scene, const AssetManager& assets,
                                       const NavMeshBuildSettings& settings) {
    return bake(gatherNavigationGeometry(scene, assets, settings.layerMask), settings);
}

NavigationResult NavigationWorld::bake(const NavigationGeometry& geometry, const NavMeshBuildSettings& settings) {
    clear();
    if (geometry.indices.size() < 3 || geometry.vertices.empty()) {
        return {.error = "The scene has no static colliders to walk on"};
    }
    if (!(settings.cellSize > 0.0f) || !(settings.cellHeight > 0.0f) || !(settings.agentHeight > 0.0f) ||
        !(settings.agentRadius >= 0.0f) || !(settings.agentMaxClimb >= 0.0f)) {
        return {.error = "Navigation mesh cell and agent sizes must be positive"};
    }
    const float* vertices = floats(geometry.vertices.front());
    const int vertexCount = static_cast<int>(geometry.vertices.size());
    std::vector<int> triangles(geometry.indices.begin(), geometry.indices.end());
    const int triangleCount = static_cast<int>(triangles.size() / 3);

    rcConfig config{};
    config.cs = settings.cellSize;
    config.ch = settings.cellHeight;
    config.walkableSlopeAngle = settings.agentMaxSlopeDegrees;
    config.walkableHeight = static_cast<int>(std::ceil(settings.agentHeight / config.ch));
    config.walkableClimb = static_cast<int>(std::floor(settings.agentMaxClimb / config.ch));
    config.walkableRadius = static_cast<int>(std::ceil(settings.agentRadius / config.cs));
    config.maxEdgeLen = static_cast<int>(settings.edgeMaxLength / config.cs);
    config.maxSimplificationError = settings.edgeMaxError;
    config.minRegionArea = static_cast<int>(settings.regionMinimumArea * settings.regionMinimumArea);
    config.mergeRegionArea = static_cast<int>(settings.regionMergeArea * settings.regionMergeArea);
    config.maxVertsPerPoly = DT_VERTS_PER_POLYGON;
    config.detailSampleDist = settings.detailSampleDistance < 0.9f ? 0.0f : config.cs * settings.detailSampleDistance;
    config.detailSampleMaxError = config.ch * settings.detailSampleMaxError;
    rcCalcBounds(vertices, vertexCount, config.bmin, config.bmax);
    rcCalcGridSize(config.bmin, config.bmax, config.cs, &config.width, &config.height);

    rcContext context(false);
    RecastPointer<rcHeightfield, rcFreeHeightField> heightfield(rcAllocHeightfield());
    if (!heightfield || !rcCreateHeightfield(&context, *heightfield, config.width, config.height, config.bmin,
                                             config.bmax, config.cs, config.ch)) {
        return {.error = "Recast could not create the heightfield"};
    }
    std::vector<unsigned char> areas(static_cast<size_t>(triangleCount), RC_NULL_AREA);
    rcMarkWalkableTriangles(&context, config.walkableSlopeAngle, vertices, vertexCount, triangles.data(), triangleCount,
                            areas.data());
    if (!rcRasterizeTriangles(&context, vertices, vertexCount, triangles.data(), areas.data(), triangleCount,
                              *heightfield, config.walkableClimb)) {
        return {.error = "Recast could not rasterize the geometry"};
    }
    rcFilterLowHangingWalkableObstacles(&context, config.walkableClimb, *heightfield);
    rcFilterLedgeSpans(&context, config.walkableHeight, config.walkableClimb, *heightfield);
    rcFilterWalkableLowHeightSpans(&context, config.walkableHeight, *heightfield);

    RecastPointer<rcCompactHeightfield, rcFreeCompactHeightfield> compact(rcAllocCompactHeightfield());
    if (!compact ||
        !rcBuildCompactHeightfield(&context, config.walkableHeight, config.walkableClimb, *heightfield, *compact) ||
        !rcErodeWalkableArea(&context, config.walkableRadius, *compact) || !rcBuildDistanceField(&context, *compact) ||
        !rcBuildRegions(&context, *compact, 0, config.minRegionArea, config.mergeRegionArea)) {
        return {.error = "Recast could not build the walkable regions"};
    }
    RecastPointer<rcContourSet, rcFreeContourSet> contours(rcAllocContourSet());
    if (!contours ||
        !rcBuildContours(&context, *compact, config.maxSimplificationError, config.maxEdgeLen, *contours)) {
        return {.error = "Recast could not build the region contours"};
    }
    RecastPointer<rcPolyMesh, rcFreePolyMesh> polygons(rcAllocPolyMesh());
    if (!polygons || !rcBuildPolyMesh(&context, *contours, config.maxVertsPerPoly, *polygons)) {
        return {.error = "Recast could not build the polygon mesh"};
    }
    RecastPointer<rcPolyMeshDetail, rcFreePolyMeshDetail> detail(rcAllocPolyMeshDetail());
    if (!detail || !rcBuildPolyMeshDetail(&context, *polygons, *compact, config.detailSampleDist,
                                          config.detailSampleMaxError, *detail)) {
        return {.error = "Recast could not build the detail mesh"};
    }
    if (polygons->npolys == 0) {
        return {.error = "The scene has no walkable area for the agent"};
    }
    for (int index = 0; index < polygons->npolys; ++index) {
        polygons->flags[index] = polygons->areas[index] == RC_WALKABLE_AREA ? walkableFlag : 0;
    }

    dtNavMeshCreateParams parameters{};
    parameters.verts = polygons->verts;
    parameters.vertCount = polygons->nverts;
    parameters.polys = polygons->polys;
    parameters.polyAreas = polygons->areas;
    parameters.polyFlags = polygons->flags;
    parameters.polyCount = polygons->npolys;
    parameters.nvp = polygons->nvp;
    parameters.detailMeshes = detail->meshes;
    parameters.detailVerts = detail->verts;
    parameters.detailVertsCount = detail->nverts;
    parameters.detailTris = detail->tris;
    parameters.detailTriCount = detail->ntris;
    parameters.walkableHeight = settings.agentHeight;
    parameters.walkableRadius = settings.agentRadius;
    parameters.walkableClimb = settings.agentMaxClimb;
    rcVcopy(parameters.bmin, polygons->bmin);
    rcVcopy(parameters.bmax, polygons->bmax);
    parameters.cs = config.cs;
    parameters.ch = config.ch;
    parameters.buildBvTree = true;
    unsigned char* navData = nullptr;
    int navDataSize = 0;
    if (!dtCreateNavMeshData(&parameters, &navData, &navDataSize)) {
        return {.error = "Detour could not create the navigation mesh data"};
    }
    RecastPointer<dtNavMesh, dtFreeNavMesh> navMesh(dtAllocNavMesh());
    if (!navMesh || dtStatusFailed(navMesh->init(navData, navDataSize, DT_TILE_FREE_DATA))) {
        dtFree(navData);
        return {.error = "Detour could not load the navigation mesh"};
    }
    RecastPointer<dtNavMeshQuery, dtFreeNavMeshQuery> query(dtAllocNavMeshQuery());
    if (!query || dtStatusFailed(query->init(navMesh.get(), maximumQueryNodes))) {
        return {.error = "Detour could not create the navigation query"};
    }
    m_impl->navMesh = std::move(navMesh);
    m_impl->query = std::move(query);
    m_impl->settings = settings;
    m_impl->stats = {.inputTriangles = static_cast<uint32_t>(triangleCount),
                     .polygons = static_cast<uint32_t>(polygons->npolys),
                     .vertices = static_cast<uint32_t>(polygons->nverts)};
    return {.success = true};
}

void NavigationWorld::clear() {
    ++m_generation;
    m_impl->query.reset();
    m_impl->navMesh.reset();
    m_impl->stats = {};
}

bool NavigationWorld::baked() const {
    return m_impl->query != nullptr;
}

uint64_t NavigationWorld::generation() const {
    return m_generation;
}

const dtNavMesh* NavigationWorld::nativeNavMesh() const {
    return m_impl->navMesh.get();
}

const NavMeshStats& NavigationWorld::stats() const {
    return m_impl->stats;
}

const NavMeshBuildSettings& NavigationWorld::settings() const {
    return m_impl->settings;
}

NavPath NavigationWorld::findPath(const glm::vec3& start, const glm::vec3& end, std::vector<glm::vec3>& path) const {
    path.clear();
    if (!baked()) {
        return NavPath::None;
    }
    dtQueryFilter filter;
    filter.setIncludeFlags(walkableFlag);
    const glm::vec3 extents(2.0f, 4.0f, 2.0f);
    dtPolyRef startPolygon = 0;
    dtPolyRef endPolygon = 0;
    glm::vec3 startPoint;
    glm::vec3 endPoint;
    const dtNavMeshQuery& query = *m_impl->query;
    if (dtStatusFailed(query.findNearestPoly(floats(start), floats(extents), &filter, &startPolygon, &startPoint.x)) ||
        dtStatusFailed(query.findNearestPoly(floats(end), floats(extents), &filter, &endPolygon, &endPoint.x)) ||
        startPolygon == 0 || endPolygon == 0) {
        return NavPath::None;
    }
    std::array<dtPolyRef, maximumPathPolygons> corridor{};
    int corridorSize = 0;
    const dtStatus status = query.findPath(startPolygon, endPolygon, &startPoint.x, &endPoint.x, &filter,
                                           corridor.data(), &corridorSize, maximumPathPolygons);
    if (dtStatusFailed(status) || corridorSize == 0) {
        return NavPath::None;
    }
    // A corridor that stops short of the end polygon leads as close to it as
    // the mesh allows: aim at the point of its last polygon nearest the end.
    const dtPolyRef last = corridor[static_cast<size_t>(corridorSize - 1)];
    const bool partial = dtStatusDetail(status, DT_PARTIAL_RESULT) || last != endPolygon;
    glm::vec3 target = endPoint;
    if (partial && dtStatusFailed(query.closestPointOnPoly(last, &endPoint.x, &target.x, nullptr))) {
        return NavPath::None;
    }
    std::array<float, maximumPathPolygons * 3> corners{};
    int cornerCount = 0;
    if (dtStatusFailed(query.findStraightPath(&startPoint.x, &target.x, corridor.data(), corridorSize, corners.data(),
                                              nullptr, nullptr, &cornerCount, maximumPathPolygons))) {
        return NavPath::None;
    }
    for (int index = 0; index < cornerCount; ++index) {
        path.emplace_back(corners[index * 3], corners[index * 3 + 1], corners[index * 3 + 2]);
    }
    if (path.empty()) {
        return NavPath::None;
    }
    return partial ? NavPath::Partial : NavPath::Complete;
}

std::optional<glm::vec3> NavigationWorld::nearestPoint(const glm::vec3& point, const glm::vec3& extents) const {
    if (!baked()) {
        return std::nullopt;
    }
    dtQueryFilter filter;
    filter.setIncludeFlags(walkableFlag);
    dtPolyRef polygon = 0;
    glm::vec3 nearest;
    if (dtStatusFailed(m_impl->query->findNearestPoly(floats(point), floats(extents), &filter, &polygon, &nearest.x)) ||
        polygon == 0) {
        return std::nullopt;
    }
    return nearest;
}

void NavigationWorld::drawDebug(DebugDraw& output, const DebugStyle& style) const {
    if (!baked()) {
        return;
    }
    const dtNavMesh& navMesh = *m_impl->navMesh;
    for (int tileIndex = 0; tileIndex < navMesh.getMaxTiles(); ++tileIndex) {
        const dtMeshTile* tile = navMesh.getTile(tileIndex);
        if (tile == nullptr || tile->header == nullptr) {
            continue;
        }
        for (int polygonIndex = 0; polygonIndex < tile->header->polyCount; ++polygonIndex) {
            const dtPoly& polygon = tile->polys[polygonIndex];
            if (polygon.getType() == DT_POLYTYPE_OFFMESH_CONNECTION) {
                continue;
            }
            for (int corner = 0; corner < polygon.vertCount; ++corner) {
                const float* a = &tile->verts[polygon.verts[corner] * 3];
                const float* b = &tile->verts[polygon.verts[(corner + 1) % polygon.vertCount] * 3];
                // Lifted slightly so the lines do not fight the ground.
                const glm::vec3 lift(0.0f, 0.05f, 0.0f);
                output.line(glm::vec3(a[0], a[1], a[2]) + lift, glm::vec3(b[0], b[1], b[2]) + lift, style);
            }
        }
    }
}
