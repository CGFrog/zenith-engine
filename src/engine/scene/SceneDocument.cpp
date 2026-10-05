#include "SceneDocument.hpp"

#include "../assets/Assets.hpp"
#include "ComponentTypes.hpp"
#include "EntityReferenceRemap.hpp"

#include <glm/gtc/type_ptr.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace {
using Json = nlohmann::json;

Json matrix4(const glm::mat4& value) {
    Json result = Json::array();
    const float* elements = glm::value_ptr(value);
    for (size_t index = 0; index < 16; ++index) {
        result.push_back(elements[index]);
    }
    return result;
}

bool readMatrix4(const Json& json, glm::mat4& value) {
    if (!json.is_array() || json.size() != 16) {
        return false;
    }
    float* elements = glm::value_ptr(value);
    for (size_t index = 0; index < 16; ++index) {
        elements[index] = json[index].get<float>();
    }
    return true;
}

// Version 1 wrote these built-in fields as bare path strings. The table is
// frozen with the version 1 format; later fields are written in the current
// shape from the start.
struct LegacyAssetField {
    const char* component;
    const char* field;
};

constexpr std::array<LegacyAssetField, 6> version1AssetFields{{
    {"environment_light", "environment_asset"},
    {"reflection_probe", "environment_asset"},
    {"particle_emitter", "texture_asset"},
    {"decal", "albedo_asset"},
    {"decal", "normal_asset"},
    {"irradiance_volume", "bake_asset"},
}};

// Version 1 to 2: path strings become reference objects. Model, mesh, and
// skin references were already `{"path": ...}` objects; empty strings still
// mean "no asset". Asset fields of game component types stay strings, which
// the loader still accepts.
SceneDocumentResult migrateVersion1To2(Json& document) {
    for (Json& entity : document["entities"]) {
        if (!entity.is_object()) {
            continue;
        }
        const auto components = entity.find("components");
        if (components == entity.end() || !components->is_object()) {
            continue;
        }
        for (const LegacyAssetField& legacy : version1AssetFields) {
            const auto component = components->find(legacy.component);
            if (component == components->end() || !component->is_object()) {
                continue;
            }
            const auto field = component->find(legacy.field);
            if (field != component->end() && field->is_string() && !field->get_ref<const std::string&>().empty()) {
                *field = Json{{"path", field->get<std::string>()}};
            }
        }
    }
    return {.success = true};
}

// Version 2 to 3: documents gain a top-level collections array. Older scenes
// had no collections, so every entity stays in the implicit root collection.
SceneDocumentResult migrateVersion2To3(Json& document) {
    if (document.contains("collections")) {
        return {.error = "A version 2 document cannot define collections"};
    }
    document["collections"] = Json::array();
    return {.success = true};
}

// Upgrades the raw JSON of a document written at fromVersion to
// fromVersion + 1. Steps run in order before the document is validated.
struct DocumentMigration {
    uint32_t fromVersion;
    SceneDocumentResult (*apply)(Json& document);
};

constexpr std::array<DocumentMigration, 2> documentMigrations{{{1, migrateVersion1To2}, {2, migrateVersion2To3}}};
static_assert(documentMigrations.size() + 1 == SceneDocument::currentVersion,
              "every earlier scene document version needs a migration");

// Checks the document's shape and version and upgrades it to the current
// version in place.
SceneDocumentResult upgradeDocument(Json& document) {
    if (!document.is_object() || !document.contains("version") || !document.contains("entities") ||
        !document["entities"].is_array()) {
        return {.error = "Scene document must contain version and entities fields"};
    }
    const Json& versionField = document["version"];
    if (!versionField.is_number_unsigned() || versionField.get<uint64_t>() == 0 ||
        versionField.get<uint64_t>() > (std::numeric_limits<uint32_t>::max)()) {
        return {.error = "Unsupported scene document version " + versionField.dump() +
                         ": the version must be a positive whole number"};
    }
    uint32_t version = versionField.get<uint32_t>();
    if (version > SceneDocument::currentVersion) {
        return {.error = "Unsupported scene document version " + std::to_string(version) +
                         ": this build reads versions up to " + std::to_string(SceneDocument::currentVersion) +
                         "; the file was saved by a newer Zenith"};
    }
    for (; version < SceneDocument::currentVersion; ++version) {
        const auto migration = std::ranges::find(documentMigrations, version, &DocumentMigration::fromVersion);
        if (SceneDocumentResult result = migration->apply(document); !result) {
            return {.error = "Scene document migration from version " + std::to_string(version) +
                             " failed: " + result.error};
        }
    }
    if (!document.contains("collections") || !document["collections"].is_array()) {
        return {.error = "Scene document must contain a collections array"};
    }
    document["version"] = SceneDocument::currentVersion;
    return {.success = true};
}

Json vector3Json(const glm::vec3& value) {
    return Json::array({value.x, value.y, value.z});
}

Json collectionsJson(const SceneCollections& collections) {
    Json result = Json::array();
    for (const SceneCollection& collection : collections.all()) {
        result.push_back({{"id", collection.id.value},
                          {"name", collection.name},
                          {"parent", collection.parent ? Json(collection.parent.value) : Json(nullptr)},
                          {"visible", collection.visible},
                          {"selectable", collection.selectable},
                          {"renderable", collection.renderable},
                          {"instance_offset", vector3Json(collection.instanceOffset)}});
    }
    return result;
}

