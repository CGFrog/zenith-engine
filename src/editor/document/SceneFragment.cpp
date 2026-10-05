#include "SceneFragment.hpp"

#include "engine/assets/Assets.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/EntityReferenceRemap.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using Json = nlohmann::json;

namespace {
// A fragment root's world transform when copied, beside its local_transform
// (relative to the parent it had). Paste removes it.
constexpr const char* worldTransformKey = "world_transform";

// Collection ids are per scene, so a fragment carries the names of the
// collections its entities reference. Pasting keeps a reference when the
// destination has the same collection (same id and name), otherwise moves it
// to a destination collection of the same name, and otherwise drops it:
// pasted entities leave collections the destination lacks (and stop
// instancing them) instead of joining an unrelated one or failing to load.
std::optional<uint64_t> pastedCollection(const SceneCollections& destination,
                                         const std::unordered_map<uint64_t, std::string>& sourceNames, uint64_t id) {
    const auto source = sourceNames.find(id);
    if (source == sourceNames.end()) {
        return std::nullopt;
    }
    if (const SceneCollection* same = destination.find(SceneCollectionId{id});
        same != nullptr && same->name == source->second) {
        return id;
    }
    const auto named = std::ranges::find(destination.all(), source->second, &SceneCollection::name);
    return named == destination.all().end() ? std::nullopt : std::optional<uint64_t>(named->id.value);
}

void remapCollections(const SceneCollections& destination, const std::unordered_map<uint64_t, std::string>& sourceNames,
                      Json& entity) {
    Json& components = entity.at("components");
    if (const auto membership = components.find("collection_membership"); membership != components.end()) {
        std::vector<uint64_t> kept;
        for (const Json& id : membership->at("collections")) {
            if (const std::optional<uint64_t> pasted = pastedCollection(destination, sourceNames, id.get<uint64_t>());
                pasted && std::ranges::find(kept, *pasted) == kept.end()) {
                kept.push_back(*pasted);
            }
        }
        std::ranges::sort(kept);
        if (kept.empty()) {
            components.erase(membership);
        } else {
            (*membership)["collections"] = kept;
        }
    }
    if (const auto instance = components.find("collection_instance"); instance != components.end()) {
        const std::optional<uint64_t> pasted =
            pastedCollection(destination, sourceNames, instance->at("collection").get<uint64_t>());
        if (pasted) {
            (*instance)["collection"] = *pasted;
        } else {
            components.erase(instance);
        }
    }
}

// Adds the collections entity (a document entity object) is a member of or
// instances.
void addCollectionReferences(const Json& entity, std::set<uint64_t>& collections) {
    const Json& components = entity.at("components");
    if (const auto membership = components.find("collection_membership"); membership != components.end()) {
        for (const Json& id : membership->at("collections")) {
            collections.insert(id.get<uint64_t>());
        }
    }
    if (const auto instance = components.find("collection_instance"); instance != components.end()) {
        collections.insert(instance->at("collection").get<uint64_t>());
    }
}

Json matrixJson(const glm::mat4& matrix) {
    Json values = Json::array();
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            values.push_back(matrix[column][row]);
        }
    }
    return values;
}

// entity's world transform from its local chain, whether or not the scene's
// cached world matrices are current.
glm::mat4 worldFromLocals(const Scene& scene, Entity entity) {
    glm::mat4 world(1.0f);
    for (Entity current = entity; current != entt::null;
         current = scene.registry().get<RelationshipComponent>(current).parent) {
        world = scene.registry().get<TransformComponent>(current).local * world;
    }
    return world;
}
} // namespace

