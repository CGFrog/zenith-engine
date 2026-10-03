#include "PrefabAuthoring.hpp"

#include <algorithm>
#include <string_view>
#include <system_error>
#include <utility>

namespace {
// Whether pointer is ancestor (or equal) to other: "" contains everything,
// "/color" contains "/color/0".
bool containsPointer(std::string_view pointer, std::string_view other) {
    return pointer.empty() || other == pointer ||
           (other.size() > pointer.size() && other.starts_with(pointer) && other[pointer.size()] == '/');
}

bool sameOrRelated(std::string_view left, std::string_view right) {
    return containsPointer(left, right) || containsPointer(right, left);
}

PrefabResult instanceRootOf(const Scene& scene, Entity entity, Entity& root) {
    if (entity == entt::null || !scene.registry().valid(entity)) {
        return {.error = "No entity was given"};
    }
    root = prefabInstanceRoot(scene, entity);
    if (root == entt::null) {
        return {.error = "'" + scene.registry().get<NameComponent>(entity).name + "' is not part of a prefab instance"};
    }
    return {.success = true};
}

PrefabResult fromInstance(PrefabInstanceResult result) {
    return {.success = result.success, .error = std::move(result.error), .diagnostics = std::move(result.diagnostics)};
}

// Links the captured scene entities to the prefab just written at reference.
void linkCapture(Scene& scene, Entity root, const std::filesystem::path& reference, const PrefabCapture& capture) {
    entt::registry& registry = scene.registry();
    uint64_t nestedId = 0;
    if (const auto* existing = registry.try_get<PrefabInstanceComponent>(root);
        existing != nullptr && existing->prefab == reference.lexically_normal().generic_string()) {
        nestedId = existing->nestedId;
    }
    registry.emplace_or_replace<PrefabInstanceComponent>(
        root, PrefabInstanceComponent{.prefab = reference.lexically_normal().generic_string(), .nestedId = nestedId});
    for (const auto& [sceneId, localId] : capture.entityIds) {
        registry.emplace_or_replace<PrefabEntityComponent>(scene.findEntity(sceneId), localId);
    }
    for (const auto& [sceneId, recordId] : capture.nestedIds) {
        registry.get<PrefabInstanceComponent>(scene.findEntity(sceneId)).nestedId = recordId;
    }
}
} // namespace

bool PrefabEntityOverrides::overridden(std::string_view component, std::string_view field) const {
    if (component.empty()) {
        return (field == "/name" && name) || (field == "/local_transform" && localTransform) ||
               (field == "/parent" && parent);
    }
    const auto found = std::ranges::find(components, component, &PrefabComponentOverrides::component);
    if (found == components.end()) {
        return false;
    }
    return std::ranges::any_of(found->fields,
                               [field](const std::string& overridden) { return sameOrRelated(overridden, field); });
}

PrefabAuthoring::PrefabAuthoring(AssetManager& assets, const SceneDocumentOptions& options)
    : m_prefabs(assets, options) {}

PrefabResult PrefabAuthoring::create(Scene& scene, Entity root, const std::filesystem::path& reference) {
    if (root == entt::null || !scene.registry().valid(root)) {
        return {.error = "Select the entity to make a prefab from"};
    }
    // An entity inside another instance belongs to that instance's prefab;
    // linking it to a second prefab would leave both links inconsistent.
    if (const Entity enclosing = prefabInstanceRoot(scene, root); enclosing != entt::null && enclosing != root) {
        return {.error = "'" + scene.registry().get<NameComponent>(root).name +
                         "' is part of a prefab instance; unpack the instance before making a prefab from it"};
    }
    if (reference.extension() != ".prefab") {
        return {.error = "Prefab files must end in .prefab: " + reference.generic_string()};
    }
    std::filesystem::path path;
    if (PrefabResult resolved = m_prefabs.resolve(reference, path); !resolved) {
        return resolved;
    }
    if (std::filesystem::exists(path)) {
        return {.error = "A file already exists at " + reference.generic_string()};
    }
    PrefabCapture capture;
    PrefabResult captured = m_prefabs.capture(scene, root, reference, capture);
    if (!captured) {
        return captured;
    }
    if (PrefabResult saved = savePrefab(path, capture.document); !saved) {
        return saved;
    }
    linkCapture(scene, root, reference, capture);
    return captured;
}

PrefabResult PrefabAuthoring::instantiate(Scene& scene, const std::filesystem::path& reference, Entity parent,
                                          Entity& root) {
    PrefabInstanceResult result = m_prefabs.instantiate(reference, scene, parent);
    root = result.root;
    return fromInstance(std::move(result));
}