// Reads a collections array. Parents may follow their children in the array
// (a collection moved under a newer one keeps its smaller id), so every
// collection is added first and nested afterwards, which also rejects cycles.
SceneDocumentResult readCollections(const Json& array, SceneCollections& output) {
    if (!array.is_array()) {
        return {.error = "Scene collections must be an array"};
    }
    SceneCollections collections;
    std::vector<std::pair<SceneCollectionId, SceneCollectionId>> parents;
    for (const Json& serialized : array) {
        SceneCollection collection{.id = {serialized.at("id").get<uint64_t>()},
                                   .name = serialized.at("name").get<std::string>(),
                                   .visible = serialized.at("visible").get<bool>(),
                                   .selectable = serialized.at("selectable").get<bool>(),
                                   .renderable = serialized.at("renderable").get<bool>()};
        const Json& offset = serialized.at("instance_offset");
        if (!offset.is_array() || offset.size() != 3) {
            return {.error = "Collection " + std::to_string(collection.id.value) +
                             " instance_offset must contain three numbers"};
        }
        collection.instanceOffset = {offset[0].get<float>(), offset[1].get<float>(), offset[2].get<float>()};
        if (const Json& parent = serialized.at("parent"); !parent.is_null()) {
            const SceneCollectionId parentId{parent.get<uint64_t>()};
            if (!parentId) {
                return {.error = "Collection " + std::to_string(collection.id.value) +
                                 " references invalid parent collection 0"};
            }
            parents.emplace_back(collection.id, parentId);
        }
        if (CollectionResult result = collections.add(std::move(collection)); !result) {
            return {.error = result.error};
        }
    }
    for (const auto& [child, parent] : parents) {
        if (CollectionResult result = collections.setParent(child, parent); !result) {
            return {.error = "Collection " + std::to_string(child.value) + ": " + result.error};
        }
    }
    output = std::move(collections);
    return {.success = true};
}

// The ids of entity's saved (non-transient) children in child order when that
// is not ascending id order, which is what readers assume without the key.
std::optional<Json> childOrderJson(const Scene& scene, const RelationshipComponent& relationship) {
    std::vector<uint64_t> ids;
    ids.reserve(relationship.children.size());
    for (const Entity child : relationship.children) {
        if (scene.registry().valid(child) && !scene.isTransient(child)) {
            ids.push_back(scene.id(child).value);
        }
    }
    if (std::ranges::is_sorted(ids)) {
        return std::nullopt;
    }
    return Json(ids);
}

// Reads an entity object's optional "children" order: an array of non-zero
// ids. Absent means ascending id order (an empty order).
SceneDocumentResult readChildOrder(const Json& entity, std::vector<uint64_t>& order) {
    order.clear();
    const auto children = entity.find("children");
    if (children == entity.end()) {
        return {.success = true};
    }
    const auto label = [&entity] { return "Entity " + std::to_string(entity.value("id", uint64_t{0})); };
    if (!children->is_array()) {
        return {.error = label() + " has a children order that is not an array"};
    }
    for (const Json& child : *children) {
        if (!child.is_number_unsigned() || child.get<uint64_t>() == 0) {
            return {.error = label() + " lists an invalid child id"};
        }
        order.push_back(child.get<uint64_t>());
    }
    return {.success = true};
}

// Puts parent's children in order: the ones order lists first, as listed, and
// then the rest (transient children, or children the order does not know) in
// ascending id order. Ids in order that are not children are ignored, so an
// order copied with a subset of the hierarchy stays usable.
void applyChildOrder(Scene& scene, Entity parent, std::span<const uint64_t> order) {
    auto& children = scene.registry().get<RelationshipComponent>(parent).children;
    if (children.size() < 2) {
        return;
    }
    std::unordered_map<uint64_t, size_t> rank;
    for (size_t index = 0; index < order.size(); ++index) {
        rank.emplace(order[index], index);
    }
    const auto key = [&scene, &rank](Entity child) {
        const uint64_t id = scene.id(child).value;
        const auto found = rank.find(id);
        return std::pair{found == rank.end() ? (std::numeric_limits<size_t>::max)() : found->second, id};
    };
    std::ranges::sort(children, {}, key);
}

SceneDocumentResult serializeEntity(const Scene& scene, Entity entity, const AssetManager& assets,
                                    const SceneDocumentOptions& options, Json& output) {
    const entt::registry& registry = scene.registry();
    const SceneEntityId entityId = scene.id(entity);
    if (!entityId) {
        return {.error = "Scene entity is missing a stable ID"};
    }
    const auto* name = registry.try_get<NameComponent>(entity);
    const auto* transform = registry.try_get<TransformComponent>(entity);
    const auto* relationship = registry.try_get<RelationshipComponent>(entity);
    if (name == nullptr || transform == nullptr || relationship == nullptr) {
        return {.error = "Scene entity " + std::to_string(entityId.value) + " is missing a required component"};
    }

    output = {{"id", entityId.value}, {"name", name->name}, {"local_transform", matrix4(transform->local)}};
    output["parent"] = relationship->parent == entt::null ? Json(nullptr) : Json(scene.id(relationship->parent).value);
    if (std::optional<Json> order = childOrderJson(scene, *relationship)) {
        output["children"] = std::move(*order);
    }
    Json components = Json::object();
    const ComponentSaveContext context{.assets = assets, .options = options};
    for (const ComponentType& type : componentTypesFor(options).types()) {
        if (!type.has(scene, entity)) {
            continue;
        }
        Json component;
        if (SceneDocumentResult result = type.save(scene, entity, context, component); !result) {
            return result;
        }
        components[type.key()] = std::move(component);
    }
    if (const auto* preserved = registry.try_get<PreservedComponentsComponent>(entity)) {
        for (const auto& [key, payload] : preserved->components) {
            // A type registered since loading now owns the key.
            if (!components.contains(key)) {
                components[key] = Json::parse(payload);
            }
        }
    }
    output["components"] = std::move(components);
    return {.success = true};
}

// The stable ids of the scene's transient entities; empty when there are none.
std::unordered_set<uint64_t> transientEntityIds(const Scene& scene) {
    std::unordered_set<uint64_t> ids;
    for (const Entity entity : scene.transientEntities()) {
        ids.insert(scene.id(entity).value);
    }
    return ids;
}

