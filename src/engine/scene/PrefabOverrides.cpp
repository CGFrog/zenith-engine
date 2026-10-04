// Prefab overrides: comparing instances with their prefab through staging
// scenes, applying overrides when rebuilding, and recording the structural
// edits (removals and moves) made to instances in the scene.
#include "PrefabSystemInternal.hpp"

#include <set>
#include <tuple>

using namespace prefab_detail;

namespace prefab_detail {
// Serializes entities, with their scene ids, exactly as SceneDocument writes
// them, by copying them into a staging scene. Parents outside the set are
// written as null. Scene-local components are left out: the staging scene
// defines no collections for them to name. Keyed by scene id.
PrefabResult serializeEntities(const Scene& scene, std::span<const Entity> entities, const AssetManager& assets,
                               const SceneDocumentOptions& options, std::map<uint64_t, Json>& output) {
    Scene staging;
    std::unordered_map<uint64_t, Entity> staged;
    const ComponentTypeRegistry& types = componentTypesFor(options);
    for (const Entity entity : entities) {
        Entity copy = entt::null;
        if (SceneResult created =
                staging.createEntityWithId(scene.id(entity), scene.registry().get<NameComponent>(entity).name, copy);
            !created) {
            return {.error = created.error};
        }
        staging.registry().get<TransformComponent>(copy).local = scene.registry().get<TransformComponent>(entity).local;
        for (const ComponentType& type : types.types()) {
            if (isSceneLocal(type.key())) {
                continue;
            }
            if (const void* component = type.find(scene, entity)) {
                type.addDefault(staging, copy);
                type.assign(type.find(staging, copy), type.copy(component));
            }
        }
        if (const auto* preserved = scene.registry().try_get<PreservedComponentsComponent>(entity)) {
            staging.registry().emplace<PreservedComponentsComponent>(copy, *preserved);
        }
        staged.emplace(scene.id(entity).value, copy);
    }
    for (const Entity entity : entities) {
        const Entity parent = parentOf(scene, entity);
        if (parent == entt::null) {
            continue;
        }
        if (const auto found = staged.find(scene.id(parent).value); found != staged.end()) {
            staging.setParent(staged.at(scene.id(entity).value), found->second);
        }
    }
    std::string serialized;
    if (SceneDocumentResult result = SceneDocument::serialize(staging, assets, options, serialized); !result) {
        return fromScene(result);
    }
    Json document = Json::parse(serialized);
    for (Json& entity : document["entities"]) {
        const uint64_t id = entity.at("id").get<uint64_t>();
        output[id] = std::move(entity);
    }
    return {.success = true};
}
} // namespace prefab_detail