PrefabResult PrefabAuthoring::apply(Scene& scene, Entity entity) {
    Entity root = entt::null;
    if (PrefabResult found = instanceRootOf(scene, entity, root); !found) {
        return found;
    }
    const std::filesystem::path reference = scene.registry().get<PrefabInstanceComponent>(root).prefab;
    PrefabDocument previous;
    if (PrefabResult loaded = m_prefabs.load(reference, previous); !loaded) {
        return loaded;
    }
    // Other instances keep what they override relative to the content being
    // replaced, so their overrides are taken before the file changes.
    std::vector<std::pair<SceneEntityId, std::vector<PrefabOverride>>> others;
    for (const Entity other : prefabInstancesOf(scene, reference)) {
        if (other == root) {
            continue;
        }
        std::vector<PrefabOverride> overrides;
        if (PrefabResult compared = m_prefabs.overrides(scene, other, previous, overrides); !compared) {
            return {.error = "Instance '" + scene.registry().get<NameComponent>(other).name + "': " + compared.error};
        }
        others.emplace_back(scene.id(other), std::move(overrides));
    }

    PrefabCapture capture;
    PrefabResult result = m_prefabs.capture(scene, root, reference, capture);
    if (!result) {
        return result;
    }
    std::filesystem::path path;
    if (PrefabResult resolved = m_prefabs.resolve(reference, path); !resolved) {
        return resolved;
    }
    if (PrefabResult saved = savePrefab(path, capture.document); !saved) {
        return saved;
    }
    linkCapture(scene, root, reference, capture);

    for (const auto& [id, overrides] : others) {
        // An earlier refresh may have removed this instance (it was added to
        // another instance of the same prefab).
        const Entity other = scene.findEntity(id);
        if (other == entt::null || !scene.registry().all_of<PrefabInstanceComponent>(other)) {
            continue;
        }
        PrefabInstanceResult refreshed =
            m_prefabs.refresh(scene, other, overrides, PrefabRefreshMode::KeepAddedObjects);
        if (!refreshed) {
            result.diagnostics.push_back("Could not update instance " + std::to_string(id.value) + ": " +
                                         refreshed.error);
        }
        result.diagnostics.insert(result.diagnostics.end(), refreshed.diagnostics.begin(), refreshed.diagnostics.end());
    }
    return result;
}

PrefabResult PrefabAuthoring::revert(Scene& scene, Entity entity) {
    Entity root = entt::null;
    if (PrefabResult found = instanceRootOf(scene, entity, root); !found) {
        return found;
    }
    return fromInstance(m_prefabs.refresh(scene, root, {}, PrefabRefreshMode::RemoveAddedObjects));
}

PrefabResult PrefabAuthoring::revertField(Scene& scene, Entity entity, std::string_view component,
                                          std::string_view field) {
    Entity root = entt::null;
    if (PrefabResult found = instanceRootOf(scene, entity, root); !found) {
        return found;
    }
    const auto* marker = scene.registry().try_get<PrefabEntityComponent>(entity);
    if (marker == nullptr) {
        return {.error = "The entity was added to the prefab instance, so it has no prefab values"};
    }
    std::vector<PrefabOverride> overrides;
    if (PrefabResult compared = m_prefabs.overrides(scene, root, overrides); !compared) {
        return compared;
    }
    const PrefabEntityId local = marker->entity;
    const size_t removed = std::erase_if(overrides, [&](const PrefabOverride& value) {
        return value.entity == local && value.component == component &&
               (component.empty() ? value.field == field : sameOrRelated(value.field, field));
    });
    if (removed == 0) {
        return {.error = "That value is not overridden"};
    }
    return fromInstance(m_prefabs.refresh(scene, root, overrides, PrefabRefreshMode::KeepAddedObjects));
}

PrefabResult PrefabAuthoring::unpack(Scene& scene, Entity entity) {
    Entity root = entt::null;
    if (PrefabResult found = instanceRootOf(scene, entity, root); !found) {
        return found;
    }
    unpackPrefabInstance(scene, root);
    return {.success = true};
}