// Writes the nullable references a serialized entity holds to transient
// entities as none, reporting each to warnings when given. Non-nullable
// references (instance roots) cannot point outside their transient subtree.
void dropTransientReferences(Json& entity, const ComponentTypeRegistry& types,
                             const std::unordered_set<uint64_t>& transient, std::vector<std::string>* warnings) {
    const uint64_t id = entity.at("id").get<uint64_t>();
    remapEntityReferences(
        entity["components"], types, [&](uint64_t target, const EntityReferenceSite& site) -> std::optional<uint64_t> {
            if (!site.nullable || !transient.contains(target)) {
                return std::nullopt;
            }
            if (warnings != nullptr) {
                warnings->push_back("Entity " + std::to_string(id) + " " + std::string(site.component) +
                                    std::string(site.field) + " referred to generated entity " +
                                    std::to_string(target) + ", which is not saved; it was saved as none");
            }
            return uint64_t{0};
        });
}

SceneDocumentResult deserializeComponents(const Json& components, Entity entity, AssetManager& assets,
                                          const SceneDocumentOptions& options, Scene& scene) {
    if (!components.is_object()) {
        return {.error = "Entity components must be an object"};
    }
    const ComponentTypeRegistry& types = componentTypesFor(options);
    const ComponentLoadContext context{.assets = assets, .options = options};
    for (const ComponentType& type : types.types()) {
        if (const auto found = components.find(type.key()); found != components.end()) {
            if (SceneDocumentResult result = type.load(*found, scene, entity, context); !result) {
                return result;
            }
        }
    }
    PreservedComponentsComponent preserved;
    for (auto component = components.begin(); component != components.end(); ++component) {
        if (types.find(component.key()) == nullptr) {
            preserved.components.emplace_back(component.key(), component.value().dump());
        }
    }
    if (!preserved.components.empty()) {
        scene.registry().emplace_or_replace<PreservedComponentsComponent>(entity, std::move(preserved));
    }
    return {.success = true};
}

SceneDocumentResult replaceFile(const std::filesystem::path& temporary, const std::filesystem::path& destination) {
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return {.error =
                    "Could not atomically replace scene file (Windows error " + std::to_string(GetLastError()) + ")"};
    }
#else
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        return {.error = "Could not atomically replace scene file: " + error.message()};
    }
#endif
    return {.success = true};
}

// The entity objects of a serialized document, keyed by stable id.
SceneDocumentResult readDocumentEntities(std::string_view input, Json& document,
                                         std::map<uint64_t, const Json*>& entities) {
    document = Json::parse(input);
    if (SceneDocumentResult result = upgradeDocument(document); !result) {
        return result;
    }
    for (const Json& serialized : document["entities"]) {
        const uint64_t id = serialized.at("id").get<uint64_t>();
        if (id == 0) {
            return {.error = "Scene contains invalid entity ID 0"};
        }
        if (!entities.emplace(id, &serialized).second) {
            return {.error = "Duplicate scene entity ID " + std::to_string(id)};
        }
    }
    return {.success = true};
}

std::optional<std::string> compactEntity(const Json* serialized) {
    return serialized == nullptr ? std::nullopt : std::optional<std::string>(serialized->dump());
}

// An entity a patch creates or rebuilds, read and loaded into a staging scene
// before the destination changes.
struct StagedEntity {
    SceneEntityId id;
    std::string name;
    glm::mat4 local{1.0f};
    SceneEntityId parent;
    // Its "children" order (see readChildOrder).
    std::vector<uint64_t> childOrder;
    Entity staged{entt::null};
};

SceneDocumentResult stageEntity(SceneEntityId id, const std::string& serialized, AssetManager& assets,
                                const SceneDocumentOptions& options, Scene& staging, StagedEntity& output) {
    const Json json = Json::parse(serialized);
    if (!json.is_object() || json.at("id").get<uint64_t>() != id.value) {
        return {.error = "Change for entity " + std::to_string(id.value) + " describes a different entity"};
    }
    output.id = id;
    output.name = json.at("name").get<std::string>();
    if (!readMatrix4(json.at("local_transform"), output.local)) {
        return {.error = "Entity " + std::to_string(id.value) + " has an invalid local transform"};
    }
    const Json& parent = json.at("parent");
    output.parent = parent.is_null() ? SceneEntityId{} : SceneEntityId{parent.get<uint64_t>()};
    if (!parent.is_null() && !output.parent) {
        return {.error = "Entity " + std::to_string(id.value) + " references invalid parent ID 0"};
    }
    if (SceneDocumentResult result = readChildOrder(json, output.childOrder); !result) {
        return result;
    }
    if (SceneResult result = staging.createEntityWithId(id, output.name, output.staged); !result) {
        return {.error = result.error};
    }
    if (SceneDocumentResult result =
            deserializeComponents(json.at("components"), output.staged, assets, options, staging);
        !result) {
        return {.error = "Entity " + std::to_string(id.value) + ": " + result.error};
    }
    return {.success = true};
}

// What a patch will do, checked against the destination's current hierarchy
// before anything changes. Upserts are sorted by id.
struct PatchPlan {
    std::vector<StagedEntity> upserts;
    std::unordered_map<uint64_t, size_t> upsertIndices;
    std::unordered_set<uint64_t> removals;
    std::unordered_map<uint64_t, Entity> existing;
};

SceneEntityId currentParent(const Scene& scene, Entity entity) {
    const Entity parent = scene.registry().get<RelationshipComponent>(entity).parent;
    return parent == entt::null ? SceneEntityId{} : scene.id(parent);
}

