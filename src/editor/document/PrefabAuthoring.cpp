#include "PrefabAuthoring.hpp"

#include <algorithm>
#include <span>
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

} // namespace

void linkPrefabCapture(Scene& scene, Entity root, const std::filesystem::path& reference,
                       const PrefabCapture& capture) {
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
    m_prefabs.invalidate(reference);
    linkPrefabCapture(scene, root, reference, capture);
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
    PrefabRefreshPlan plan;
    const Entity applied[]{root};
    if (PrefabResult planned = planRefresh(scene, reference, &previous, applied, plan); !planned) {
        return planned;
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
    // The refreshes below must build from the new content even when the
    // file's timestamp did not visibly change.
    m_prefabs.invalidate(reference);
    linkPrefabCapture(scene, root, reference, capture);

    PrefabResult refreshed = refresh(scene, plan);
    if (!refreshed) {
        return refreshed;
    }
    result.diagnostics.insert(result.diagnostics.end(), refreshed.diagnostics.begin(), refreshed.diagnostics.end());
    return result;
}

PrefabResult PrefabAuthoring::refreshInstances(Scene& scene, const std::filesystem::path& reference,
                                               const PrefabDocument* previous, std::span<const Entity> skip) {
    PrefabRefreshPlan plan;
    if (PrefabResult planned = planRefresh(scene, reference, previous, skip, plan); !planned) {
        return planned;
    }
    // The file changed outside this system, which may have cached it.
    m_prefabs.invalidate(reference);
    return refresh(scene, plan);
}

PrefabResult PrefabAuthoring::planRefresh(const Scene& scene, const std::filesystem::path& reference,
                                          const PrefabDocument* previous, std::span<const Entity> skip,
                                          PrefabRefreshPlan& output) const {
    PrefabRefreshPlan plan{.reference = reference};
    // The previous content is normalized once for all instances.
    std::optional<PrefabComparisonSource> previousSource;
    for (const Entity instance : prefabInstancesOf(scene, reference)) {
        if (std::ranges::find(skip, instance) != skip.end()) {
            continue;
        }
        std::vector<PrefabOverride> overrides;
        if (previous == nullptr) {
            overrides = scene.registry().get<PrefabInstanceComponent>(instance).overrides;
        } else {
            if (!previousSource) {
                previousSource.emplace();
                if (PrefabResult prepared = m_prefabs.prepareComparison(*previous, *previousSource); !prepared) {
                    return prepared;
                }
            }
            if (PrefabResult compared = m_prefabs.overrides(scene, instance, *previousSource, overrides); !compared) {
                return {.error =
                            "Instance '" + scene.registry().get<NameComponent>(instance).name + "': " + compared.error};
            }
        }
        plan.instances.emplace_back(scene.id(instance), std::move(overrides));
    }
    if (previous != nullptr) {
        plan.previousRecords.emplace();
        for (const NestedPrefab& record : previous->nested) {
            plan.previousRecords->push_back(record.id);
        }
    }
    output = std::move(plan);
    return {.success = true};
}

PrefabResult PrefabAuthoring::refresh(Scene& scene, const PrefabRefreshPlan& plan) {
    PrefabResult result{.success = true};
    if (plan.instances.empty()) {
        return result;
    }
    // Nested records the new content deleted leave the instances too; record
    // ids are never reused, so the removals cannot hit a later record.
    std::vector<PrefabOverride> deletedRecords;
    if (plan.previousRecords) {
        PrefabDocument current;
        if (PrefabResult loaded = m_prefabs.load(plan.reference, current); !loaded) {
            return loaded;
        }
        for (const uint64_t record : *plan.previousRecords) {
            if (std::ranges::none_of(current.nested,
                                     [record](const NestedPrefab& kept) { return kept.id == record; })) {
                deletedRecords.push_back({.field = "/removed", .jsonValue = "true", .nested = record});
            }
        }
    }

    for (const auto& [id, planned] : plan.instances) {
        // An earlier refresh may have removed this instance (it was added to
        // another instance of the same prefab).
        const Entity instance = scene.findEntity(id);
        if (instance == entt::null || !scene.registry().all_of<PrefabInstanceComponent>(instance)) {
            continue;
        }
        std::vector<PrefabOverride> overrides = planned;
        for (const PrefabOverride& removal : deletedRecords) {
            if (std::ranges::find(overrides, removal) == overrides.end()) {
                overrides.push_back(removal);
            }
        }
        PrefabInstanceResult refreshed =
            m_prefabs.refresh(scene, instance, overrides, PrefabRefreshMode::KeepAddedObjects);
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

namespace {
// The entity's link, before its overrides are known: linked, the instance,
// the prefab, and whether the entity was added in the scene. Returns the
// instance root, or null when entity is in no instance.
Entity describeLink(const Scene& scene, Entity entity, PrefabEntityOverrides& output) {
    output = {};
    const Entity root = prefabInstanceRoot(scene, entity);
    if (root == entt::null) {
        return root;
    }
    output.linked = true;
    output.instanceRoot = scene.id(root);
    output.prefab = scene.registry().get<PrefabInstanceComponent>(root).prefab;
    output.displayName = std::filesystem::path(output.prefab).stem().generic_string();
    output.added = !scene.registry().all_of<PrefabEntityComponent>(entity);
    return root;
}

// Fills output (already described by describeLink) from the instance's
// overrides against its prefab document.
void describeOverrides(const Scene& scene, Entity entity, const PrefabDocument& source,
                       std::span<const PrefabOverride> all, PrefabEntityOverrides& output) {
    if (!source.displayName.empty()) {
        output.displayName = source.displayName;
    }
    if (output.added) {
        return;
    }
    const PrefabEntityId local = scene.registry().get<PrefabEntityComponent>(entity).entity;
    const auto prefabEntity = std::ranges::find(source.entities, local, &PrefabEntity::id);
    if (prefabEntity == source.entities.end()) {
        // The prefab no longer has the entity; the next refresh removes it.
        output.added = true;
        return;
    }
    output.instanceOverrides = all.size();
    for (const PrefabOverride& value : all) {
        if (value.component.empty() && value.field == "/removed") {
            ++output.removedEntities;
        }
        if (value.nested != 0 || value.entity != local) {
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
}
} // namespace

PrefabResult PrefabAuthoring::overrides(const Scene& scene, Entity entity, PrefabEntityOverrides& output) const {
    output = {};
    if (entity == entt::null || !scene.registry().valid(entity)) {
        return {.error = "No entity was given"};
    }
    const Entity root = describeLink(scene, entity, output);
    if (root == entt::null) {
        return {.success = true};
    }
    PrefabDocument source;
    if (PrefabResult loaded = m_prefabs.load(output.prefab, source); !loaded) {
        return loaded;
    }
    std::vector<PrefabOverride> all;
    if (!output.added) {
        if (PrefabResult compared = m_prefabs.overrides(scene, root, source, all); !compared) {
            return compared;
        }
    }
    describeOverrides(scene, entity, source, all, output);
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

const std::string& PrefabDisplayNames::get(const std::filesystem::path& file) {
    std::error_code error;
    const std::filesystem::file_time_type writeTime = std::filesystem::last_write_time(file, error);
    const std::string key = file.lexically_normal().generic_string();
    auto found = m_entries.find(key);
    if (found != m_entries.end() && found->second.writeTime == writeTime) {
        return found->second.name;
    }
    Entry entry{.writeTime = writeTime, .name = file.stem().generic_string()};
    PrefabDocument document;
    if (loadPrefab(file, document) && !document.displayName.empty()) {
        entry.name = document.displayName;
    }
    return m_entries.insert_or_assign(key, std::move(entry)).first->second.name;
}

PrefabOverrideCache::PrefabOverrideCache(std::chrono::milliseconds settleDelay)
    : m_settleDelay(settleDelay) {}

const PrefabEntityOverrides& PrefabOverrideCache::get(const PrefabAuthoring& authoring, const Scene& scene,
                                                      SceneEntityId entityId, uint64_t revision) {
    const Entity entity = scene.findEntity(entityId);
    PrefabEntityOverrides link;
    const Entity root = entity == entt::null ? entt::null : describeLink(scene, entity, link);
    if (root == entt::null) {
        m_result.reset();
        m_error.clear();
        m_overrides = std::move(link);
        return m_overrides;
    }
    InstanceKey key{.root = link.instanceRoot,
                    .revision = revision,
                    .sceneGeneration = scene.contentGeneration(),
                    .prefab = link.prefab};
    std::filesystem::path path;
    std::error_code ignored;
    if (authoring.resolve(key.prefab, path)) {
        key.prefabWriteTime = std::filesystem::last_write_time(path, ignored);
    }
    const bool changed = update(authoring, scene, root, entity, link.added, key);
    if (!changed && m_result && m_result->first == entityId && m_result->second == key) {
        return m_overrides;
    }
    m_result.emplace(entityId, key);
    if (!m_error.empty()) {
        // Keep the link, so the instance header still offers its actions.
        m_overrides = std::move(link);
        return m_overrides;
    }
    describeOverrides(scene, entity, m_source.document, m_instanceOverrides, link);
    m_overrides = std::move(link);
    return m_overrides;
}

bool PrefabOverrideCache::update(const PrefabAuthoring& authoring, const Scene& scene, Entity root, Entity entity,
                                 bool added, const InstanceKey& key) {
    const PrefabSystem& prefabs = authoring.prefabs();
    // The prepared prefab is reused until its file changes.
    if (const SourceKey sourceKey{.prefab = key.prefab, .writeTime = key.prefabWriteTime}; m_sourceKey != sourceKey) {
        m_sourceKey = sourceKey;
        m_instanceKey.reset();
        m_sourceError.clear();
        PrefabDocument document;
        PrefabResult prepared = prefabs.load(key.prefab, document);
        if (prepared) {
            prepared = prefabs.prepareComparison(document, m_source);
        }
        if (!prepared) {
            m_sourceError = std::move(prepared.error);
        }
    }
    if (!m_sourceError.empty()) {
        const bool changed = m_error != m_sourceError;
        m_error = m_sourceError;
        return changed;
    }
    const bool sameInstance = m_instanceKey && m_error.empty() && m_instanceKey->root == key.root &&
                              m_instanceKey->sceneGeneration == key.sceneGeneration &&
                              m_instanceKey->prefab == key.prefab &&
                              m_instanceKey->prefabWriteTime == key.prefabWriteTime;
    const auto now = std::chrono::steady_clock::now();
    if (sameInstance && m_instanceKey->revision != key.revision) {
        // An edit (every frame of a drag): compare only the entity shown.
        if (!added) {
            std::vector<PrefabOverride> entityOverrides;
            if (PrefabResult compared = prefabs.entityOverrides(scene, root, entity, m_source, entityOverrides);
                !compared) {
                m_error = std::move(compared.error);
                m_instanceKey = key;
                return true;
            }
            const PrefabEntityId local = scene.registry().get<PrefabEntityComponent>(entity).entity;
            std::erase_if(m_instanceOverrides,
                          [local](const PrefabOverride& value) { return value.nested == 0 && value.entity == local; });
            m_instanceOverrides.insert(m_instanceOverrides.end(), entityOverrides.begin(), entityOverrides.end());
            sortPrefabOverrides(m_instanceOverrides);
        }
        m_instanceKey = key;
        m_partial = true;
        m_lastPartialUpdate = now;
        return true;
    }
    if (sameInstance && (!m_partial || now - m_lastPartialUpdate < m_settleDelay)) {
        return false;
    }
    // A new instance or prefab, or edits have settled: compare everything.
    m_instanceKey = key;
    m_partial = false;
    m_error.clear();
    if (PrefabResult compared = prefabs.overrides(scene, root, m_source, m_instanceOverrides); !compared) {
        m_instanceOverrides.clear();
        m_error = std::move(compared.error);
    }
    return true;
}

void PrefabOverrideCache::clear() {
    m_sourceKey.reset();
    m_source = {};
    m_sourceError.clear();
    m_instanceKey.reset();
    m_instanceOverrides.clear();
    m_partial = false;
    m_result.reset();
    m_overrides = {};
    m_error.clear();
}
