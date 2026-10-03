#include "Prefab.hpp"

#include "../assets/Assets.hpp"
#include "ComponentTypes.hpp"
#include "EntityReferenceRemap.hpp"

#include <glm/gtc/type_ptr.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

// Instances are compared with and rebuilt from their prefab through the scene
// document entity form: instance entities are serialized exactly as a scene
// save writes them, prefab entities are normalized by loading them into a
// staging scene and serializing them the same way, and instances are rebuilt
// with SceneDocument::applyChanges, which keeps stable ids and is atomic.

namespace {
using Json = nlohmann::json;
using IdMap = std::unordered_map<uint64_t, uint64_t>;

constexpr const char* instanceKey = "prefab_instance";
constexpr const char* entityKey = "prefab_entity";

// Built-in custom-codec fields naming the instance root of a model's node
// entities; a model whose nodes a prefab lists names its entity in one.
struct ModelRootField {
    const char* component;
    const char* field;
};

constexpr std::array<ModelRootField, 2> modelRootFields{{
    {"model_node", "instance_root"},
    {"skin", "instance_root"},
}};

// Components holding scene organization rather than prefab content: a prefab
// never stores them, an instance keeps its own through every refresh, and they
// are never overrides. Collection ids are per scene.
constexpr std::array<const char*, 2> sceneLocalKeys{"collection_membership", "collection_instance"};

bool isSceneLocal(std::string_view key) {
    return std::ranges::find(sceneLocalKeys, key) != sceneLocalKeys.end();
}

// Marks a scene id inside prefab-local data (an instance's override of a
// reference to an entity outside the instance), so it cannot be mistaken for
// a prefab-local id. Stable ids never reach this bit.
constexpr uint64_t sceneReferenceTag = uint64_t{1} << 63u;

// What becomes of an entity reference whose target the id map lacks.
enum class UnmappedReferences : uint8_t {
    // Scene to prefab-local, for a capture: cleared to none and reported,
    // because a prefab names only its own entities.
    Clear,
    // Scene to prefab-local, for comparing an instance with its prefab:
    // tagged with sceneReferenceTag, as it names an entity outside the
    // instance.
    TagScene,
    // Prefab-local to scene, for rebuilding an instance: tagged scene ids are
    // untagged, and other ids (prefab entities the instance removed) become
    // none where a reference may be none.
    Resolve,
};

// Rewrites the ids an entity document object holds (its id, its parent, and
// every entity reference its components hold, see remapEntityReferences)
// through map. Ids the map lacks keep their id and parent; references are
// handled as unmapped says.
void remapIds(Json& entity, const IdMap& map, const ComponentTypeRegistry& types, UnmappedReferences unmapped,
              std::vector<std::string>* diagnostics = nullptr) {
    const auto remap = [&map](Json& value) {
        if (!value.is_number_unsigned()) {
            return;
        }
        if (const auto found = map.find(value.get<uint64_t>()); found != map.end()) {
            value = found->second;
        }
    };
    remap(entity["id"]);
    remap(entity["parent"]);
    const std::string name = entity.value("name", std::string{});
    remapEntityReferences(
        entity["components"], types, [&](uint64_t id, const EntityReferenceSite& site) -> std::optional<uint64_t> {
            if (unmapped == UnmappedReferences::Resolve && (id & sceneReferenceTag) != 0) {
                return id & ~sceneReferenceTag;
            }
            if (const auto found = map.find(id); found != map.end()) {
                return found->second;
            }
            if (unmapped == UnmappedReferences::TagScene) {
                return id | sceneReferenceTag;
            }
            if (!site.nullable) {
                return std::nullopt;
            }
            if (unmapped == UnmappedReferences::Resolve) {
                return uint64_t{0};
            }
            if (diagnostics != nullptr) {
                diagnostics->push_back("'" + name + "' " + std::string(site.component) + std::string(site.field) +
                                       " referenced an entity outside the prefab; the prefab "
                                       "stores no reference there");
            }
            return uint64_t{0};
        });
}

// Removes what is not prefab content: the link components and the
// scene-local components.
void stripNonContentComponents(Json& entity) {
    Json& components = entity["components"];
    components.erase(instanceKey);
    components.erase(entityKey);
    for (const char* key : sceneLocalKeys) {
        components.erase(key);
    }
}

// The scene-local component objects entity holds now, keyed by component key.
PrefabResult sceneLocalComponents(const Scene& scene, Entity entity, const AssetManager& assets,
                                  const SceneDocumentOptions& options, Json& output) {
    output = Json::object();
    const ComponentTypeRegistry& types = componentTypesFor(options);
    for (const char* key : sceneLocalKeys) {
        const ComponentType* type = types.find(key);
        if (type == nullptr || !type->has(scene, entity)) {
            continue;
        }
        Json value = Json::object();
        if (SceneDocumentResult saved = type->save(scene, entity, {.assets = assets, .options = options}, value);
            !saved) {
            return {.error = saved.error};
        }
        output[key] = std::move(value);
    }
    return {.success = true};
}

std::string normalizedReference(const std::filesystem::path& reference) {
    return reference.lexically_normal().generic_string();
}

PrefabResult fromScene(const SceneDocumentResult& result) {
    return {.success = result.success, .error = result.error};
}

Json matrixJson(const glm::mat4& value) {
    Json result = Json::array();
    for (size_t index = 0; index < 16; ++index) {
        result.push_back(glm::value_ptr(value)[index]);
    }
    return result;
}

Entity parentOf(const Scene& scene, Entity entity) {
    return scene.registry().get<RelationshipComponent>(entity).parent;
}

// entity and all of its descendants, depth first.
std::vector<Entity> subtree(const Scene& scene, Entity entity) {
    std::vector<Entity> result;
    std::vector<Entity> pending{entity};
    while (!pending.empty()) {
        const Entity current = pending.back();
        pending.pop_back();
        result.push_back(current);
        const auto& children = scene.registry().get<RelationshipComponent>(current).children;
        pending.insert(pending.end(), children.rbegin(), children.rend());
    }
    return result;
}

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

// The mapped instance members' document objects, keyed by prefab-local id,
// with prefab-local ids and only prefab content. A parent that is not a mapped
// member (an entity added in the scene) is written as null, since no
// prefab-local id names it.
PrefabResult instanceLocalEntities(const Scene& scene, Entity root, const AssetManager& assets,
                                   const SceneDocumentOptions& options, std::map<uint64_t, Json>& output) {
    std::vector<Entity> mapped;
    IdMap sceneToLocal;
    std::unordered_set<uint64_t> locals;
    for (const Entity member : prefabInstanceMembers(scene, root)) {
        const auto* marker = scene.registry().try_get<PrefabEntityComponent>(member);
        // A second member claiming a local id (a copy pasted into the
        // instance) counts as added.
        if (marker != nullptr && marker->entity && locals.insert(marker->entity.value).second) {
            mapped.push_back(member);
            sceneToLocal.emplace(scene.id(member).value, marker->entity.value);
        }
    }
    std::map<uint64_t, Json> serialized;
    if (PrefabResult result = serializeEntities(scene, mapped, assets, options, serialized); !result) {
        return result;
    }
    for (auto& [sceneId, json] : serialized) {
        const bool parentMapped =
            json["parent"].is_number_unsigned() && sceneToLocal.contains(json["parent"].get<uint64_t>());
        remapIds(json, sceneToLocal, componentTypesFor(options), UnmappedReferences::TagScene);
        if (!parentMapped) {
            json["parent"] = nullptr;
        }
        stripNonContentComponents(json);
        output[sceneToLocal.at(sceneId)] = std::move(json);
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

bool isParentOverride(const PrefabOverride& value) {
    return value.component.empty() && value.field == "/parent";
}

bool isRemovedOverride(const PrefabOverride& value) {
    return value.component.empty() && value.field == "/removed";
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
        if (!isRemovedOverride(overrideValue) || !entities.contains(overrideValue.entity.value)) {
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

// Applies overrides to prefab-local entity objects: values first, then
// parents, then removals (which take the removed entities' descendants
// along). Overrides whose entity, component, or parent value no longer exists
// are reported and skipped, except removals of entities the prefab no longer
// has, which already hold. Scene-local components are never overrides.
void applyOverrides(std::map<uint64_t, Json>& entities, std::span<const PrefabOverride> overrides, uint64_t rootLocal,
                    std::vector<std::string>& diagnostics) {
    for (const PrefabOverride& overrideValue : overrides) {
        if (isSceneLocal(overrideValue.component) || isRemovedOverride(overrideValue)) {
            continue;
        }
        const auto entity = entities.find(overrideValue.entity.value);
        if (entity == entities.end()) {
            diagnostics.push_back("Orphaned override for " + describe(overrideValue) +
                                  ": the prefab no longer has that entity");
            continue;
        }
        try {
            if (isParentOverride(overrideValue)) {
                applyParentOverride(entities, overrideValue, rootLocal, diagnostics);
            } else {
                applyOverride(entity->second, overrideValue, diagnostics);
            }
        } catch (const Json::exception& error) {
            diagnostics.push_back("Invalid override for " + describe(overrideValue) + ": " + error.what());
        }
    }
    applyRemovals(entities, overrides, rootLocal, diagnostics);
}

bool hasModelNodes(const Scene& scene, Entity entity) {
    const SceneEntityId id = scene.id(entity);
    for (const Entity descendant : subtree(scene, entity)) {
        const auto* node = scene.registry().try_get<ModelNodeComponent>(descendant);
        if (node != nullptr && node->instanceRoot == id) {
            return true;
        }
    }
    return false;
}

// A prefab entity holding a model component whose node entities the prefab
// does not list (version 1 `modelAsset` prefabs, hand-written prefabs) gets the
// model's node hierarchy instantiated under it.
//
// The node entities get prefab-local ids above the document's, in node order,
// so every instance numbers them alike: a later apply writes them into the
// prefab and other instances then match them by id instead of duplicating
// them.
PrefabResult expandModels(const std::map<uint64_t, Json>& entities, const IdMap& localToScene,
                          const AssetManager& assets, Scene& scene) {
    uint64_t nextLocal = entities.empty() ? 1 : entities.rbegin()->first + 1;
    std::unordered_set<uint64_t> listedRoots;
    for (const auto& [local, json] : entities) {
        for (const ModelRootField& reference : modelRootFields) {
            const Json& components = json.at("components");
            if (const auto component = components.find(reference.component);
                component != components.end() && component->contains(reference.field)) {
                listedRoots.insert(component->at(reference.field).get<uint64_t>());
            }
        }
    }
    for (const auto& [local, json] : entities) {
        if (!json.at("components").contains("model") || listedRoots.contains(local)) {
            continue;
        }
        const Entity entity = scene.findEntity({localToScene.at(local)});
        auto* model = scene.registry().try_get<ModelComponent>(entity);
        if (model == nullptr || hasModelNodes(scene, entity)) {
            continue;
        }
        const ModelHandle handle = model->model;
        scene.registry().remove<ModelComponent>(entity);
        if (SceneResult result = scene.instantiateModel(entity, handle, assets); !result) {
            return {.error = "Could not instantiate prefab model: " + result.error};
        }
        std::vector<std::pair<uint32_t, Entity>> nodes;
        for (const Entity descendant : subtree(scene, entity)) {
            const auto* node = scene.registry().try_get<ModelNodeComponent>(descendant);
            if (node != nullptr && node->instanceRoot == scene.id(entity)) {
                nodes.emplace_back(node->nodeIndex, descendant);
            }
        }
        std::ranges::sort(nodes, {}, &std::pair<uint32_t, Entity>::first);
        for (const auto& [index, node] : nodes) {
            scene.registry().emplace_or_replace<PrefabEntityComponent>(node, PrefabEntityId{nextLocal++});
        }
    }
    return {.success = true};
}

bool sameTarget(const PrefabOverride& left, const PrefabOverride& right) {
    return left.entity == right.entity && left.component == right.component && left.field == right.field;
}

// The overrides to refresh a kept nested instance with when its containing
// instance keeps its own changes: the containing prefab's (possibly changed)
// record, plus what the nested instance changed in the scene since it was last
// synchronized with that record. Its persisted overrides are the record's
// values as of that synchronization, so live overrides equal to them came from
// the old record and give way to the new one.
PrefabResult mergeNestedOverrides(const PrefabSystem& prefabs, const Scene& scene, Entity nested,
                                  std::vector<PrefabOverride>& overrides) {
    const auto& instance = scene.registry().get<PrefabInstanceComponent>(nested);
    std::vector<PrefabOverride> live;
    if (PrefabResult compared = prefabs.overrides(scene, nested, live); !compared) {
        return compared;
    }
    const auto upsert = [&overrides](PrefabOverride value) {
        const auto found = std::ranges::find_if(
            overrides, [&value](const PrefabOverride& existing) { return sameTarget(existing, value); });
        if (found != overrides.end()) {
            *found = std::move(value);
        } else {
            overrides.push_back(std::move(value));
        }
    };
    for (const PrefabOverride& value : live) {
        // A removal is only ever recorded on the instance itself (deleting an
        // entity in the scene), so it always stays.
        if (isRemovedOverride(value) || std::ranges::find(instance.overrides, value) == instance.overrides.end()) {
            upsert(value);
        }
    }
    // The nested root's placement is never a live override; keep it when it
    // moved since the last synchronization.
    if (const auto* rootMarker = scene.registry().try_get<PrefabEntityComponent>(nested)) {
        const PrefabOverride placement{.entity = rootMarker->entity,
                                       .field = "/local_transform",
                                       .jsonValue =
                                           matrixJson(scene.registry().get<TransformComponent>(nested).local).dump()};
        const auto persisted = std::ranges::find_if(
            instance.overrides, [&placement](const PrefabOverride& value) { return sameTarget(value, placement); });
        if (persisted == instance.overrides.end() || persisted->jsonValue != placement.jsonValue) {
            upsert(placement);
        }
    }
    return {.success = true};
}

std::optional<uint64_t> rootId(const PrefabDocument& document) {
    for (const PrefabEntity& entity : document.entities) {
        if (!entity.parent) {
            return entity.id.value;
        }
    }
    return std::nullopt;
}
} // namespace

Entity prefabInstanceRoot(const Scene& scene, Entity entity) {
    for (Entity current = entity; current != entt::null && scene.registry().valid(current);
         current = parentOf(scene, current)) {
        if (scene.registry().all_of<PrefabInstanceComponent>(current)) {
            return current;
        }
    }
    return entt::null;
}

std::vector<Entity> prefabInstanceMembers(const Scene& scene, Entity root, std::vector<Entity>* nestedRoots) {
    std::vector<Entity> members;
    std::vector<Entity> pending{root};
    while (!pending.empty()) {
        const Entity current = pending.back();
        pending.pop_back();
        if (current != root && scene.registry().all_of<PrefabInstanceComponent>(current)) {
            if (nestedRoots != nullptr) {
                nestedRoots->push_back(current);
            }
            continue;
        }
        members.push_back(current);
        const auto& children = scene.registry().get<RelationshipComponent>(current).children;
        pending.insert(pending.end(), children.rbegin(), children.rend());
    }
    return members;
}

std::vector<Entity> prefabInstancesOf(const Scene& scene, const std::filesystem::path& reference) {
    const std::string wanted = normalizedReference(reference);
    std::vector<Entity> roots;
    const auto view = scene.registry().view<const PrefabInstanceComponent>();
    for (const Entity entity : view) {
        if (normalizedReference(view.get<const PrefabInstanceComponent>(entity).prefab) == wanted) {
            roots.push_back(entity);
        }
    }
    std::ranges::sort(roots, {}, [&scene](Entity entity) { return scene.id(entity).value; });
    return roots;
}

void unpackPrefabInstance(Scene& scene, Entity root) {
    std::vector<Entity> nestedRoots;
    for (const Entity member : prefabInstanceMembers(scene, root, &nestedRoots)) {
        scene.registry().remove<PrefabEntityComponent>(member);
    }
    scene.registry().remove<PrefabInstanceComponent>(root);
    for (const Entity nested : nestedRoots) {
        scene.registry().get<PrefabInstanceComponent>(nested).nestedId = 0;
    }
}

bool recordPrefabEntityRemoval(Scene& scene, Entity entity) {
    if (entity == entt::null || !scene.registry().valid(entity)) {
        return false;
    }
    const Entity root = prefabInstanceRoot(scene, entity);
    const auto* marker = scene.registry().try_get<PrefabEntityComponent>(entity);
    if (root == entt::null || root == entity || marker == nullptr || !marker->entity) {
        return false;
    }
    // A copy pasted into the instance claims a local id another member holds;
    // only the member the instance maps the id to stands for the prefab entity.
    for (const Entity member : prefabInstanceMembers(scene, root)) {
        const auto* memberMarker = scene.registry().try_get<PrefabEntityComponent>(member);
        if (memberMarker != nullptr && memberMarker->entity == marker->entity) {
            if (member != entity) {
                return false;
            }
            break;
        }
    }
    auto& instance = scene.registry().get<PrefabInstanceComponent>(root);
    const PrefabOverride removal{.entity = marker->entity, .field = "/removed", .jsonValue = "true"};
    if (std::ranges::find(instance.overrides, removal) == instance.overrides.end()) {
        instance.overrides.push_back(removal);
    }
    return true;
}

PrefabInstanceResult PrefabSystem::transact(Scene& scene, const std::function<PrefabInstanceResult()>& change) {
    // The staging validation catches nearly every failure; this restores the
    // scene when a nested instance fails after its container was rebuilt.
    std::string before;
    const bool restorable = SceneDocument::serialize(scene, m_assets, m_options, before).success;
    PrefabInstanceResult result = change();
    if (result || !restorable) {
        return result;
    }
    std::string after;
    SceneChanges changes;
    if (!SceneDocument::serialize(scene, m_assets, m_options, after) || !SceneDocument::diff(before, after, changes) ||
        !SceneDocument::applyChanges(changes, SceneChangeDirection::Backward, m_assets, m_options, scene)) {
        result.error += " (the scene could not be restored)";
    }
    return result;
}

PrefabSystem::PrefabSystem(AssetManager& assets, SceneDocumentOptions options)
    : m_assets(assets)
    , m_options(std::move(options)) {
    m_options.assetRoot = std::filesystem::absolute(m_options.assetRoot).lexically_normal();
}

PrefabResult PrefabSystem::resolve(const std::filesystem::path& reference, std::filesystem::path& path) const {
    if (reference.empty() || reference.is_absolute()) {
        return {.error = "Prefab references must be project-relative: " + reference.generic_string()};
    }
    path = (m_options.assetRoot / reference).lexically_normal();
    const std::filesystem::path relative = path.lexically_relative(m_options.assetRoot);
    if (relative.empty() || relative == "." || *relative.begin() == "..") {
        return {.error = "Prefab reference escapes the project asset root: " + reference.generic_string()};
    }
    return {.success = true};
}

PrefabResult PrefabSystem::load(const std::filesystem::path& reference, PrefabDocument& document) const {
    std::filesystem::path path;
    if (PrefabResult result = resolve(reference, path); !result) {
        return result;
    }
    return loadPrefab(path, document);
}

PrefabInstanceResult PrefabSystem::instantiate(const std::filesystem::path& reference, Scene& scene, Entity parent,
                                               std::span<const PrefabOverride> overrides) {
    // Resolve the complete nested graph in isolation first, so a missing
    // source, cycle, or invalid component cannot partially change the scene.
    Scene validation;
    std::vector<std::filesystem::path> validationStack;
    if (PrefabInstanceResult checked = build(reference, validation, entt::null, entt::null, overrides, 0,
                                             PrefabRefreshMode::RemoveAddedObjects, validationStack);
        !checked) {
        return checked;
    }
    return transact(scene, [&] {
        std::vector<std::filesystem::path> stack;
        return build(reference, scene, entt::null, parent, overrides, 0, PrefabRefreshMode::RemoveAddedObjects, stack);
    });
}

PrefabInstanceResult PrefabSystem::refresh(Scene& scene, Entity root, std::span<const PrefabOverride> overrides,
                                           PrefabRefreshMode mode) {
    const auto* instance = root == entt::null || !scene.registry().valid(root)
                               ? nullptr
                               : scene.registry().try_get<PrefabInstanceComponent>(root);
    if (instance == nullptr) {
        return {.error = "The entity is not a prefab instance root"};
    }
    // Copied: the rebuild replaces the component the arguments may point into.
    const std::filesystem::path reference = instance->prefab;
    const uint64_t nestedId = instance->nestedId;
    const std::vector<PrefabOverride> values(overrides.begin(), overrides.end());
    Scene validation;
    std::vector<std::filesystem::path> validationStack;
    if (PrefabInstanceResult checked = build(reference, validation, entt::null, entt::null, values, nestedId,
                                             PrefabRefreshMode::RemoveAddedObjects, validationStack);
        !checked) {
        return checked;
    }
    return transact(scene, [&] {
        std::vector<std::filesystem::path> stack;
        return build(reference, scene, root, entt::null, values, nestedId, mode, stack);
    });
}

PrefabInstanceResult PrefabSystem::refresh(Scene& scene, Entity root, PrefabRefreshMode mode) {
    const auto* instance = root == entt::null || !scene.registry().valid(root)
                               ? nullptr
                               : scene.registry().try_get<PrefabInstanceComponent>(root);
    if (instance == nullptr) {
        return {.error = "The entity is not a prefab instance root"};
    }
    const std::vector<PrefabOverride> persisted = instance->overrides;
    return refresh(scene, root, persisted, mode);
}

PrefabInstanceResult PrefabSystem::build(const std::filesystem::path& reference, Scene& scene, Entity existingRoot,
                                         Entity parent, std::span<const PrefabOverride> overrides, uint64_t nestedId,
                                         PrefabRefreshMode mode, std::vector<std::filesystem::path>& stack) {
    std::filesystem::path path;
    if (PrefabResult resolved = resolve(reference, path); !resolved) {
        return {.error = resolved.error};
    }
    if (std::ranges::find(stack, path) != stack.end()) {
        return {.error = "Nested prefab cycle detected at: " + normalizedReference(reference)};
    }
    PrefabDocument document;
    PrefabResult loaded = loadPrefab(path, document);
    if (!loaded) {
        return {.error = loaded.error};
    }
    PrefabInstanceResult result{.success = true, .diagnostics = std::move(loaded.diagnostics)};
    const uint64_t rootLocal = *rootId(document);

    std::map<uint64_t, Json> entities;
    for (const PrefabEntity& entity : document.entities) {
        Json& json = entities[entity.id.value];
        if (PrefabResult encoded = prefabEntityToJson(entity, json); !encoded) {
            return {.error = encoded.error};
        }
        // Older prefabs may hold collection ids of the scene they came from.
        stripNonContentComponents(json);
    }
    applyOverrides(entities, overrides, rootLocal, result.diagnostics);
    const bool rootTransformOverridden = std::ranges::any_of(overrides, [rootLocal](const PrefabOverride& value) {
        return value.entity.value == rootLocal && value.component.empty() && value.field == "/local_transform";
    });

    // Which existing entities stay, and which scene ids the prefab's entities
    // already have.
    IdMap localToScene;
    std::map<uint64_t, Entity> keptNested;
    SceneChanges changes;
    if (existingRoot != entt::null) {
        const auto* rootMarker = scene.registry().try_get<PrefabEntityComponent>(existingRoot);
        if (rootMarker == nullptr || rootMarker->entity.value != rootLocal) {
            return {.error = "The instance root no longer matches the prefab's root entity; unpack the instance "
                             "and instantiate the prefab again"};
        }
        std::vector<Entity> nestedRoots;
        const std::vector<Entity> members = prefabInstanceMembers(scene, existingRoot, &nestedRoots);
        std::unordered_set<uint64_t> stays;
        for (const Entity member : members) {
            const uint64_t sceneId = scene.id(member).value;
            const auto* marker = scene.registry().try_get<PrefabEntityComponent>(member);
            if (marker != nullptr && entities.contains(marker->entity.value) &&
                !localToScene.contains(marker->entity.value)) {
                localToScene.emplace(marker->entity.value, sceneId);
                stays.insert(sceneId);
            } else if (mode == PrefabRefreshMode::KeepAddedObjects && member != existingRoot &&
                       (marker == nullptr || entities.contains(marker->entity.value)) &&
                       stays.contains(scene.id(parentOf(scene, member)).value)) {
                // Added in the scene, or a copy of a prefab entity. Entities
                // whose prefab entity was deleted are removed.
                stays.insert(sceneId);
            }
        }
        // A nested instance stands for a record only while it instantiates the
        // record's prefab.
        std::unordered_map<uint64_t, std::string> recordSources;
        for (const NestedPrefab& record : document.nested) {
            recordSources.emplace(record.id, normalizedReference(reference.parent_path() / record.source));
        }
        for (const Entity nested : nestedRoots) {
            const auto& nestedInstance = scene.registry().get<PrefabInstanceComponent>(nested);
            const uint64_t id = nestedInstance.nestedId;
            const bool parentStays = stays.contains(scene.id(parentOf(scene, nested)).value);
            const auto source = recordSources.find(id);
            const bool isRecord = source != recordSources.end() &&
                                  source->second == normalizedReference(nestedInstance.prefab) &&
                                  !keptNested.contains(id);
            if (parentStays && (isRecord || mode == PrefabRefreshMode::KeepAddedObjects)) {
                if (isRecord) {
                    keptNested.emplace(id, nested);
                }
                for (const Entity entity : subtree(scene, nested)) {
                    stays.insert(scene.id(entity).value);
                }
            }
        }
        for (const Entity entity : subtree(scene, existingRoot)) {
            if (!stays.contains(scene.id(entity).value)) {
                changes.entities.push_back({.id = scene.id(entity)});
            }
        }
    }

    // Rebuilt entities keep their scene organization (collection links).
    std::unordered_map<uint64_t, Json> keptSceneLocal;
    for (const auto& [local, sceneId] : localToScene) {
        if (PrefabResult saved =
                sceneLocalComponents(scene, scene.findEntity({sceneId}), m_assets, m_options, keptSceneLocal[local]);
            !saved) {
            return {.error = saved.error};
        }
    }

    for (const auto& [local, json] : entities) {
        if (!localToScene.contains(local)) {
            localToScene.emplace(local, scene.reserveEntityId().value);
        }
    }

    const ComponentTypeRegistry& types = componentTypesFor(m_options);
    for (const auto& [local, json] : entities) {
        Json target = json;
        remapIds(target, localToScene, types, UnmappedReferences::Resolve);
        if (const auto kept = keptSceneLocal.find(local); kept != keptSceneLocal.end()) {
            target["components"].update(kept->second);
        }
        target["components"][entityKey] = {{"entity", local}};
        if (local == rootLocal) {
            const Entity rootParent = existingRoot != entt::null ? parentOf(scene, existingRoot) : parent;
            target["parent"] = rootParent == entt::null ? Json(nullptr) : Json(scene.id(rootParent).value);
            if (existingRoot != entt::null && !rootTransformOverridden) {
                target["local_transform"] = matrixJson(scene.registry().get<TransformComponent>(existingRoot).local);
            }
            Json persisted = Json::array();
            for (const PrefabOverride& value : overrides) {
                persisted.push_back(prefabOverrideToJson(value));
            }
            target["components"][instanceKey] = {{"prefab", {{"path", normalizedReference(reference)}}},
                                                 {"overrides", std::move(persisted)}};
            if (nestedId != 0) {
                target["components"][instanceKey]["nested_id"] = nestedId;
            }
        }
        changes.entities.push_back({.id = {localToScene.at(local)}, .after = target.dump()});
    }
    if (SceneDocumentResult applied =
            SceneDocument::applyChanges(changes, SceneChangeDirection::Forward, m_assets, m_options, scene);
        !applied) {
        return {.error = normalizedReference(reference) + ": " + applied.error};
    }
    if (PrefabResult expanded = expandModels(entities, localToScene, m_assets, scene); !expanded) {
        return {.error = expanded.error};
    }
    const Entity root = scene.findEntity({localToScene.at(rootLocal)});

    stack.push_back(path);
    for (const NestedPrefab& record : document.nested) {
        const std::filesystem::path nestedReference = (reference.parent_path() / record.source).lexically_normal();
        const auto kept = keptNested.find(record.id);
        PrefabInstanceResult child;
        if (kept != keptNested.end()) {
            std::vector<PrefabOverride> nestedOverrides(record.overrides.begin(), record.overrides.end());
            if (mode == PrefabRefreshMode::KeepAddedObjects) {
                if (PrefabResult merged = mergeNestedOverrides(*this, scene, kept->second, nestedOverrides); !merged) {
                    stack.pop_back();
                    return {.error = merged.error};
                }
            }
            child = build(nestedReference, scene, kept->second, entt::null, nestedOverrides, record.id, mode, stack);
        } else {
            const uint64_t parentLocal = record.parent ? record.parent->value : rootLocal;
            if (!entities.contains(parentLocal)) {
                // The instance removed the entity the record is parented to.
                continue;
            }
            const Entity nestedParent = scene.findEntity({localToScene.at(parentLocal)});
            child = build(nestedReference, scene, entt::null, nestedParent, record.overrides, record.id, mode, stack);
        }
        if (!child) {
            stack.pop_back();
            return child;
        }
        result.diagnostics.insert(result.diagnostics.end(), child.diagnostics.begin(), child.diagnostics.end());
    }
    stack.pop_back();

    scene.updateWorldTransforms();
    result.root = root;
    result.entities = subtree(scene, root);
    return result;
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

PrefabResult PrefabSystem::overrides(const Scene& scene, Entity root, const PrefabDocument& source,
                                     std::vector<PrefabOverride>& output) const {
    std::map<uint64_t, Json> sourceEntities;
    if (PrefabResult result = normalizedEntities(source, m_assets, m_options, sourceEntities); !result) {
        return result;
    }
    std::map<uint64_t, Json> instanceEntities;
    if (PrefabResult result = instanceLocalEntities(scene, root, m_assets, m_options, instanceEntities); !result) {
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
    // A prefab entity the instance lacks was deleted in the scene only when
    // the deletion was recorded; otherwise the prefab gained it since the
    // instance was last refreshed, which is not an override.
    if (const auto* instance = scene.registry().try_get<PrefabInstanceComponent>(root)) {
        for (const PrefabOverride& value : instance->overrides) {
            if (isRemovedOverride(value) && sourceEntities.contains(value.entity.value) &&
                !instanceEntities.contains(value.entity.value) && std::ranges::find(found, value) == found.end()) {
                found.push_back(value);
            }
        }
    }
    std::ranges::sort(found, {}, [](const PrefabOverride& value) {
        return std::tie(value.entity.value, value.component, value.field);
    });
    output = std::move(found);
    return {.success = true};
}

PrefabResult PrefabSystem::capture(const Scene& scene, Entity root, const std::filesystem::path& reference,
                                   PrefabCapture& output) const {
    std::filesystem::path path;
    if (PrefabResult resolved = resolve(reference, path); !resolved) {
        return resolved;
    }
    std::vector<Entity> nestedRoots;
    const std::vector<Entity> members = prefabInstanceMembers(scene, root, &nestedRoots);
    const auto* instance = scene.registry().try_get<PrefabInstanceComponent>(root);
    const bool applying =
        instance != nullptr && normalizedReference(instance->prefab) == normalizedReference(reference);

    PrefabCapture captured;
    IdMap sceneToLocal;
    std::unordered_set<uint64_t> usedLocal;
    if (applying) {
        for (const Entity member : members) {
            const auto* marker = scene.registry().try_get<PrefabEntityComponent>(member);
            if (marker != nullptr && marker->entity && usedLocal.insert(marker->entity.value).second) {
                sceneToLocal.emplace(scene.id(member).value, marker->entity.value);
            }
        }
    }
    // An apply never reuses the ids of entities or nested records the prefab
    // had, so other instances cannot mistake a new entity for a deleted one.
    PrefabDocument previous;
    const bool hasPrevious = applying && load(reference, previous).success;
    uint64_t nextLocal = usedLocal.empty() ? 1 : *std::ranges::max_element(usedLocal) + 1;
    uint64_t nextNested = 1;
    if (hasPrevious) {
        for (const PrefabEntity& entity : previous.entities) {
            nextLocal = std::max(nextLocal, entity.id.value + 1);
        }
        for (const NestedPrefab& record : previous.nested) {
            nextNested = std::max(nextNested, record.id + 1);
        }
    }
    for (const Entity member : members) {
        if (!sceneToLocal.contains(scene.id(member).value)) {
            sceneToLocal.emplace(scene.id(member).value, nextLocal++);
        }
    }

    // An apply keeps the prefab's own root placement; a new prefab starts from
    // the captured root's.
    std::optional<glm::mat4> rootTransform;
    if (hasPrevious) {
        for (const PrefabEntity& entity : previous.entities) {
            if (!entity.parent) {
                rootTransform = entity.localTransform;
            }
        }
    }

    std::map<uint64_t, Json> serialized;
    if (PrefabResult result = serializeEntities(scene, members, m_assets, m_options, serialized); !result) {
        return result;
    }
    std::vector<std::string> diagnostics;
    for (const Entity member : members) {
        Json& json = serialized.at(scene.id(member).value);
        remapIds(json, sceneToLocal, componentTypesFor(m_options), UnmappedReferences::Clear, &diagnostics);
        stripNonContentComponents(json);
        PrefabEntity entity;
        if (PrefabResult read = prefabEntityFromJson(json, entity); !read) {
            return read;
        }
        if (member == root) {
            entity.parent.reset();
            if (rootTransform) {
                entity.localTransform = *rootTransform;
            }
        }
        captured.entityIds.emplace_back(scene.id(member), entity.id);
        captured.document.entities.push_back(std::move(entity));
    }

    std::unordered_set<uint64_t> usedNested;
    std::vector<std::pair<Entity, uint64_t>> nestedIds;
    for (const Entity nested : nestedRoots) {
        const uint64_t id = scene.registry().get<PrefabInstanceComponent>(nested).nestedId;
        nestedIds.emplace_back(nested, applying && id != 0 && usedNested.insert(id).second ? id : 0);
    }
    if (!usedNested.empty()) {
        nextNested = std::max(nextNested, *std::ranges::max_element(usedNested) + 1);
    }
    const std::filesystem::path directory = path.parent_path();
    for (auto& [nested, id] : nestedIds) {
        if (id == 0) {
            id = nextNested++;
        }
        const auto& nestedInstance = scene.registry().get<PrefabInstanceComponent>(nested);
        const auto* nestedRootMarker = scene.registry().try_get<PrefabEntityComponent>(nested);
        if (nestedRootMarker == nullptr) {
            return {.error = "Nested prefab instance '" + scene.registry().get<NameComponent>(nested).name +
                             "' is missing its prefab link"};
        }
        std::filesystem::path nestedPath;
        if (PrefabResult resolved = resolve(nestedInstance.prefab, nestedPath); !resolved) {
            return resolved;
        }
        NestedPrefab record{.id = id,
                            .source = nestedPath.lexically_relative(directory),
                            .parent = PrefabEntityId{sceneToLocal.at(scene.id(parentOf(scene, nested)).value)}};
        if (PrefabResult result = overrides(scene, nested, record.overrides); !result) {
            return {.error = "Nested prefab instance '" + scene.registry().get<NameComponent>(nested).name +
                             "': " + result.error};
        }
        // The nested root's placement inside the containing prefab.
        record.overrides.push_back(
            {.entity = nestedRootMarker->entity,
             .field = "/local_transform",
             .jsonValue = matrixJson(scene.registry().get<TransformComponent>(nested).local).dump()});
        captured.nestedIds.emplace_back(scene.id(nested), id);
        captured.document.nested.push_back(std::move(record));
    }
    if (PrefabResult checked = checkNestedGraph(reference, captured.document); !checked) {
        return checked;
    }
    output = std::move(captured);
    return {.success = true, .diagnostics = std::move(diagnostics)};
}

PrefabResult PrefabSystem::checkNestedGraph(const std::filesystem::path& reference,
                                            const PrefabDocument& document) const {
    std::vector<std::string> stack{normalizedReference(reference)};
    std::function<PrefabResult(const std::filesystem::path&, const PrefabDocument&)> visit =
        [&](const std::filesystem::path& containing, const PrefabDocument& current) -> PrefabResult {
        for (const NestedPrefab& record : current.nested) {
            const std::filesystem::path child = (containing.parent_path() / record.source).lexically_normal();
            const std::string key = normalizedReference(child);
            if (std::ranges::find(stack, key) != stack.end()) {
                return {.error = "Saving " + normalizedReference(reference) +
                                 " would make a nested prefab cycle through " + key};
            }
            PrefabDocument nested;
            if (PrefabResult loaded = load(child, nested); !loaded) {
                return loaded;
            }
            stack.push_back(key);
            if (PrefabResult result = visit(child, nested); !result) {
                return result;
            }
            stack.pop_back();
        }
        return {.success = true};
    };
    return visit(reference, document);
}