SceneDocumentResult validateHierarchy(const PatchPlan& plan, const Scene& scene) {
    const auto survives = [&plan](uint64_t id) {
        return plan.upsertIndices.contains(id) || (plan.existing.contains(id) && !plan.removals.contains(id));
    };
    const auto finalParent = [&](uint64_t id) {
        if (const auto upsert = plan.upsertIndices.find(id); upsert != plan.upsertIndices.end()) {
            return plan.upserts[upsert->second].parent.value;
        }
        return currentParent(scene, plan.existing.at(id)).value;
    };

    for (const StagedEntity& upsert : plan.upserts) {
        if (upsert.parent && !survives(upsert.parent.value)) {
            return {.error = "Entity " + std::to_string(upsert.id.value) + " references missing parent " +
                             std::to_string(upsert.parent.value)};
        }
    }
    // Every parent now resolves, so ancestor walks stay inside the planned
    // scene. Ids already shown to reach a root are not walked again.
    std::unordered_set<uint64_t> rooted;
    for (const StagedEntity& upsert : plan.upserts) {
        std::unordered_set<uint64_t> path{upsert.id.value};
        for (uint64_t ancestor = upsert.parent.value; ancestor != 0 && !rooted.contains(ancestor);
             ancestor = finalParent(ancestor)) {
            if (!path.insert(ancestor).second) {
                return {.error = "Scene hierarchy contains a cycle at entity " + std::to_string(ancestor)};
            }
        }
        rooted.insert(path.begin(), path.end());
    }

    for (const uint64_t removed : plan.removals) {
        const auto found = plan.existing.find(removed);
        if (found == plan.existing.end()) {
            continue;
        }
        for (const Entity child : scene.registry().get<RelationshipComponent>(found->second).children) {
            const uint64_t childId = scene.id(child).value;
            // Documents never list transient children; they go with the parent.
            if (!plan.removals.contains(childId) && !plan.upsertIndices.contains(childId) &&
                !scene.isTransient(child)) {
                return {.error = "Removing entity " + std::to_string(removed) + " would also remove unchanged child " +
                                 std::to_string(childId)};
            }
        }
    }
    return {.success = true};
}

// Every collection the patched scene references must exist in its final
// collection set: the staged entities' references, and those of existing
// entities the patch keeps unchanged.
SceneDocumentResult validateCollectionReferences(const PatchPlan& plan, const Scene& staging, const Scene& scene,
                                                 const SceneCollections& collections) {
    if (std::string missing = findMissingCollectionReference(staging, collections); !missing.empty()) {
        return {.error = std::move(missing)};
    }
    const auto unchanged = [&plan](uint64_t id) {
        return !plan.removals.contains(id) && !plan.upsertIndices.contains(id);
    };
    for (const auto [entity, membership] : scene.registry().view<const CollectionMembershipComponent>().each()) {
        const uint64_t id = scene.id(entity).value;
        for (const SceneCollectionId collection : membership.collections) {
            if (unchanged(id) && collections.find(collection) == nullptr) {
                return {.error = "Entity " + std::to_string(id) + " is a member of missing collection " +
                                 std::to_string(collection.value)};
            }
        }
    }
    for (const auto [entity, instance] : scene.registry().view<const CollectionInstanceComponent>().each()) {
        const uint64_t id = scene.id(entity).value;
        if (unchanged(id) && collections.find(instance.collection) == nullptr) {
            return {.error = "Entity " + std::to_string(id) + " instances missing collection " +
                             std::to_string(instance.collection.value)};
        }
    }
    return {.success = true};
}

// Removes every component except the stable id, name, transform, and
// hierarchy, so the entity starts from what a fresh document load creates.
// Scene-private transform caches are recreated when the transform is marked
// dirty.
void stripComponents(Scene& scene, Entity entity) {
    for (auto&& pool : scene.registry().storage()) {
        auto& storage = pool.second;
        const entt::type_info& type = storage.type();
        if (type == entt::type_id<SceneEntityIdComponent>() || type == entt::type_id<NameComponent>() ||
            type == entt::type_id<TransformComponent>() || type == entt::type_id<RelationshipComponent>()) {
            continue;
        }
        if (storage.contains(entity)) {
            storage.remove(entity);
        }
    }
}

// Moves child under parent, or to the root when parent is null, placing it
// before the first sibling with a larger stable id.
void attachInIdOrder(Scene& scene, Entity child, Entity parent) {
    entt::registry& registry = scene.registry();
    auto& relationship = registry.get<RelationshipComponent>(child);
    if (relationship.parent != entt::null && registry.valid(relationship.parent)) {
        std::erase(registry.get<RelationshipComponent>(relationship.parent).children, child);
    }
    relationship.parent = parent;
    if (parent == entt::null) {
        return;
    }
    auto& siblings = registry.get<RelationshipComponent>(parent).children;
    const uint64_t childId = scene.id(child).value;
    const auto position =
        std::ranges::find_if(siblings, [&scene, childId](Entity sibling) { return scene.id(sibling).value > childId; });
    siblings.insert(position, child);
}

bool hasPhysics(const Scene& scene, Entity entity) {
    return scene.registry().any_of<ColliderComponent, RigidBodyComponent>(entity);
}

void transferComponents(const Scene& staging, Entity source, const ComponentTypeRegistry& types, Scene& scene,
                        Entity destination) {
    for (const ComponentType& type : types.types()) {
        if (const void* component = type.find(staging, source)) {
            type.addDefault(scene, destination);
            type.assign(type.find(scene, destination), type.copy(component));
        }
    }
    if (const auto* preserved = staging.registry().try_get<PreservedComponentsComponent>(source)) {
        scene.registry().emplace<PreservedComponentsComponent>(destination, *preserved);
    }
}