namespace {
// The document's entities in their canonical serialized form, keyed by
// prefab-local id, so they compare equal to instance entities serialized the
// same way.
PrefabResult normalizedEntities(const PrefabDocument& document, AssetManager& assets,
                                const SceneDocumentOptions& options, std::map<uint64_t, Json>& output) {
    SceneChanges changes;
    for (const PrefabEntity& entity : document.entities) {
        Json json;
        if (PrefabResult result = prefabEntityToJson(entity, json); !result) {
            return result;
        }
        // Older prefabs may hold collection ids no staging scene defines.
        stripNonContentComponents(json);
        changes.entities.push_back({.id = {entity.id.value}, .after = json.dump()});
    }
    Scene staging;
    if (SceneDocumentResult result =
            SceneDocument::applyChanges(changes, SceneChangeDirection::Forward, assets, options, staging);
        !result) {
        return fromScene(result);
    }
    std::vector<Entity> entities;
    for (const PrefabEntity& entity : document.entities) {
        entities.push_back(staging.findEntity({entity.id.value}));
    }
    if (PrefabResult result = serializeEntities(staging, entities, assets, options, output); !result) {
        return result;
    }
    for (auto& [id, json] : output) {
        stripNonContentComponents(json);
    }
    return {.success = true};
}

// The mapped instance members' document objects (or only entity's, when
// given), keyed by prefab-local id, with prefab-local ids and only prefab
// content. A parent added in the scene is written as its scene id with
// sceneReferenceTag; a parent outside the instance (the root's) as null.
PrefabResult instanceLocalEntities(const Scene& scene, Entity root, const AssetManager& assets,
                                   const SceneDocumentOptions& options, std::map<uint64_t, Json>& output,
                                   Entity only = entt::null) {
    const InstanceIds ids = instanceIds(scene, root);
    std::vector<Entity> wanted = ids.mapped;
    if (only != entt::null) {
        wanted.clear();
        if (std::ranges::find(ids.mapped, only) != ids.mapped.end()) {
            wanted.push_back(only);
        }
    }
    std::map<uint64_t, Json> serialized;
    if (PrefabResult result = serializeEntities(scene, wanted, assets, options, serialized); !result) {
        return result;
    }
    for (auto& [sceneId, json] : serialized) {
        const Entity parent = parentOf(scene, scene.findEntity({sceneId}));
        Json parentValue = nullptr;
        if (parent != entt::null) {
            const uint64_t parentId = scene.id(parent).value;
            if (const auto found = ids.sceneToLocal.find(parentId);
                found != ids.sceneToLocal.end() && !isNestedReference(found->second)) {
                parentValue = found->second;
            } else if (ids.added.contains(parentId)) {
                parentValue = parentId | sceneReferenceTag;
            }
        }
        remapIds(json, ids.sceneToLocal, componentTypesFor(options), UnmappedReferences::TagScene);
        json["parent"] = std::move(parentValue);
        stripNonContentComponents(json);
        output[ids.sceneToLocal.at(sceneId)] = std::move(json);
    }
    return {.success = true};
}

std::string escapePointerToken(std::string_view key) {
    std::string escaped;
    for (const char character : key) {
        if (character == '~') {
            escaped += "~0";
        } else if (character == '/') {
            escaped += "~1";
        } else {
            escaped += character;
        }
    }
    return escaped;
}

// Whether every key of source is also a key of instance.
bool keepsKeys(const Json& source, const Json& instance) {
    for (auto item = source.begin(); item != source.end(); ++item) {
        if (!instance.contains(item.key())) {
            return false;
        }
    }
    return true;
}

// Pointers (relative to the component) and instance values of the leaves that
// differ. Objects are compared member by member, and members only the instance
// has are leaves of their own; anything else (arrays such as colors, objects
// missing a member the source has) is one value.
void diffValue(const Json& source, const Json& instance, const std::string& pointer,
               std::vector<std::pair<std::string, const Json*>>& output) {
    if (source == instance) {
        return;
    }
    if (source.is_object() && instance.is_object() && keepsKeys(source, instance)) {
        for (auto item = instance.begin(); item != instance.end(); ++item) {
            const std::string member = pointer + "/" + escapePointerToken(item.key());
            if (const auto inSource = source.find(item.key()); inSource != source.end()) {
                diffValue(*inSource, item.value(), member, output);
            } else {
                output.emplace_back(member, &item.value());
            }
        }
        return;
    }
    output.emplace_back(pointer, &instance);
}

void diffEntity(PrefabEntityId id, const Json& source, const Json& instance, bool isRoot,
                std::vector<PrefabOverride>& output) {
    if (source.at("name") != instance.at("name")) {
        output.push_back({.entity = id, .field = "/name", .jsonValue = instance.at("name").dump()});
    }
    // The root's transform places the instance; it is never an override.
    if (!isRoot && source.at("local_transform") != instance.at("local_transform")) {
        output.push_back(
            {.entity = id, .field = "/local_transform", .jsonValue = instance.at("local_transform").dump()});
    }
    // Moved under another entity of the instance. A parent that is not a
    // prefab entity (null here) cannot be named, so it is not an override.
    if (!isRoot && instance.at("parent").is_number_unsigned() && source.at("parent") != instance.at("parent")) {
        output.push_back({.entity = id, .field = "/parent", .jsonValue = instance.at("parent").dump()});
    }
    const Json& sourceComponents = source.at("components");
    const Json& instanceComponents = instance.at("components");
    std::set<std::string> keys;
    for (auto item = sourceComponents.begin(); item != sourceComponents.end(); ++item) {
        keys.insert(item.key());
    }
    for (auto item = instanceComponents.begin(); item != instanceComponents.end(); ++item) {
        keys.insert(item.key());
    }
    for (const std::string& key : keys) {
        const auto inSource = sourceComponents.find(key);
        const auto inInstance = instanceComponents.find(key);
        if (inInstance == instanceComponents.end()) {
            output.push_back({.entity = id, .component = key, .jsonValue = "null"});
            continue;
        }
        if (inSource == sourceComponents.end()) {
            output.push_back({.entity = id, .component = key, .jsonValue = inInstance->dump()});
            continue;
        }
        std::vector<std::pair<std::string, const Json*>> fields;
        diffValue(*inSource, *inInstance, "", fields);
        for (const auto& [pointer, value] : fields) {
            output.push_back({.entity = id, .component = key, .field = pointer, .jsonValue = value->dump()});
        }
    }
}

std::string describe(const PrefabOverride& value) {
    return "entity " + std::to_string(value.entity.value) + " " +
           (value.component.empty() ? std::string() : value.component) + value.field;
}

// Applies one value override to a prefab-local entity object. Structural
// overrides (parent, removed) are applied by applyOverrides.
void applyOverride(Json& target, const PrefabOverride& overrideValue, std::vector<std::string>& diagnostics) {
    Json value = Json::parse(overrideValue.jsonValue, nullptr, false);
    if (value.is_discarded()) {
        diagnostics.push_back("Invalid JSON in override for " + describe(overrideValue));
        return;
    }
    if (overrideValue.component.empty()) {
        if ((overrideValue.field == "/name" && value.is_string()) ||
            (overrideValue.field == "/local_transform" && value.is_array() && value.size() == 16)) {
            target[overrideValue.field.substr(1)] = std::move(value);
        } else {
            diagnostics.push_back("Invalid override for " + describe(overrideValue));
        }
        return;
    }
    Json& components = target["components"];
    if (overrideValue.field.empty()) {
        if (value.is_null()) {
            components.erase(overrideValue.component);
        } else {
            components[overrideValue.component] = std::move(value);
        }
        return;
    }
    const auto component = components.find(overrideValue.component);
    const Json::json_pointer pointer(overrideValue.field);
    if (component == components.end() || !component->contains(pointer.parent_pointer())) {
        diagnostics.push_back("Orphaned override for " + describe(overrideValue) +
                              ": the prefab no longer has that component value");
        return;
    }
    (*component)[pointer] = std::move(value);
}

// Whether making parent the parent of entity would put entity above itself.
bool makesCycle(const std::map<uint64_t, Json>& entities, uint64_t entity, uint64_t parent) {
    for (uint64_t current = parent;;) {
        if (current == entity) {
            return true;
        }
        const auto found = entities.find(current);
        if (found == entities.end() || !found->second.at("parent").is_number_unsigned()) {
            return false;
        }
        current = found->second.at("parent").get<uint64_t>();
    }
}

void applyParentOverride(std::map<uint64_t, Json>& entities, const PrefabOverride& overrideValue, uint64_t rootLocal,
                         std::vector<std::string>& diagnostics) {
    const Json value = Json::parse(overrideValue.jsonValue, nullptr, false);
    const uint64_t entity = overrideValue.entity.value;
    if (entity == rootLocal || !value.is_number_unsigned()) {
        diagnostics.push_back("Invalid override for " + describe(overrideValue));
        return;
    }
    const uint64_t parent = value.get<uint64_t>();
    if (isSceneReference(parent)) {
        // Under an entity added in the scene; the rebuild checks it still is.
        entities.at(entity)["parent"] = parent;
        return;
    }
    if (!entities.contains(parent)) {
        diagnostics.push_back("Orphaned override for " + describe(overrideValue) +
                              ": the prefab no longer has the new parent");
        return;
    }
    if (makesCycle(entities, entity, parent)) {
        diagnostics.push_back("Skipped override for " + describe(overrideValue) +
                              ": the prefab now holds the entity above its new parent");
        return;
    }
    entities.at(entity)["parent"] = parent;
}

// Removes the entities named by removed overrides, with their descendants.
void applyRemovals(std::map<uint64_t, Json>& entities, std::span<const PrefabOverride> overrides, uint64_t rootLocal,
                   std::vector<std::string>& diagnostics) {
    std::unordered_set<uint64_t> removed;
    for (const PrefabOverride& overrideValue : overrides) {
        if (!isRemovedOverride(overrideValue) || overrideValue.nested != 0 ||
            !entities.contains(overrideValue.entity.value)) {
            continue;
        }
        if (overrideValue.entity.value == rootLocal || overrideValue.jsonValue != "true") {
            diagnostics.push_back("Invalid override for " + describe(overrideValue));
            continue;
        }
        removed.insert(overrideValue.entity.value);
    }
    if (removed.empty()) {
        return;
    }
    const auto underRemoved = [&](uint64_t local) {
        for (uint64_t current = local;;) {
            if (removed.contains(current)) {
                return true;
            }
            const Json& parent = entities.at(current).at("parent");
            if (!parent.is_number_unsigned() || !entities.contains(parent.get<uint64_t>())) {
                return false;
            }
            current = parent.get<uint64_t>();
        }
    };
    std::vector<uint64_t> doomed;
    for (const auto& [local, json] : entities) {
        if (underRemoved(local)) {
            doomed.push_back(local);
        }
    }
    for (const uint64_t local : doomed) {
        entities.erase(local);
    }
}
} // namespace