SceneDocumentResult SceneFragment::copy(const Scene& scene, const AssetManager& assets,
                                        const SceneDocumentOptions& options, std::span<const SceneEntityId> roots,
                                        std::string& fragment) {
    try {
        const entt::registry& registry = scene.registry();
        std::unordered_set<uint64_t> selected;
        for (SceneEntityId root : roots) {
            selected.emplace(root.value);
        }
        // A selected entity below another selected one is copied with it.
        std::vector<Entity> effectiveRoots;
        for (SceneEntityId root : roots) {
            const Entity entity = scene.findEntity(root);
            if (entity == entt::null || scene.isTransient(entity) ||
                std::ranges::find(effectiveRoots, entity) != effectiveRoots.end()) {
                continue;
            }
            bool nested = false;
            for (Entity parent = registry.get<RelationshipComponent>(entity).parent; parent != entt::null && !nested;
                 parent = registry.get<RelationshipComponent>(parent).parent) {
                nested = selected.contains(scene.id(parent).value);
            }
            if (!nested) {
                effectiveRoots.push_back(entity);
            }
        }
        // Only the copied subtrees are serialized, never the rest of the scene.
        std::vector<SceneEntityId> included;
        std::vector<Entity> pending(effectiveRoots.rbegin(), effectiveRoots.rend());
        while (!pending.empty()) {
            const Entity entity = pending.back();
            pending.pop_back();
            included.push_back(scene.id(entity));
            for (const Entity child : registry.get<RelationshipComponent>(entity).children) {
                if (!scene.isTransient(child)) {
                    pending.push_back(child);
                }
            }
        }
        if (included.empty()) {
            return {.error = "Copy requires at least one valid entity"};
        }
        std::ranges::sort(included, {}, &SceneEntityId::value);
        std::vector<std::optional<std::string>> serialized;
        if (SceneDocumentResult result = SceneDocument::serializeEntities(scene, assets, options, included, serialized);
            !result) {
            return result;
        }
        Json output{{"version", SceneDocument::currentVersion},
                    {"collections", Json::array()},
                    {"entities", Json::array()},
                    {"roots", Json::array()}};
        for (const Entity root : effectiveRoots) {
            output["roots"].push_back(scene.id(root).value);
        }
        std::set<uint64_t> collections;
        for (size_t index = 0; index < included.size(); ++index) {
            if (!serialized[index]) {
                continue;
            }
            Json entity = Json::parse(*serialized[index]);
            const Entity source = scene.findEntity(included[index]);
            if (std::ranges::find(effectiveRoots, source) != effectiveRoots.end()) {
                entity["parent"] = nullptr;
                entity[worldTransformKey] = matrixJson(worldFromLocals(scene, source));
            }
            addCollectionReferences(entity, collections);
            output["entities"].push_back(std::move(entity));
        }
        // Paste matches collections by name, so the fragment carries the
        // names of those its entities reference.
        for (const uint64_t id : collections) {
            if (const SceneCollection* collection = scene.collections().find(SceneCollectionId{id})) {
                output["collections"].push_back({{"id", id}, {"name", collection->name}});
            }
        }
        fragment = output.dump(2) + '\n';
        return {.success = true};
    } catch (const std::exception& exception) {
        return {.error = std::string("Could not serialize scene fragment: ") + exception.what()};
    }
}

SceneDocumentResult SceneFragment::paste(std::string_view fragment, Scene& scene, AssetManager& assets,
                                         const SceneDocumentOptions& options, Entity parent,
                                         std::vector<SceneEntityId>& pastedRoots) {
    try {
        Json source = Json::parse(fragment);
        if (!source.contains("entities") || !source.contains("roots")) {
            return {.error = "Clipboard does not contain a Zenith scene fragment"};
        }
        std::unordered_map<uint64_t, uint64_t> remap;
        for (const Json& entity : source.at("entities")) {
            if (const uint64_t id = entity.at("id").get<uint64_t>(); !remap.contains(id)) {
                remap.emplace(id, scene.reserveEntityId().value);
            }
        }
        // Pasting under a transient parent would make the copies transient
        // (never saved), so it pastes at the top level instead.
        const SceneEntityId parentId =
            parent == entt::null || scene.isTransient(parent) ? SceneEntityId{} : scene.id(parent);
        std::unordered_map<uint64_t, std::string> sourceCollections;
        for (const Json& collection : source.value("collections", Json::array())) {
            sourceCollections.emplace(collection.at("id").get<uint64_t>(), collection.at("name").get<std::string>());
        }
        const ComponentTypeRegistry& types = componentTypesFor(options);
        std::vector<Json> entities;
        entities.reserve(source.at("entities").size());
        for (Json entity : source.at("entities")) {
            remapCollections(scene.collections(), sourceCollections, entity);
            // References between pasted entities follow to the copies;
            // references to anything else keep naming the same entity.
            remapEntityReferences(entity.at("components"), types,
                                  [&remap](uint64_t id, const EntityReferenceSite&) -> std::optional<uint64_t> {
                                      const auto found = remap.find(id);
                                      return found == remap.end() ? std::nullopt : std::optional(found->second);
                                  });
            entity["id"] = remap.at(entity.at("id").get<uint64_t>());
            // A parent's sibling order names its copied children.
            remapChildOrder(entity, [&remap](uint64_t id) -> std::optional<uint64_t> {
                const auto found = remap.find(id);
                return found == remap.end() ? std::nullopt : std::optional(found->second);
            });
            if (entity.at("parent").is_null()) {
                entity["parent"] = parentId ? Json(parentId.value) : Json(nullptr);
                // At the top level a root lands where its original was.
                if (const auto world = entity.find(worldTransformKey); !parentId && world != entity.end()) {
                    entity["local_transform"] = *world;
                }
            } else {
                const uint64_t oldParent = entity.at("parent").get<uint64_t>();
                entity["parent"] = remap.contains(oldParent) ? Json(remap.at(oldParent)) : Json(nullptr);
            }
            entity.erase(worldTransformKey);
            entities.push_back(std::move(entity));
        }
        std::vector<SceneEntityId> roots;
        for (const Json& root : source.at("roots")) {
            roots.push_back({remap.at(root.get<uint64_t>())});
        }
        // The copies are created in place: every other entity keeps its EnTT
        // handle and runtime state.
        std::vector<SceneEntityDocument> documents;
        documents.reserve(entities.size());
        for (const Json& entity : entities) {
            documents.push_back({.id = {entity.at("id").get<uint64_t>()}, .entity = &entity});
        }
        if (SceneDocumentResult result = SceneDocument::createEntities(documents, assets, options, scene); !result) {
            return result;
        }
        pastedRoots = std::move(roots);
        return {.success = true};
    } catch (const std::exception& exception) {
        return {.error = std::string("Could not paste scene fragment: ") + exception.what()};
    }
}