// Applies a validated plan. Nothing here can fail: ids, parents, hierarchy,
// and components were all checked while planning.
void applyPlan(const PatchPlan& plan, const Scene& staging, const SceneDocumentOptions& options, Scene& scene) {
    std::unordered_map<uint64_t, Entity> entities = plan.existing;
    std::vector<bool> hadPhysics(plan.upserts.size(), false);
    for (size_t index = 0; index < plan.upserts.size(); ++index) {
        const StagedEntity& upsert = plan.upserts[index];
        const auto found = entities.find(upsert.id.value);
        if (found == entities.end()) {
            Entity created = entt::null;
            scene.createEntityWithId(upsert.id, upsert.name, created);
            entities[upsert.id.value] = created;
            continue;
        }
        hadPhysics[index] = hasPhysics(scene, found->second);
        scene.registry().get<NameComponent>(found->second).name = upsert.name;
        stripComponents(scene, found->second);
    }

    // Reparent before removing so a moved child is not destroyed with its old
    // parent; validation guaranteed every other child is removed too.
    for (const StagedEntity& upsert : plan.upserts) {
        const Entity entity = entities.at(upsert.id.value);
        scene.registry().get<TransformComponent>(entity).local = upsert.local;
        const Entity parent = upsert.parent ? entities.at(upsert.parent.value) : Entity{entt::null};
        const bool created = !plan.existing.contains(upsert.id.value);
        if (created || scene.registry().get<RelationshipComponent>(entity).parent != parent) {
            attachInIdOrder(scene, entity, parent);
        }
    }
    for (const uint64_t removed : plan.removals) {
        if (const auto found = plan.existing.find(removed); found != plan.existing.end()) {
            scene.destroyEntity(found->second);
        }
    }
    // Every move is done, so each rebuilt entity's children are all in place.
    for (const StagedEntity& upsert : plan.upserts) {
        applyChildOrder(scene, entities.at(upsert.id.value), upsert.childOrder);
    }

    const ComponentTypeRegistry& types = componentTypesFor(options);
    for (size_t index = 0; index < plan.upserts.size(); ++index) {
        const StagedEntity& upsert = plan.upserts[index];
        const Entity entity = entities.at(upsert.id.value);
        transferComponents(staging, upsert.staged, types, scene, entity);
        scene.markTransformDirty(entity);
        if (hadPhysics[index] || hasPhysics(scene, entity)) {
            scene.markPhysicsDirty(entity);
        }
    }
    if (!plan.upserts.empty()) {
        // Revisions are scene-wide counters; one bump invalidates every cache
        // that might hold a rebuilt entity's previous components.
        const Entity any = entities.at(plan.upserts.front().id.value);
        scene.markRenderableDirty(any);
        scene.markLightDirty(any);
        scene.markCameraDirty(any);
        scene.markSkinDirty(any);
        scene.markMaterialDirty(any);
    }
    scene.updateWorldTransforms();
}

// An entity createEntities adds, read from its document object. Sorted by id.
struct NewEntity {
    SceneEntityId id;
    const Json* json{nullptr};
    std::string name;
    glm::mat4 local{1.0f};
    SceneEntityId parent;
    std::vector<uint64_t> childOrder;
    Entity entity{entt::null};
};

// Reads the entities and checks their ids, parents, and hierarchy against
// scene, which is not changed.
SceneDocumentResult planNewEntities(std::span<const SceneEntityDocument> documents, const Scene& scene,
                                    std::vector<NewEntity>& output) {
    std::vector<NewEntity> planned;
    planned.reserve(documents.size());
    for (const SceneEntityDocument& document : documents) {
        const auto label = [&document] { return "Entity " + std::to_string(document.id.value); };
        if (!document.id || document.entity == nullptr || !document.entity->is_object()) {
            return {.error = label() + " is not a valid new entity"};
        }
        if (scene.findEntity(document.id) != entt::null) {
            return {.error = "Duplicate scene entity ID " + std::to_string(document.id.value)};
        }
        NewEntity entity{.id = document.id, .json = document.entity};
        entity.name = document.entity->at("name").get<std::string>();
        if (!readMatrix4(document.entity->at("local_transform"), entity.local)) {
            return {.error = label() + " has an invalid local transform"};
        }
        const Json& parent = document.entity->at("parent");
        entity.parent = parent.is_null() ? SceneEntityId{} : SceneEntityId{parent.get<uint64_t>()};
        if (!parent.is_null() && !entity.parent) {
            return {.error = label() + " references invalid parent ID 0"};
        }
        if (SceneDocumentResult result = readChildOrder(*document.entity, entity.childOrder); !result) {
            return result;
        }
        planned.push_back(std::move(entity));
    }
    // Ascending ids make sibling insertion deterministic, as in applyChanges.
    std::ranges::sort(planned, {}, [](const NewEntity& entity) { return entity.id.value; });
    std::unordered_map<uint64_t, size_t> indices;
    for (size_t index = 0; index < planned.size(); ++index) {
        if (!indices.emplace(planned[index].id.value, index).second) {
            return {.error = "Scene change lists entity " + std::to_string(planned[index].id.value) + " twice"};
        }
    }
    for (const NewEntity& entity : planned) {
        if (entity.parent && !indices.contains(entity.parent.value) && scene.findEntity(entity.parent) == entt::null) {
            return {.error = "Entity " + std::to_string(entity.id.value) + " references missing parent " +
                             std::to_string(entity.parent.value)};
        }
    }
    // Existing entities never have a new entity as parent, so only chains
    // through new entities can cycle.
    std::unordered_set<uint64_t> rooted;
    for (const NewEntity& entity : planned) {
        std::unordered_set<uint64_t> path{entity.id.value};
        for (auto ancestor = indices.find(entity.parent.value);
             ancestor != indices.end() && !rooted.contains(ancestor->first);
             ancestor = indices.find(planned[ancestor->second].parent.value)) {
            if (!path.insert(ancestor->first).second) {
                return {.error = "Scene hierarchy contains a cycle at entity " + std::to_string(ancestor->first)};
            }
        }
        rooted.insert(path.begin(), path.end());
    }
    output = std::move(planned);
    return {.success = true};
}