namespace prefab_detail {
// Applies overrides to prefab-local entity objects: values first, then
// parents, then removals (which take the removed entities' descendants
// along). Overrides whose entity, component, or parent value no longer exists
// are reported and skipped, except removals of entities the prefab no longer
// has, which already hold. Scene-local components are never overrides, and
// nested record removals are applied by the rebuild.
void applyOverrides(std::map<uint64_t, Json>& entities, std::span<const PrefabOverride> overrides, uint64_t rootLocal,
                    std::vector<std::string>& diagnostics) {
    // Values, then moves under entities added in the scene (which take
    // entities out of the prefab hierarchy, so the cycle checks of the other
    // moves see that), then moves under prefab entities.
    const auto pass = [](const PrefabOverride& value) {
        if (!isParentOverride(value)) {
            return 0;
        }
        const Json parent = Json::parse(value.jsonValue, nullptr, false);
        return parent.is_number_unsigned() && isSceneReference(parent.get<uint64_t>()) ? 1 : 2;
    };
    for (const int current : {0, 1, 2}) {
        for (const PrefabOverride& overrideValue : overrides) {
            if (isSceneLocal(overrideValue.component) || isRemovedOverride(overrideValue) ||
                overrideValue.nested != 0 || pass(overrideValue) != current) {
                continue;
            }
            const auto entity = entities.find(overrideValue.entity.value);
            if (entity == entities.end()) {
                diagnostics.push_back("Orphaned override for " + describe(overrideValue) +
                                      ": the prefab no longer has that entity");
                continue;
            }
            try {
                if (current == 0) {
                    applyOverride(entity->second, overrideValue, diagnostics);
                } else {
                    applyParentOverride(entities, overrideValue, rootLocal, diagnostics);
                }
            } catch (const Json::exception& error) {
                diagnostics.push_back("Invalid override for " + describe(overrideValue) + ": " + error.what());
            }
        }
    }
    applyRemovals(entities, overrides, rootLocal, diagnostics);
}

// Settles the entities a /parent override moved under an entity added in the
// scene (a parent tagged with sceneReferenceTag): they stay there while that
// entity stays (stays holds the scene ids that do), and otherwise go back
// under their prefab parent; entities whose parent is no longer there go with
// it. Returns whether anything changed.
bool resolveSceneParents(std::map<uint64_t, Json>& entities, const PrefabDocument& document,
                         const std::unordered_set<uint64_t>& stays, std::vector<std::string>& diagnostics) {
    bool changed = false;
    for (auto& [local, json] : entities) {
        const Json& parent = json.at("parent");
        if (!parent.is_number_unsigned() || !isSceneReference(parent.get<uint64_t>()) ||
            stays.contains(parent.get<uint64_t>() & ~sceneReferenceTag)) {
            continue;
        }
        const auto prefabEntity = std::ranges::find(document.entities, PrefabEntityId{local}, &PrefabEntity::id);
        uint64_t prefabParent =
            prefabEntity != document.entities.end() && prefabEntity->parent ? prefabEntity->parent->value : uint64_t{0};
        // Other parent overrides may have put the prefab parent below the
        // entity; the root is never below it.
        if (prefabParent != 0 && makesCycle(entities, local, prefabParent)) {
            prefabParent = rootId(document).value_or(0);
        }
        json["parent"] = prefabParent;
        diagnostics.push_back("'" + json.value("name", std::string{}) +
                              "' was moved under an object that is no longer in the instance; it is back under "
                              "its prefab parent");
        changed = true;
    }
    for (bool removed = true; removed;) {
        removed = false;
        for (auto entity = entities.begin(); entity != entities.end();) {
            const Json& parent = entity->second.at("parent");
            if (parent.is_number_unsigned() && !isSceneReference(parent.get<uint64_t>()) &&
                !entities.contains(parent.get<uint64_t>())) {
                entity = entities.erase(entity);
                removed = true;
                changed = true;
            } else {
                ++entity;
            }
        }
    }
    return changed;
}
} // namespace prefab_detail

