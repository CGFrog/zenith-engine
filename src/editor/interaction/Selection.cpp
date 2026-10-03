#include "Selection.hpp"

#include "engine/scene/SceneVisibility.hpp"

#include <algorithm>
#include <utility>

const std::vector<SceneEntityId>& Selection::ids() const {
    return m_ids;
}

bool Selection::empty() const {
    return m_ids.empty();
}

size_t Selection::size() const {
    return m_ids.size();
}

std::optional<SceneEntityId> Selection::active() const {
    if (m_ids.empty()) {
        return std::nullopt;
    }
    return m_ids.front();
}

bool Selection::contains(SceneEntityId id) const {
    return std::ranges::find(m_ids, id) != m_ids.end();
}

void Selection::clear() {
    m_ids.clear();
}

void Selection::assign(std::vector<SceneEntityId> ids) {
    m_ids = std::move(ids);
}

void Selection::selectOnly(SceneEntityId id) {
    m_ids = {id};
}

void Selection::click(SceneEntityId id, SelectionClickMode mode) {
    const auto found = std::ranges::find(m_ids, id);
    if (mode == SelectionClickMode::Replace) {
        m_ids = {id};
    } else if (found == m_ids.end()) {
        m_ids.insert(m_ids.begin(), id);
    } else if (found == m_ids.begin()) {
        m_ids.erase(found);
    } else {
        std::rotate(m_ids.begin(), found, found + 1);
    }
}

void Selection::clickRange(std::span<const SceneEntityId> displayed, SceneEntityId anchor, SceneEntityId clicked,
                           SelectionClickMode mode) {
    const std::vector<SceneEntityId> range = displayedRange(displayed, anchor, clicked);
    if (range.empty()) {
        return;
    }
    // The anchor stays active; without one in the range, the clicked row is.
    const SceneEntityId active = std::ranges::find(range, anchor) != range.end() ? anchor : clicked;
    std::vector<SceneEntityId> ids{active};
    if (mode == SelectionClickMode::Extend) {
        for (const SceneEntityId id : m_ids) {
            if (id != active) {
                ids.push_back(id);
            }
        }
    }
    for (const SceneEntityId id : range) {
        if (std::ranges::find(ids, id) == ids.end()) {
            ids.push_back(id);
        }
    }
    m_ids = std::move(ids);
}

void Selection::selectAll(const Scene& scene) {
    std::vector<SceneEntityId> all;
    if (!m_ids.empty() && scene.findEntity(m_ids.front()) != entt::null) {
        all.push_back(m_ids.front());
    }
    const SceneVisibility visibility(scene, SceneVisibilityMode::Editor);
    for (Entity entity : scene.registry().view<const TransformComponent>()) {
        const SceneEntityId id = scene.id(entity);
        if (visibility.selectable(entity) && (all.empty() || id != all.front())) {
            all.push_back(id);
        }
    }
    m_ids = std::move(all);
}

void Selection::invert(const Scene& scene) {
    std::vector<SceneEntityId> inverted;
    const SceneVisibility visibility(scene, SceneVisibilityMode::Editor);
    for (Entity entity : scene.registry().view<const TransformComponent>()) {
        const SceneEntityId id = scene.id(entity);
        if (!contains(id) && visibility.selectable(entity)) {
            inverted.push_back(id);
        }
    }
    m_ids = std::move(inverted);
}

void Selection::removeMissing(const Scene& scene) {
    std::erase_if(m_ids, [&scene](SceneEntityId id) { return scene.findEntity(id) == entt::null; });
}

std::vector<SceneEntityId> Selection::transformRoots(const Scene& scene) const {
    std::vector<SceneEntityId> roots;
    for (SceneEntityId id : m_ids) {
        const Entity entity = scene.findEntity(id);
        if (entity == entt::null) {
            continue;
        }
        bool ancestorSelected = false;
        for (Entity parent = scene.registry().get<RelationshipComponent>(entity).parent; parent != entt::null;
             parent = scene.registry().get<RelationshipComponent>(parent).parent) {
            if (contains(scene.id(parent))) {
                ancestorSelected = true;
                break;
            }
        }
        if (!ancestorSelected) {
            roots.push_back(id);
        }
    }
    return roots;
}

std::vector<SceneEntityId> displayedRange(std::span<const SceneEntityId> displayed, SceneEntityId anchor,
                                          SceneEntityId clicked) {
    const auto clickedRow = std::ranges::find(displayed, clicked);
    if (clickedRow == displayed.end()) {
        return {};
    }
    const auto anchorRow = std::ranges::find(displayed, anchor);
    if (anchorRow == displayed.end()) {
        return {clicked};
    }
    const auto first = std::min(anchorRow, clickedRow);
    const auto last = std::max(anchorRow, clickedRow);
    std::vector<SceneEntityId> range;
    for (auto row = first; row <= last; ++row) {
        if (std::ranges::find(range, *row) == range.end()) {
            range.push_back(*row);
        }
    }
    return range;
}