// Why entity names a collection the scene does not define, or empty.
std::string missingCollectionReference(const Scene& scene, Entity entity) {
    const auto label = [&scene, entity] { return "Entity " + std::to_string(scene.id(entity).value); };
    if (const auto* membership = scene.registry().try_get<CollectionMembershipComponent>(entity)) {
        for (const SceneCollectionId collection : membership->collections) {
            if (scene.collections().find(collection) == nullptr) {
                return label() + " is a member of missing collection " + std::to_string(collection.value);
            }
        }
    }
    if (const auto* instance = scene.registry().try_get<CollectionInstanceComponent>(entity);
        instance != nullptr && scene.collections().find(instance->collection) == nullptr) {
        return label() + " instances missing collection " + std::to_string(instance->collection.value);
    }
    return {};
}

// Creates the planned entities in scene with their components; on failure
// the caller destroys what was created.
SceneDocumentResult createPlanned(std::vector<NewEntity>& planned, AssetManager& assets,
                                  const SceneDocumentOptions& options, Scene& scene) {
    for (NewEntity& entity : planned) {
        if (SceneResult result = scene.createEntityWithId(entity.id, entity.name, entity.entity); !result) {
            return {.error = result.error};
        }
        scene.registry().get<TransformComponent>(entity.entity).local = entity.local;
    }
    std::unordered_map<uint64_t, Entity> created;
    for (const NewEntity& entity : planned) {
        created.emplace(entity.id.value, entity.entity);
    }
    for (const NewEntity& entity : planned) {
        if (entity.parent) {
            const auto parent = created.find(entity.parent.value);
            attachInIdOrder(scene, entity.entity,
                            parent != created.end() ? parent->second : scene.findEntity(entity.parent));
        }
    }
    for (const NewEntity& entity : planned) {
        if (!entity.childOrder.empty()) {
            applyChildOrder(scene, entity.entity, entity.childOrder);
        }
    }
    for (const NewEntity& entity : planned) {
        if (SceneDocumentResult result =
                deserializeComponents(entity.json->at("components"), entity.entity, assets, options, scene);
            !result) {
            return {.error = "Entity " + std::to_string(entity.id.value) + ": " + result.error};
        }
        if (std::string missing = missingCollectionReference(scene, entity.entity); !missing.empty()) {
            return {.error = std::move(missing)};
        }
    }
    return {.success = true};
}
} // namespace

struct ParsedSceneDocument::Impl {
    Json document;
};

SceneDocumentResult SceneDocument::serialize(const Scene& scene, const AssetManager& assets,
                                             const SceneDocumentOptions& options, std::string& output) {
    Json document = {{"version", currentVersion},
                     {"collections", collectionsJson(scene.collections())},
                     {"entities", Json::array()}};
    std::vector<Entity> entities;
    auto view = scene.registry().view<const SceneEntityIdComponent>();
    entities.assign(view.begin(), view.end());
    std::ranges::sort(entities, {}, [&scene](Entity entity) { return scene.id(entity).value; });

    const std::unordered_set<uint64_t> transient = transientEntityIds(scene);
    std::vector<std::string> warnings;
    for (Entity entity : entities) {
        if (transient.contains(scene.id(entity).value)) {
            continue;
        }
        Json serialized;
        if (SceneDocumentResult result = serializeEntity(scene, entity, assets, options, serialized); !result) {
            return result;
        }
        if (!transient.empty()) {
            dropTransientReferences(serialized, componentTypesFor(options), transient, &warnings);
        }
        document["entities"].push_back(std::move(serialized));
    }
    output = document.dump(2) + '\n';
    return {.success = true, .warnings = std::move(warnings)};
}

SceneDocumentResult SceneDocument::deserialize(std::string_view input, AssetManager& assets,
                                               const SceneDocumentOptions& options, Scene& destination) {
    ParsedSceneDocument parsed;
    if (SceneDocumentResult result = parse(input, parsed); !result) {
        return result;
    }
    return commit(parsed, assets, options, destination);
}

SceneDocumentResult SceneDocument::parse(std::string_view input, ParsedSceneDocument& output) {
    try {
        Json document = Json::parse(input);
        if (SceneDocumentResult result = upgradeDocument(document); !result) {
            return result;
        }

        std::unordered_set<uint64_t> ids;
        std::unordered_map<uint64_t, uint64_t> parents;
        for (const Json& serialized : document["entities"]) {
            const uint64_t id = serialized.at("id").get<uint64_t>();
            if (id == 0) {
                return {.error = "Scene contains invalid entity ID 0"};
            }
            if (!ids.insert(id).second) {
                return {.error = "Duplicate scene entity ID " + std::to_string(id)};
            }
            if (!serialized.at("parent").is_null()) {
                parents.emplace(id, serialized.at("parent").get<uint64_t>());
            }
        }
        for (const auto& [child, parent] : parents) {
            if (!ids.contains(parent)) {
                return {.error =
                            "Entity " + std::to_string(child) + " references missing parent " + std::to_string(parent)};
            }
            std::unordered_set<uint64_t> visited{child};
            uint64_t ancestor = parent;
            while (ancestor != 0) {
                if (!visited.insert(ancestor).second) {
                    return {.error = "Scene hierarchy contains a cycle at entity " + std::to_string(ancestor)};
                }
                const auto found = parents.find(ancestor);
                ancestor = found == parents.end() ? 0 : found->second;
            }
        }
        SceneCollections collections;
        if (SceneDocumentResult result = readCollections(document["collections"], collections); !result) {
            return result;
        }

        auto parsed = std::make_shared<ParsedSceneDocument::Impl>();
        parsed->document = std::move(document);
        output.m_impl = std::move(parsed);
        return {.success = true};
    } catch (const nlohmann::json::exception& error) {
        return {.error = "Invalid scene document: " + std::string(error.what())};
    } catch (const std::exception& error) {
        return {.error = "Could not parse scene document: " + std::string(error.what())};
    }
}