namespace {
// The instance whose prefab entity or nested record entity stands for: its
// own instance for a member, the containing instance for an instance root.
Entity owningInstance(const Scene& scene, Entity entity) {
    if (!scene.registry().all_of<PrefabInstanceComponent>(entity)) {
        return prefabInstanceRoot(scene, entity);
    }
    const Entity parent = parentOf(scene, entity);
    return parent == entt::null ? entt::null : prefabInstanceRoot(scene, parent);
}

void addOverride(Scene& scene, Entity root, const PrefabOverride& value) {
    auto& instance = scene.registry().get<PrefabInstanceComponent>(root);
    if (std::ranges::find(instance.overrides, value) == instance.overrides.end()) {
        instance.overrides.push_back(value);
    }
}

// The removal override for entity of the instance at root: its prefab entity
// or, for a nested instance root, its nested record. Nullopt when entity was
// added in the scene, including copies claiming an id another entity holds.
std::optional<PrefabOverride> removalOf(const Scene& scene, Entity root, Entity entity) {
    if (scene.registry().all_of<PrefabInstanceComponent>(entity)) {
        const uint64_t record = scene.registry().get<PrefabInstanceComponent>(entity).nestedId;
        if (record == 0) {
            return std::nullopt;
        }
        std::vector<Entity> nestedRoots;
        prefabInstanceMembers(scene, root, &nestedRoots);
        const auto first = std::ranges::find_if(nestedRoots, [&](Entity nested) {
            return scene.registry().get<PrefabInstanceComponent>(nested).nestedId == record;
        });
        if (first == nestedRoots.end() || *first != entity) {
            return std::nullopt;
        }
        return PrefabOverride{.field = "/removed", .jsonValue = "true", .nested = record};
    }
    for (const auto& [member, local] : mappedMembers(scene, root)) {
        if (member == entity) {
            return PrefabOverride{.entity = local, .field = "/removed", .jsonValue = "true"};
        }
    }
    return std::nullopt;
}
} // namespace

