#include "IconPicking.hpp"

#include "editor/interaction/EditorCamera.hpp"
#include "editor/viewport/ViewportPicker.hpp"
#include "engine/audio/AudioSystem.hpp"
#include "engine/scene/ParticleEmitter.hpp"
#include "engine/scene/SceneVisibility.hpp"

#include <algorithm>
#include <optional>
#include <unordered_set>

namespace {
// How far in front of an icon (metres) a surface must be to hide it, so an
// icon on a surface still counts.
constexpr float iconOcclusionTolerance = 0.01f;

// A point's position in view pixels, or nullopt behind the lens.
std::optional<glm::vec2> projectToView(const glm::mat4& viewProjection, const RenderView& lens, glm::vec2 viewSize,
                                       const glm::vec3& point) {
    const glm::vec4 clip = viewProjection * glm::vec4(point, 1.0f);
    if (clip.w <= lens.nearPlane) {
        return std::nullopt;
    }
    return (glm::vec2(clip) / clip.w * 0.5f + 0.5f) * viewSize;
}

glm::mat4 viewProjectionOf(const RenderView& lens, glm::vec2 viewSize) {
    return lensProjectionMatrix(lens, viewSize.x / std::max(viewSize.y, 1.0f)) * lensViewMatrix(lens);
}

void appendUnique(std::vector<SceneEntityId>& entities, SceneEntityId entity) {
    if (std::ranges::find(entities, entity) == entities.end()) {
        entities.push_back(entity);
    }
}
} // namespace

std::vector<SceneGizmoAnchor> pickableIcons(const SceneGizmoView& view, const SceneGizmoOverlays& overlays) {
    const Scene& scene = view.scene;
    const SceneVisibility visibility(scene, SceneVisibilityMode::Editor);
    const auto pickable = [&](Entity entity) {
        return visibility.selectable(entity) &&
               (view.isolatedObjects == nullptr ||
                std::ranges::binary_search(*view.isolatedObjects,
                                           static_cast<RenderObjectId>(entt::to_integral(entity))));
    };
    std::vector<SceneGizmoAnchor> icons;
    std::unordered_set<uint64_t> listed;
    for (const SceneGizmoAnchor& anchor : sceneGizmoAnchors(view, overlays)) {
        if (const Entity entity = scene.findEntity(anchor.entity); entity != entt::null && pickable(entity)) {
            icons.push_back(anchor);
            listed.insert(anchor.entity.value);
        }
    }
    // Objects the overlays draw nothing for, marked by their origins.
    const entt::registry& registry = scene.registry();
    const auto addOrigins = [&]<typename Component>() {
        for (const Entity entity : registry.view<const Component, const TransformComponent>()) {
            const SceneEntityId id = scene.id(entity);
            if (pickable(entity) && listed.insert(id.value).second) {
                icons.push_back(
                    {.entity = id, .position = glm::vec3(registry.get<TransformComponent>(entity).world[3])});
            }
        }
    };
    addOrigins.operator()<AudioEmitterComponent>();
    addOrigins.operator()<AudioListenerComponent>();
    addOrigins.operator()<ParticleEmitterComponent>();
    addOrigins.operator()<VolumetricFogComponent>();
    return icons;
}

std::vector<SceneEntityId> iconsNearPoint(std::span<const SceneGizmoAnchor> icons, const RenderView& lens,
                                          glm::vec2 viewSize, glm::vec2 point, float radiusPixels) {
    struct Hit {
        SceneEntityId entity;
        float screenDistance{0.0f};
        float depth{0.0f};
    };

    const glm::mat4 viewProjection = viewProjectionOf(lens, viewSize);
    std::vector<Hit> hits;
    for (const SceneGizmoAnchor& icon : icons) {
        const std::optional<glm::vec2> screen = projectToView(viewProjection, lens, viewSize, icon.position);
        if (!screen) {
            continue;
        }
        const float distance = glm::length(*screen - point);
        if (distance <= radiusPixels) {
            hits.push_back({.entity = icon.entity,
                            .screenDistance = distance,
                            .depth = glm::dot(icon.position - lens.position, lens.forward)});
        }
    }
    std::ranges::stable_sort(hits, [](const Hit& left, const Hit& right) {
        if (left.screenDistance != right.screenDistance) {
            return left.screenDistance < right.screenDistance;
        }
        return left.depth < right.depth;
    });
    std::vector<SceneEntityId> entities;
    for (const Hit& hit : hits) {
        appendUnique(entities, hit.entity);
    }
    return entities;
}

std::vector<SceneEntityId> iconsInRegion(std::span<const SceneGizmoAnchor> icons, const RenderView& lens,
                                         glm::vec2 viewSize, const RegionSelection& region) {
    const glm::mat4 viewProjection = viewProjectionOf(lens, viewSize);
    std::vector<SceneEntityId> entities;
    for (const SceneGizmoAnchor& icon : icons) {
        const std::optional<glm::vec2> screen = projectToView(viewProjection, lens, viewSize, icon.position);
        if (screen && regionContains(region, *screen)) {
            appendUnique(entities, icon.entity);
        }
    }
    return entities;
}

void mergeIconPicks(const ViewportPickQuery& query, const Scene& scene, const AssetManager& assets,
                    ViewportPickOutcome& outcome) {
    if (query.icons.empty()) {
        return;
    }
    if (query.kind == ViewportPickKind::Region) {
        for (SceneEntityId icon : iconsInRegion(query.icons, query.lens, query.viewSize, query.region)) {
            appendUnique(outcome.entities, icon);
        }
        return;
    }
    const std::vector<SceneEntityId> near =
        iconsNearPoint(query.icons, query.lens, query.viewSize, query.point, iconPickRadiusPixels);
    if (near.empty()) {
        return;
    }
    // The nearest surface along the click's ray, which hides icons behind it.
    const std::vector<RenderObjectId>* isolation = query.isolatedObjects ? &*query.isolatedObjects : nullptr;
    const std::vector<LensHit> surfaces =
        pickAllThroughLens(scene, assets, query.lens, query.point / glm::max(query.viewSize, glm::vec2(1.0f)),
                           query.viewSize.x / std::max(query.viewSize.y, 1.0f), isolation);
    std::vector<SceneEntityId> visible;
    std::vector<SceneEntityId> hidden;
    for (SceneEntityId icon : near) {
        const auto anchor = std::ranges::find(query.icons, icon, &SceneGizmoAnchor::entity);
        const float iconDistance = glm::length(anchor->position - query.lens.position);
        const auto blocker = std::ranges::find_if(surfaces, [&](const LensHit& hit) {
            return scene.id(hit.entity) != icon && hit.distance < iconDistance - iconOcclusionTolerance;
        });
        (blocker == surfaces.end() ? visible : hidden).push_back(icon);
    }
    if (query.kind == ViewportPickKind::Click) {
        if (!visible.empty()) {
            outcome.entities = {visible.front()};
        }
        return;
    }
    for (SceneEntityId entity : outcome.entities) {
        appendUnique(visible, entity);
    }
    for (SceneEntityId entity : hidden) {
        appendUnique(visible, entity);
    }
    outcome.entities = std::move(visible);
}