SceneDocumentResult SceneDocument::commit(const ParsedSceneDocument& parsed, AssetManager& assets,
                                          const SceneDocumentOptions& options, Scene& destination) {
    if (!parsed.m_impl) {
        return {.error = "Cannot commit an empty parsed scene document"};
    }
    try {
        const Json& document = parsed.m_impl->document;

        Scene loaded;
        SceneCollections collections;
        if (SceneDocumentResult result = readCollections(document["collections"], collections); !result) {
            return result;
        }
        loaded.setCollections(std::move(collections));
        std::unordered_map<uint64_t, Entity> entities;
        // Children are attached in document order, which serialize writes
        // in ascending id order, so siblings are in id order unless their
        // parent lists a "children" order.
        std::vector<std::pair<uint64_t, uint64_t>> parents;
        for (const Json& serialized : document["entities"]) {
            const SceneEntityId id{serialized.at("id").get<uint64_t>()};
            Entity entity = entt::null;
            if (SceneResult result = loaded.createEntityWithId(id, serialized.at("name").get<std::string>(), entity);
                !result) {
                return {.error = result.error};
            }
            if (!readMatrix4(serialized.at("local_transform"),
                             loaded.registry().get<TransformComponent>(entity).local)) {
                return {.error = "Entity " + std::to_string(id.value) + " has an invalid local transform"};
            }
            entities.emplace(id.value, entity);
            if (!serialized.at("parent").is_null()) {
                parents.emplace_back(id.value, serialized.at("parent").get<uint64_t>());
            }
        }

        for (const auto& [child, parent] : parents) {
            loaded.setParent(entities.at(child), entities.at(parent));
        }
        std::vector<uint64_t> order;
        for (const Json& serialized : document["entities"]) {
            if (SceneDocumentResult result = readChildOrder(serialized, order); !result) {
                return result;
            }
            if (!order.empty()) {
                applyChildOrder(loaded, entities.at(serialized.at("id").get<uint64_t>()), order);
            }
        }
        for (const Json& serialized : document["entities"]) {
            const uint64_t id = serialized.at("id").get<uint64_t>();
            if (SceneDocumentResult result =
                    deserializeComponents(serialized.at("components"), entities.at(id), assets, options, loaded);
                !result) {
                return {.error = "Entity " + std::to_string(id) + ": " + result.error};
            }
        }
        if (std::string missing = findMissingCollectionReference(loaded, loaded.collections()); !missing.empty()) {
            return {.error = std::move(missing)};
        }
        loaded.updateWorldTransforms();
        destination.replaceWith(std::move(loaded));
        return {.success = true};
    } catch (const nlohmann::json::exception& error) {
        return {.error = "Invalid parsed scene document: " + std::string(error.what())};
    } catch (const std::exception& error) {
        return {.error = "Could not load scene document: " + std::string(error.what())};
    }
}

SceneDocumentResult SceneDocument::save(const std::filesystem::path& path, const Scene& scene,
                                        const AssetManager& assets, const SceneDocumentOptions& options) {
    std::string serialized;
    SceneDocumentResult serializedResult = serialize(scene, assets, options, serialized);
    if (!serializedResult) {
        return serializedResult;
    }

    std::error_code error;
    if (const std::filesystem::path parent = path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) {
            return {.error = "Could not create scene directory: " + error.message()};
        }
    }
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
        output.flush();
        if (!output) {
            std::filesystem::remove(temporary, error);
            return {.error = "Could not write temporary scene file: " + temporary.string()};
        }
    }
    if (SceneDocumentResult result = replaceFile(temporary, path); !result) {
        std::filesystem::remove(temporary, error);
        return result;
    }
    return {.success = true, .warnings = std::move(serializedResult.warnings)};
}

SceneDocumentResult SceneDocument::load(const std::filesystem::path& path, AssetManager& assets,
                                        const SceneDocumentOptions& options, Scene& destination) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {.error = "Could not open scene document: " + path.string()};
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) {
        return {.error = "Could not read scene document: " + path.string()};
    }
    return deserialize(contents.str(), assets, options, destination);
}

SceneDocumentResult SceneDocument::serializeEntities(const Scene& scene, const AssetManager& assets,
                                                     const SceneDocumentOptions& options,
                                                     std::span<const SceneEntityId> ids,
                                                     std::vector<std::optional<std::string>>& output) {
    std::vector<std::optional<std::string>> serialized;
    serialized.reserve(ids.size());
    const std::unordered_set<uint64_t> transient = transientEntityIds(scene);
    for (const SceneEntityId id : ids) {
        const Entity entity = scene.findEntity(id);
        if (entity == entt::null || transient.contains(id.value)) {
            serialized.emplace_back();
            continue;
        }
        Json json;
        if (SceneDocumentResult result = serializeEntity(scene, entity, assets, options, json); !result) {
            return result;
        }
        if (!transient.empty()) {
            dropTransientReferences(json, componentTypesFor(options), transient, nullptr);
        }
        serialized.emplace_back(json.dump());
    }
    output = std::move(serialized);
    return {.success = true};
}