bool recordPrefabEntityRemoval(Scene& scene, Entity entity) {
    if (entity == entt::null || !scene.registry().valid(entity)) {
        return false;
    }
    const Entity root = owningInstance(scene, entity);
    if (root == entt::null || root == entity) {
        return false;
    }
    const std::optional<PrefabOverride> removal = removalOf(scene, root, entity);
    if (!removal) {
        return false;
    }
    addOverride(scene, root, *removal);
    return true;
}

bool recordPrefabEntityMove(Scene& scene, Entity entity, Entity newParent) {
    if (entity == entt::null || !scene.registry().valid(entity)) {
        return false;
    }
    const Entity root = owningInstance(scene, entity);
    if (root == entt::null || root == entity ||
        (newParent != entt::null && prefabInstanceRoot(scene, newParent) == root)) {
        return false;
    }
    // The moved part of the instance: its members under entity, and the
    // nested instances among them.
    std::vector<Entity> moved;
    std::vector<Entity> nested;
    for (std::vector<Entity> pending{entity}; !pending.empty();) {
        const Entity current = pending.back();
        pending.pop_back();
        if (scene.registry().all_of<PrefabInstanceComponent>(current)) {
            nested.push_back(current);
            continue;
        }
        moved.push_back(current);
        const auto& children = scene.registry().get<RelationshipComponent>(current).children;
        pending.insert(pending.end(), children.begin(), children.end());
    }
    // Only the topmost prefab entities need a removal; their descendants go
    // with them.
    std::unordered_set<Entity> mapped;
    for (const auto& [member, local] : mappedMembers(scene, root)) {
        mapped.insert(member);
    }
    const auto recorded = [&](Entity candidate) {
        return candidate == entity || !mapped.contains(parentOf(scene, candidate));
    };
    bool changed = false;
    for (const Entity member : moved) {
        if (recorded(member)) {
            if (const std::optional<PrefabOverride> removal = removalOf(scene, root, member)) {
                addOverride(scene, root, *removal);
            }
        }
    }
    for (const Entity nestedRoot : nested) {
        if (recorded(nestedRoot)) {
            if (const std::optional<PrefabOverride> removal = removalOf(scene, root, nestedRoot)) {
                addOverride(scene, root, *removal);
            }
        }
    }
    for (const Entity member : moved) {
        changed |= scene.registry().remove<PrefabEntityComponent>(member) > 0;
    }
    for (const Entity nestedRoot : nested) {
        auto& instance = scene.registry().get<PrefabInstanceComponent>(nestedRoot);
        changed |= instance.nestedId != 0;
        instance.nestedId = 0;
    }
    return changed;
}

