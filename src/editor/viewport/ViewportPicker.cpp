#include "ViewportPicker.hpp"

#include "editor/interaction/EditorCamera.hpp"
#include "editor/interaction/PickQueries.hpp"
#include "engine/scene/SceneVisibility.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
const std::vector<RenderObjectId>* isolationOf(const ViewportPickQuery& query) {
    return query.isolatedObjects ? &*query.isolatedObjects : nullptr;
}

glm::ivec2 pixelOf(glm::vec2 point) {
    return {static_cast<int32_t>(std::floor(point.x)), static_cast<int32_t>(std::floor(point.y))};
}

float aspectOf(const ViewportPickQuery& query) {
    return query.viewSize.x / std::max(query.viewSize.y, 1.0f);
}

glm::vec2 normalizedOf(const ViewportPickQuery& query, glm::vec2 point) {
    return point / glm::max(query.viewSize, glm::vec2(1.0f));
}

std::vector<SceneEntityId> stableIds(const Scene& scene, std::span<const Entity> entities) {
    std::vector<SceneEntityId> ids;
    ids.reserve(entities.size());
    for (Entity entity : entities) {
        ids.push_back(scene.id(entity));
    }
    return ids;
}

std::vector<SceneEntityId> occludedCandidates(const ViewportPickQuery& query, const Scene& scene,
                                              const AssetManager& assets) {
    std::vector<Entity> hits;
    for (const LensHit& hit : pickAllThroughLens(scene, assets, query.lens, normalizedOf(query, query.point),
                                                 aspectOf(query), isolationOf(query))) {
        hits.push_back(hit.entity);
    }
    return stableIds(scene, hits);
}

// The result's ids with every object the editor may not pick here zeroed:
// gone, hidden, unselectable, or outside the isolation. The renderer already
// draws only what the view shows, but unselectable objects are drawn.
std::vector<uint32_t> pickableIds(const ViewportPickQuery& query, const PickResult& result, const Scene& scene) {
    const SceneVisibility visibility(scene, SceneVisibilityMode::Editor);
    const std::vector<RenderObjectId>* isolation = isolationOf(query);
    std::vector<bool> allowed(result.objects.size() + 1, false);
    for (size_t index = 0; index < result.objects.size(); ++index) {
        const PickedObject& object = result.objects[index];
        const Entity entity = object.entity ? scene.findEntity(object.entity) : entt::null;
        allowed[index + 1] = entity != entt::null && visibility.selectable(entity) &&
                             (isolation == nullptr || std::ranges::binary_search(*isolation, object.object));
    }
    std::vector<uint32_t> ids = result.ids;
    for (uint32_t& id : ids) {
        if (id >= allowed.size() || !allowed[id]) {
            id = 0;
        }
    }
    return ids;
}
} // namespace

PickRect pickRectFor(const ViewportPickQuery& query) {
    if (query.kind != ViewportPickKind::Region) {
        const glm::ivec2 pixel = pixelOf(query.point);
        constexpr int32_t radius = static_cast<int32_t>(ViewportPicker::clickRadius);
        constexpr uint32_t size = ViewportPicker::clickRadius * 2 + 1;
        return {.x = pixel.x - radius, .y = pixel.y - radius, .width = size, .height = size};
    }
    const glm::vec2 minimum = glm::floor(regionMinimum(query.region));
    const glm::vec2 maximum = glm::ceil(regionMaximum(query.region));
    return {.x = static_cast<int32_t>(minimum.x),
            .y = static_cast<int32_t>(minimum.y),
            .width = static_cast<uint32_t>(std::max(maximum.x - minimum.x, 1.0f)),
            .height = static_cast<uint32_t>(std::max(maximum.y - minimum.y, 1.0f))};
}

