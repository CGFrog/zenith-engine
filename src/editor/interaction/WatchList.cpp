#include "WatchList.hpp"

#include "engine/scene/ComponentTypes.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <utility>

bool WatchList::add(WatchedField field) {
    if (contains(field)) {
        return false;
    }
    m_fields.push_back(std::move(field));
    return true;
}

bool WatchList::remove(const WatchedField& field) {
    const auto found =
        std::ranges::find_if(m_fields, [&field](const WatchedField& watched) { return watched.sameField(field); });
    if (found == m_fields.end()) {
        return false;
    }
    m_fields.erase(found);
    return true;
}

void WatchList::clear() {
    m_fields.clear();
}

std::span<const WatchedField> WatchList::fields() const {
    return m_fields;
}

bool WatchList::contains(const WatchedField& field) const {
    return std::ranges::any_of(m_fields, [&field](const WatchedField& watched) { return watched.sameField(field); });
}

std::string watchedValueText(const Scene& scene, const AssetManager& assets, const SceneDocumentOptions& options,
                             const WatchedField& field) {
    const Entity entity = scene.findEntity(field.entity);
    if (entity == entt::null) {
        return "(no entity)";
    }
    const ComponentType* type = componentTypesFor(options).find(field.component);
    if (type == nullptr || !type->has(scene, entity)) {
        return "(no component)";
    }
    // Watching debugs a running game, so runtime state is shown too.
    SceneDocumentOptions saveOptions = options;
    saveOptions.purpose = SceneDocumentPurpose::SaveGame;
    nlohmann::json document;
    if (!type->save(scene, entity, {.assets = assets, .options = saveOptions}, document).success) {
        return "(no field)";
    }
    try {
        const nlohmann::json::json_pointer pointer(field.pointer);
        if (!document.contains(pointer)) {
            return "(no field)";
        }
        return document.at(pointer).dump();
    } catch (const nlohmann::json::exception&) {
        // A malformed pointer names no field.
        return "(no field)";
    }
}