PrefabResult PrefabSystem::overrides(const Scene& scene, Entity root, std::vector<PrefabOverride>& output) const {
    const auto* instance = scene.registry().try_get<PrefabInstanceComponent>(root);
    if (instance == nullptr) {
        return {.error = "The entity is not a prefab instance root"};
    }
    PrefabDocument source;
    if (PrefabResult loaded = load(instance->prefab, source); !loaded) {
        return loaded;
    }
    return overrides(scene, root, source, output);
}

namespace {
void sortOverrides(std::vector<PrefabOverride>& overrides) {
    std::ranges::sort(overrides, {}, [](const PrefabOverride& value) {
        return std::tie(value.nested, value.entity.value, value.component, value.field);
    });
}

// The recorded removals that still hold: prefab entities and nested records
// the source has and the instance lacks. A prefab entity the instance lacks
// was deleted in the scene only when the deletion was recorded; otherwise the
// prefab gained it since the instance was last refreshed, which is not an
// override.
void recordedRemovals(const Scene& scene, Entity root, const PrefabDocument& source,
                      const std::map<uint64_t, Json>& instanceEntities, std::vector<PrefabOverride>& found) {
    const auto* instance = scene.registry().try_get<PrefabInstanceComponent>(root);
    if (instance == nullptr) {
        return;
    }
    std::vector<Entity> nestedRoots;
    prefabInstanceMembers(scene, root, &nestedRoots);
    const auto hasNested = [&](uint64_t record) {
        return std::ranges::any_of(nestedRoots, [&](Entity nested) {
            return scene.registry().get<PrefabInstanceComponent>(nested).nestedId == record;
        });
    };
    for (const PrefabOverride& value : instance->overrides) {
        if (!isRemovedOverride(value) || std::ranges::find(found, value) != found.end()) {
            continue;
        }
        const bool holds =
            value.nested != 0
                ? std::ranges::any_of(source.nested,
                                      [&](const NestedPrefab& record) { return record.id == value.nested; }) &&
                      !hasNested(value.nested)
                : std::ranges::any_of(source.entities,
                                      [&](const PrefabEntity& entity) { return entity.id == value.entity; }) &&
                      !instanceEntities.contains(value.entity.value);
        if (holds) {
            found.push_back(value);
        }
    }
}

// Compares the instance's mapped members (or only entity's) with the source
// entities in canonical form.
PrefabResult compareInstance(const Scene& scene, Entity root, const PrefabDocument& source,
                             const std::map<uint64_t, Json>& sourceEntities, Entity only, const AssetManager& assets,
                             const SceneDocumentOptions& options, std::vector<PrefabOverride>& output) {
    std::map<uint64_t, Json> instanceEntities;
    if (PrefabResult result = instanceLocalEntities(scene, root, assets, options, instanceEntities, only); !result) {
        return result;
    }
    const std::optional<uint64_t> rootLocal = rootId(source);
    std::vector<PrefabOverride> found;
    try {
        for (const auto& [local, instance] : instanceEntities) {
            if (const auto inSource = sourceEntities.find(local); inSource != sourceEntities.end()) {
                diffEntity({local}, inSource->second, instance, rootLocal == local, found);
            }
        }
    } catch (const Json::exception& error) {
        return {.error = std::string("Could not compare the instance with its prefab: ") + error.what()};
    }
    if (only == entt::null) {
        recordedRemovals(scene, root, source, instanceEntities, found);
    }
    sortOverrides(found);
    output = std::move(found);
    return {.success = true};
}
} // namespace