SceneDocumentResult SceneDocument::diff(std::string_view before, std::string_view after, SceneChanges& changes) {
    try {
        Json beforeDocument;
        Json afterDocument;
        std::map<uint64_t, const Json*> beforeEntities;
        std::map<uint64_t, const Json*> afterEntities;
        if (SceneDocumentResult result = readDocumentEntities(before, beforeDocument, beforeEntities); !result) {
            return result;
        }
        if (SceneDocumentResult result = readDocumentEntities(after, afterDocument, afterEntities); !result) {
            return result;
        }

        std::vector<SceneEntityChange> result;
        auto beforeEntity = beforeEntities.begin();
        auto afterEntity = afterEntities.begin();
        // Both maps are ordered by id, so one merge pass visits every id once.
        while (beforeEntity != beforeEntities.end() || afterEntity != afterEntities.end()) {
            const bool takeBefore = afterEntity == afterEntities.end() ||
                                    (beforeEntity != beforeEntities.end() && beforeEntity->first <= afterEntity->first);
            const bool takeAfter = beforeEntity == beforeEntities.end() ||
                                   (afterEntity != afterEntities.end() && afterEntity->first <= beforeEntity->first);
            const uint64_t id = takeBefore ? beforeEntity->first : afterEntity->first;
            const Json* beforeJson = takeBefore ? beforeEntity->second : nullptr;
            const Json* afterJson = takeAfter ? afterEntity->second : nullptr;
            if (beforeJson == nullptr || afterJson == nullptr || *beforeJson != *afterJson) {
                result.push_back({.id = {id}, .before = compactEntity(beforeJson), .after = compactEntity(afterJson)});
            }
            if (takeBefore) {
                ++beforeEntity;
            }
            if (takeAfter) {
                ++afterEntity;
            }
        }
        changes.entities = std::move(result);
        changes.collections.reset();
        if (const Json& beforeCollections = beforeDocument["collections"];
            beforeCollections != afterDocument["collections"]) {
            changes.collections = SceneCollectionsChange{.before = beforeCollections.dump(),
                                                         .after = afterDocument["collections"].dump()};
        }
        return {.success = true};
    } catch (const nlohmann::json::exception& error) {
        return {.error = "Invalid scene document: " + std::string(error.what())};
    }
}

SceneDocumentResult SceneDocument::applyChanges(const SceneChanges& changes, SceneChangeDirection direction,
                                                AssetManager& assets, const SceneDocumentOptions& options,
                                                Scene& destination) {
    try {
        PatchPlan plan;
        Scene staging;
        std::optional<SceneCollections> collections;
        if (changes.collections) {
            const std::string& target =
                direction == SceneChangeDirection::Forward ? changes.collections->after : changes.collections->before;
            collections.emplace();
            if (SceneDocumentResult result = readCollections(Json::parse(target), *collections); !result) {
                return result;
            }
        }
        std::unordered_set<uint64_t> listed;
        for (const SceneEntityChange& change : changes.entities) {
            if (!change.id) {
                return {.error = "Scene change contains invalid entity ID 0"};
            }
            if (!listed.insert(change.id.value).second) {
                return {.error = "Scene change lists entity " + std::to_string(change.id.value) + " twice"};
            }
            const std::optional<std::string>& target =
                direction == SceneChangeDirection::Forward ? change.after : change.before;
            if (!target) {
                plan.removals.insert(change.id.value);
                continue;
            }
            StagedEntity staged;
            if (SceneDocumentResult result = stageEntity(change.id, *target, assets, options, staging, staged);
                !result) {
                return result;
            }
            plan.upserts.push_back(std::move(staged));
        }
        // Ascending ids make sibling insertion deterministic.
        std::ranges::sort(plan.upserts, {}, [](const StagedEntity& upsert) { return upsert.id.value; });
        for (size_t index = 0; index < plan.upserts.size(); ++index) {
            plan.upsertIndices.emplace(plan.upserts[index].id.value, index);
        }

        auto view = destination.registry().view<const SceneEntityIdComponent>();
        for (const Entity entity : view) {
            plan.existing.emplace(view.get<const SceneEntityIdComponent>(entity).id.value, entity);
        }
        if (SceneDocumentResult result = validateHierarchy(plan, destination); !result) {
            return result;
        }
        const SceneCollections& finalCollections = collections ? *collections : destination.collections();
        if (SceneDocumentResult result = validateCollectionReferences(plan, staging, destination, finalCollections);
            !result) {
            return result;
        }
        applyPlan(plan, staging, options, destination);
        if (collections) {
            destination.setCollections(std::move(*collections));
        }
        return {.success = true};
    } catch (const nlohmann::json::exception& error) {
        return {.error = "Invalid scene change: " + std::string(error.what())};
    } catch (const std::exception& error) {
        return {.error = "Could not apply scene change: " + std::string(error.what())};
    }
}

SceneDocumentResult SceneDocument::createEntities(std::span<const SceneEntityDocument> entities, AssetManager& assets,
                                                  const SceneDocumentOptions& options, Scene& destination) {
    std::vector<NewEntity> planned;
    try {
        if (SceneDocumentResult result = planNewEntities(entities, destination, planned); !result) {
            return result;
        }
    } catch (const nlohmann::json::exception& error) {
        return {.error = "Invalid scene change: " + std::string(error.what())};
    }
    SceneDocumentResult created;
    try {
        created = createPlanned(planned, assets, options, destination);
    } catch (const nlohmann::json::exception& error) {
        created = {.error = "Invalid scene change: " + std::string(error.what())};
    } catch (const std::exception& error) {
        created = {.error = "Could not apply scene change: " + std::string(error.what())};
    }
    if (!created) {
        // A new entity's children are new too, so this never reaches an
        // existing entity.
        for (const NewEntity& entity : std::views::reverse(planned)) {
            if (entity.entity != entt::null && destination.registry().valid(entity.entity)) {
                destination.destroyEntity(entity.entity);
            }
        }
        return created;
    }

    for (const NewEntity& entity : planned) {
        destination.markTransformDirty(entity.entity);
        if (hasPhysics(destination, entity.entity)) {
            destination.markPhysicsDirty(entity.entity);
        }
    }
    if (!planned.empty()) {
        // As applyPlan: revisions are scene-wide, so one bump covers every
        // new entity.
        const Entity first = planned.front().entity;
        destination.markRenderableDirty(first);
        destination.markLightDirty(first);
        destination.markCameraDirty(first);
        destination.markSkinDirty(first);
        destination.markMaterialDirty(first);
    }
    destination.updateWorldTransforms();
    return {.success = true};
}
