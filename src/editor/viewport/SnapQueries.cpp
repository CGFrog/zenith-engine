#include "SnapQueries.hpp"

#include "editor/interaction/EditorCamera.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
bool excludedEntity(std::span<const Entity> excluded, Entity entity) {
    return std::ranges::binary_search(excluded, entity);
}

float aspectRatio(const SnapView& view) {
    return view.viewSize.x / std::max(view.viewSize.y, 1.0f);
}

glm::vec2 normalizedPoint(const SnapView& view, glm::vec2 viewPoint) {
    return viewPoint / glm::max(view.viewSize, glm::vec2(1.0f));
}

// The closest projected vertex so far: squared screen distance, then depth.
struct VertexCandidate {
    glm::vec3 position{0.0f};
    float screenDistanceSquared{std::numeric_limits<float>::max()};
    float depth{std::numeric_limits<float>::max()};
    bool found{false};
};

void considerVertex(VertexCandidate& best, const glm::vec3& position, float screenDistanceSquared, float depth) {
    constexpr float tieTolerance = 1e-4f;
    const bool closer = screenDistanceSquared < best.screenDistanceSquared - tieTolerance;
    const bool tieButNearer =
        std::abs(screenDistanceSquared - best.screenDistanceSquared) <= tieTolerance && depth < best.depth;
    if (!best.found || closer || tieButNearer) {
        best = {.position = position, .screenDistanceSquared = screenDistanceSquared, .depth = depth, .found = true};
    }
}
} // namespace

std::vector<Entity> snapExclusions(const Scene& scene, std::span<const SceneEntityId> entities) {
    std::vector<Entity> excluded;
    std::vector<Entity> pending;
    for (SceneEntityId id : entities) {
        if (const Entity entity = scene.findEntity(id); entity != entt::null) {
            pending.push_back(entity);
        }
    }
    while (!pending.empty()) {
        const Entity entity = pending.back();
        pending.pop_back();
        excluded.push_back(entity);
        if (const auto* relationship = scene.registry().try_get<RelationshipComponent>(entity)) {
            pending.insert(pending.end(), relationship->children.begin(), relationship->children.end());
        }
    }
    std::ranges::sort(excluded);
    const auto duplicates = std::ranges::unique(excluded);
    excluded.erase(duplicates.begin(), duplicates.end());
    return excluded;
}

std::optional<SurfaceHit> surfaceHitAlongRay(const Scene& scene, const AssetManager& assets, const LensRay& ray,
                                             const std::vector<RenderObjectId>* isolatedObjects,
                                             std::span<const Entity> excluded) {
    // Hits are nearest first; the distance is along the normalized ray.
    for (const LensHit& hit : pickAllAlongRay(scene, assets, ray, isolatedObjects)) {
        if (!excludedEntity(excluded, hit.entity)) {
            const glm::vec3 normal = glm::dot(hit.normal, ray.direction) > 0.0f ? -hit.normal : hit.normal;
            return SurfaceHit{.position = ray.origin + ray.direction * hit.distance, .normal = normal};
        }
    }
    return std::nullopt;
}

std::optional<glm::vec3> surfaceSnapPoint(const Scene& scene, const AssetManager& assets, const SnapView& view,
                                          glm::vec2 viewPoint, std::span<const Entity> excluded) {
    const LensRay ray = lensRay(view.lens, normalizedPoint(view, viewPoint), aspectRatio(view));
    if (const std::optional<SurfaceHit> hit = surfaceHitAlongRay(scene, assets, ray, view.isolatedObjects, excluded)) {
        return hit->position;
    }
    return std::nullopt;
}

Placement placementUnderMouse(const Scene& scene, const AssetManager& assets, const SnapView& view, glm::vec2 viewPoint,
                              float fallbackDepth, std::span<const Entity> excluded) {
    const LensRay ray = lensRay(view.lens, normalizedPoint(view, viewPoint), aspectRatio(view));
    if (const std::optional<SurfaceHit> hit = surfaceHitAlongRay(scene, assets, ray, view.isolatedObjects, excluded)) {
        return {.position = hit->position, .normal = hit->normal};
    }
    const glm::vec3 forward = glm::normalize(view.lens.forward);
    const float alongRay = fallbackDepth / std::max(glm::dot(ray.direction, forward), 1e-4f);
    return {.position = ray.origin + ray.direction * alongRay};
}

std::optional<glm::vec3> vertexSnapPoint(const Scene& scene, const AssetManager& assets, const SnapView& view,
                                         glm::vec2 viewPoint, float radiusPixels, std::span<const Entity> excluded) {
    const glm::mat4 projection = lensProjectionMatrix(view.lens, aspectRatio(view));
    const glm::mat4 viewProjection = projection * lensViewMatrix(view.lens);
    const float radiusSquared = radiusPixels * radiusPixels;
    // Pixels per world unit at view depth 1, for culling whole meshes.
    const float pixelsPerUnit = std::abs(projection[1][1]) * view.viewSize.y * 0.5f;
    const auto toScreen = [&view](const glm::vec4& clip) {
        return (glm::vec2(clip) / clip.w * 0.5f + 0.5f) * view.viewSize;
    };
    VertexCandidate best;
    forEachPickableMesh(scene, view.isolatedObjects, [&](Entity meshEntity, const glm::mat4& world, Entity picked) {
        if (excludedEntity(excluded, picked) || excludedEntity(excluded, meshEntity)) {
            return;
        }
        const MeshAsset* mesh = assets.mesh(scene.registry().get<MeshRendererComponent>(meshEntity).mesh);
        if (mesh == nullptr) {
            return;
        }
        // Skip meshes whose bounds project too far from the point.
        const glm::vec4 centerClip = viewProjection * world * glm::vec4(mesh->bounds.center, 1.0f);
        const float scale = std::max(
            {glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])), glm::length(glm::vec3(world[2]))});
        const float radius = mesh->bounds.radius * scale;
        if (centerClip.w > radius) {
            const float boundsPixels = radius * pixelsPerUnit / centerClip.w;
            if (glm::length(toScreen(centerClip) - viewPoint) - boundsPixels > radiusPixels) {
                return;
            }
        }
        for (const Vertex& vertex : mesh->vertices) {
            const glm::vec4 position = world * glm::vec4(vertex.position, 1.0f);
            const glm::vec4 clip = viewProjection * position;
            // Behind the lens or beyond its clip range.
            if (clip.w <= view.lens.nearPlane || clip.z < 0.0f || clip.z > clip.w) {
                continue;
            }
            const glm::vec2 offset = toScreen(clip) - viewPoint;
            const float distanceSquared = glm::dot(offset, offset);
            if (distanceSquared <= radiusSquared) {
                considerVertex(best, glm::vec3(position), distanceSquared, clip.w);
            }
        }
    });
    return best.found ? std::optional<glm::vec3>(best.position) : std::nullopt;
}

std::optional<glm::vec3> snapTargetPoint(SnapTarget target, const Scene& scene, const AssetManager& assets,
                                         const SnapView& view, glm::vec2 viewPoint, std::span<const Entity> excluded) {
    switch (target) {
    case SnapTarget::Surface:
        return surfaceSnapPoint(scene, assets, view, viewPoint, excluded);
    case SnapTarget::Vertex:
        return vertexSnapPoint(scene, assets, view, viewPoint, vertexSnapRadiusPixels, excluded);
    case SnapTarget::Increment:
        break;
    }
    return std::nullopt;
}