PrefabResult PrefabSystem::overrides(const Scene& scene, Entity root, const PrefabDocument& source,
                                     std::vector<PrefabOverride>& output) const {
    std::map<uint64_t, Json> sourceEntities;
    if (PrefabResult result = normalizedEntities(source, m_assets, m_options, sourceEntities); !result) {
        return result;
    }
    return compareInstance(scene, root, source, sourceEntities, entt::null, m_assets, m_options, output);
}

PrefabResult PrefabSystem::prepareComparison(const PrefabDocument& document, PrefabComparisonSource& output) const {
    std::map<uint64_t, Json> entities;
    if (PrefabResult result = normalizedEntities(document, m_assets, m_options, entities); !result) {
        return result;
    }
    PrefabComparisonSource prepared{.document = document};
    for (const auto& [local, json] : entities) {
        prepared.entities.emplace(local, json.dump());
    }
    output = std::move(prepared);
    return {.success = true};
}

PrefabResult PrefabSystem::overrides(const Scene& scene, Entity root, const PrefabComparisonSource& source,
                                     std::vector<PrefabOverride>& output) const {
    std::map<uint64_t, Json> sourceEntities;
    for (const auto& [local, text] : source.entities) {
        sourceEntities.emplace(local, Json::parse(text, nullptr, false));
    }
    return compareInstance(scene, root, source.document, sourceEntities, entt::null, m_assets, m_options, output);
}

PrefabResult PrefabSystem::entityOverrides(const Scene& scene, Entity root, Entity entity,
                                           const PrefabComparisonSource& source,
                                           std::vector<PrefabOverride>& output) const {
    output.clear();
    const auto* marker = scene.registry().try_get<PrefabEntityComponent>(entity);
    if (entity == entt::null || marker == nullptr) {
        return {.success = true};
    }
    const auto text = source.entities.find(marker->entity.value);
    if (text == source.entities.end()) {
        return {.success = true};
    }
    std::map<uint64_t, Json> sourceEntities;
    sourceEntities.emplace(text->first, Json::parse(text->second, nullptr, false));
    return compareInstance(scene, root, source.document, sourceEntities, entity, m_assets, m_options, output);
}