ViewportPickOutcome resolveGpuPick(const ViewportPickQuery& query, const PickResult& result, const Scene& scene,
                                   const AssetManager& assets) {
    ViewportPickOutcome outcome{.kind = query.kind, .fromGpu = true};
    const std::vector<uint32_t> ids = pickableIds(query, result, scene);
    IdGrid grid = idGrid(result);
    grid.ids = ids;
    const glm::vec2 regionOrigin(static_cast<float>(result.region.x), static_cast<float>(result.region.y));
    const glm::ivec2 regionPixel(result.region.x, result.region.y);
    switch (query.kind) {
    case ViewportPickKind::Click: {
        const uint32_t id = pickPoint(grid, pixelOf(query.point) - regionPixel, ViewportPicker::clickRadius);
        outcome.entities = resolvePickedEntities(std::span(&id, 1), result.objects);
        break;
    }
    case ViewportPickKind::Cycle: {
        const std::vector<uint32_t> candidates =
            pickCandidates(grid, pixelOf(query.point) - regionPixel, ViewportPicker::clickRadius);
        const std::vector<SceneEntityId> visible = resolvePickedEntities(candidates, result.objects);
        outcome.entities = mergeCycleCandidates(visible, occludedCandidates(query, scene, assets));
        break;
    }
    case ViewportPickKind::Region: {
        std::vector<uint32_t> inside;
        if (query.region.shape == RegionShape::Box) {
            const PickRect rect = pickRectFor(query);
            inside = pickBox(grid, {.x = rect.x - result.region.x,
                                    .y = rect.y - result.region.y,
                                    .width = rect.width,
                                    .height = rect.height});
        } else {
            std::vector<glm::vec2> outline = query.region.points;
            for (glm::vec2& point : outline) {
                point -= regionOrigin;
            }
            inside = pickLasso(grid, outline);
        }
        outcome.entities = resolvePickedEntities(inside, result.objects);
        break;
    }
    }
    return outcome;
}

ViewportPickOutcome resolveCpuPick(const ViewportPickQuery& query, const Scene& scene, const AssetManager& assets) {
    ViewportPickOutcome outcome{.kind = query.kind, .fromGpu = false};
    switch (query.kind) {
    case ViewportPickKind::Click: {
        const Entity picked = pickThroughLens(scene, assets, query.lens, normalizedOf(query, query.point),
                                              aspectOf(query), isolationOf(query));
        if (picked != entt::null) {
            outcome.entities.push_back(scene.id(picked));
        }
        break;
    }
    case ViewportPickKind::Cycle:
        outcome.entities = occludedCandidates(query, scene, assets);
        break;
    case ViewportPickKind::Region: {
        const auto contains = [&query](glm::vec2 normalized) {
            return regionContains(query.region, normalized * query.viewSize);
        };
        const std::vector<Entity> inside =
            pickInsideLens(scene, assets, query.lens, aspectOf(query), contains, isolationOf(query));
        outcome.entities = stableIds(scene, inside);
        break;
    }
    }
    return outcome;
}

void ViewportPicker::start(ViewportPickProvider& provider, RenderViewId view, ViewportPickQuery query) {
    cancel(provider);
    PickTicket ticket;
    if (view && provider.available(view)) {
        ticket = provider.request(view, pickRectFor(query));
    }
    m_pending = Pending{.query = std::move(query), .ticket = ticket};
}

bool ViewportPicker::pending() const {
    return m_pending.has_value();
}

std::optional<ViewportPickOutcome> ViewportPicker::poll(ViewportPickProvider& provider, const Scene& scene,
                                                        const AssetManager& assets) {
    if (!m_pending) {
        return std::nullopt;
    }
    Pending& pending = *m_pending;
    if (pending.ticket) {
        const PickStatus status = provider.status(pending.ticket);
        if (status == PickStatus::Ready) {
            const PickResult result = provider.take(pending.ticket);
            ViewportPickOutcome outcome = resolveGpuPick(pending.query, result, scene, assets);
            m_pending.reset();
            return outcome;
        }
        if (status == PickStatus::Pending && pending.polls < frameBudget) {
            ++pending.polls;
            return std::nullopt;
        }
        // Failed, cancelled (the view resized), evicted, or too late; the
        // ticket is released either way.
        if (status == PickStatus::Pending) {
            provider.cancel(pending.ticket);
        }
        if (status != PickStatus::Unknown) {
            provider.take(pending.ticket);
        }
    }
    ViewportPickOutcome outcome = resolveCpuPick(pending.query, scene, assets);
    m_pending.reset();
    return outcome;
}

void ViewportPicker::cancel(ViewportPickProvider& provider) {
    if (m_pending && m_pending->ticket) {
        provider.cancel(m_pending->ticket);
        provider.take(m_pending->ticket);
    }
    m_pending.reset();
}