PrefabResult PrefabAuthoring::overrides(const Scene& scene, Entity entity, PrefabEntityOverrides& output) const {
    output = {};
    if (entity == entt::null || !scene.registry().valid(entity)) {
        return {.error = "No entity was given"};
    }
    const Entity root = prefabInstanceRoot(scene, entity);
    if (root == entt::null) {
        return {.success = true};
    }
    output.linked = true;
    output.instanceRoot = scene.id(root);
    output.prefab = scene.registry().get<PrefabInstanceComponent>(root).prefab;
    const auto* marker = scene.registry().try_get<PrefabEntityComponent>(entity);
    if (marker == nullptr) {
        output.added = true;
        return {.success = true};
    }
    PrefabDocument source;
    if (PrefabResult loaded = m_prefabs.load(output.prefab, source); !loaded) {
        return loaded;
    }
    const auto prefabEntity = std::ranges::find(source.entities, marker->entity, &PrefabEntity::id);
    if (prefabEntity == source.entities.end()) {
        // The prefab no longer has the entity; the next refresh removes it.
        output.added = true;
        return {.success = true};
    }
    std::vector<PrefabOverride> all;
    if (PrefabResult compared = m_prefabs.overrides(scene, root, source, all); !compared) {
        return compared;
    }
    output.instanceOverrides = all.size();
    for (const PrefabOverride& value : all) {
        if (value.component.empty() && value.field == "/removed") {
            ++output.removedEntities;
        }
        if (value.entity != marker->entity) {
            continue;
        }
        if (value.component.empty()) {
            output.name |= value.field == "/name";
            output.localTransform |= value.field == "/local_transform";
            output.parent |= value.field == "/parent";
            continue;
        }
        auto found = std::ranges::find(output.components, value.component, &PrefabComponentOverrides::component);
        if (found == output.components.end()) {
            output.components.push_back({.component = value.component});
            found = std::prev(output.components.end());
        }
        found->fields.push_back(value.field);
        if (value.field.empty()) {
            found->removed = value.jsonValue == "null";
            found->added = !found->removed && std::ranges::find(prefabEntity->components, value.component,
                                                                &std::pair<std::string, std::string>::first) ==
                                                  prefabEntity->components.end();
        }
    }
    // Overrides arrive sorted by component, then field.
    return {.success = true};
}

std::filesystem::path PrefabAuthoring::suggestPath(const Scene& scene, Entity entity) const {
    std::string stem = "Prefab";
    if (entity != entt::null && scene.registry().valid(entity)) {
        // Keep the name a portable file name; prefab references are stored as
        // generic narrow strings, so other characters become underscores.
        std::string name;
        for (const char character : scene.registry().get<NameComponent>(entity).name) {
            const auto code = static_cast<unsigned char>(character);
            const bool reserved = std::string_view("<>:\"/\\|?*").find(character) != std::string_view::npos;
            name += reserved || code < 0x20 || code >= 0x7F ? '_' : character;
        }
        while (!name.empty() && (name.back() == ' ' || name.back() == '.')) {
            name.pop_back();
        }
        if (!name.empty()) {
            stem = std::move(name);
        }
    }
    for (int number = 1;; ++number) {
        const std::string suffix = number == 1 ? std::string() : " " + std::to_string(number);
        std::filesystem::path reference = std::filesystem::path("prefabs") / (stem + suffix + ".prefab");
        std::filesystem::path path;
        std::error_code error;
        if (!m_prefabs.resolve(reference, path) || !std::filesystem::exists(path, error)) {
            return reference;
        }
    }
}

PrefabResult PrefabAuthoring::resolve(const std::filesystem::path& reference, std::filesystem::path& path) const {
    return m_prefabs.resolve(reference, path);
}

const PrefabEntityOverrides& PrefabOverrideCache::get(const PrefabAuthoring& authoring, const Scene& scene,
                                                      SceneEntityId entityId, uint64_t revision) {
    const Entity entity = scene.findEntity(entityId);
    Key key{.entity = entityId, .revision = revision, .sceneGeneration = scene.contentGeneration()};
    if (const Entity root = entity == entt::null ? entt::null : prefabInstanceRoot(scene, entity); root != entt::null) {
        key.prefab = scene.registry().get<PrefabInstanceComponent>(root).prefab;
        std::filesystem::path path;
        std::error_code error;
        if (authoring.resolve(key.prefab, path)) {
            key.prefabWriteTime = std::filesystem::last_write_time(path, error);
        }
    }
    if (m_key == key) {
        return m_overrides;
    }
    m_key = key;
    m_error.clear();
    if (entity == entt::null) {
        m_overrides = {};
        return m_overrides;
    }
    if (PrefabResult result = authoring.overrides(scene, entity, m_overrides); !result) {
        m_error = std::move(result.error);
        // Keep the link, so the instance header still offers its actions.
        const bool linked = m_overrides.linked;
        m_overrides = {};
        m_overrides.linked = linked || !key.prefab.empty();
        m_overrides.prefab = key.prefab;
        if (m_overrides.linked) {
            m_overrides.instanceRoot = scene.id(prefabInstanceRoot(scene, entity));
        }
    }
    return m_overrides;
}

void PrefabOverrideCache::clear() {
    m_key.reset();
    m_overrides = {};
    m_error.clear();
}