SceneDocumentResult SceneFragment::roots(std::string_view fragment, std::vector<SceneEntityId>& sourceRoots) {
    try {
        const Json source = Json::parse(fragment);
        sourceRoots.clear();
        for (const Json& root : source.at("roots")) {
            sourceRoots.push_back({root.get<uint64_t>()});
        }
        return {.success = true};
    } catch (const std::exception& exception) {
        return {.error = std::string("Not a scene fragment: ") + exception.what()};
    }
}

SceneDocumentResult SceneFragment::replicate(std::string_view fragment, const SceneDocumentOptions& options,
                                             std::span<const std::vector<glm::mat4>> rootWorlds, std::string& output) {
    try {
        const Json source = Json::parse(fragment);
        const Json& roots = source.at("roots");
        uint64_t nextId = 1;
        for (const Json& entity : source.at("entities")) {
            nextId = std::max(nextId, entity.at("id").get<uint64_t>() + 1);
        }
        const ComponentTypeRegistry& types = componentTypesFor(options);
        Json replicated{{"version", source.at("version")},
                        {"collections", source.value("collections", Json::array())},
                        {"entities", Json::array()},
                        {"roots", Json::array()}};
        for (const std::vector<glm::mat4>& worlds : rootWorlds) {
            if (worlds.size() != roots.size()) {
                return {.error = "Each copy needs one transform per fragment root"};
            }
            // Fresh ids for this copy; paste gives them scene ids later.
            std::unordered_map<uint64_t, uint64_t> remap;
            for (const Json& entity : source.at("entities")) {
                remap.emplace(entity.at("id").get<uint64_t>(), nextId++);
            }
            for (Json entity : source.at("entities")) {
                remapEntityReferences(entity.at("components"), types,
                                      [&remap](uint64_t id, const EntityReferenceSite&) -> std::optional<uint64_t> {
                                          const auto found = remap.find(id);
                                          return found == remap.end() ? std::nullopt : std::optional(found->second);
                                      });
                const uint64_t oldId = entity.at("id").get<uint64_t>();
                entity["id"] = remap.at(oldId);
                remapChildOrder(entity, [&remap](uint64_t id) -> std::optional<uint64_t> {
                    const auto found = remap.find(id);
                    return found == remap.end() ? std::nullopt : std::optional(found->second);
                });
                if (!entity.at("parent").is_null()) {
                    entity["parent"] = remap.at(entity.at("parent").get<uint64_t>());
                }
                for (size_t root = 0; root < roots.size(); ++root) {
                    if (roots[root].get<uint64_t>() == oldId) {
                        entity[worldTransformKey] = matrixJson(worlds[root]);
                    }
                }
                replicated["entities"].push_back(std::move(entity));
            }
            for (const Json& root : roots) {
                replicated["roots"].push_back(remap.at(root.get<uint64_t>()));
            }
        }
        output = replicated.dump() + '\n';
        return {.success = true};
    } catch (const std::exception& exception) {
        return {.error = std::string("Could not replicate scene fragment: ") + exception.what()};
    }
}
