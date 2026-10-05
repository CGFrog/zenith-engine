#include "SceneFragment.hpp"

#include "engine/assets/Assets.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/EntityReferenceRemap.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <optional>
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
    std::string serialized;
    if (SceneDocumentResult result = SceneDocument::serialize(scene, assets, options, serialized); !result) {
        return result;
    }
    try {
        Json document = Json::parse(serialized);
        std::unordered_map<uint64_t, Json> entities;
        for (const Json& entity : document.at("entities")) {
            entities.emplace(entity.at("id").get<uint64_t>(), entity);
        }
        std::unordered_set<uint64_t> included;
        std::unordered_set<uint64_t> selected;
        for (SceneEntityId root : roots) {
            selected.emplace(root.value);
        }
        std::vector<uint64_t> effectiveRoots;
        for (SceneEntityId root : roots) {
            auto entity = entities.find(root.value);
            if (entity == entities.end()) {
                continue;
            }
            bool nested = false;
            Json parent = entity->second.at("parent");
            while (!parent.is_null()) {
                const uint64_t parentId = parent.get<uint64_t>();
                if (selected.contains(parentId)) {
                    nested = true;
                    break;
                }
                const auto ancestor = entities.find(parentId);
                parent = ancestor == entities.end() ? Json(nullptr) : ancestor->second.at("parent");
            }
            if (!nested) {
                effectiveRoots.push_back(root.value);
            }
        }
        const auto includeChildren = [&](const auto& self, uint64_t parent) -> void {
            if (!included.emplace(parent).second) {
                return;
            }
            for (const auto& [id, entity] : entities) {
                if (!entity.at("parent").is_null() && entity.at("parent").get<uint64_t>() == parent) {
                    self(self, id);
                }
            }
        };
        for (uint64_t root : effectiveRoots) {
            if (entities.contains(root)) {
                includeChildren(includeChildren, root);
            }
        }
        if (included.empty()) {
            return {.error = "Copy requires at least one valid entity"};
        }
        Json output{{"version", SceneDocument::currentVersion},
                    {"collections", document.at("collections")},
                    {"entities", Json::array()},
                    {"roots", Json::array()}};
        for (uint64_t root : effectiveRoots) {
            if (included.contains(root)) {
                output["roots"].push_back(root);
            }
        }
        for (const Json& entity : document.at("entities")) {
            if (included.contains(entity.at("id").get<uint64_t>())) {
                Json fragmentEntity = entity;
                const uint64_t id = fragmentEntity.at("id").get<uint64_t>();
                if (std::ranges::find(effectiveRoots, id) != effectiveRoots.end()) {
                    fragmentEntity["parent"] = nullptr;
                    if (const Entity root = scene.findEntity(SceneEntityId{id}); root != entt::null) {
                        fragmentEntity[worldTransformKey] = matrixJson(worldFromLocals(scene, root));
                    }
                }
                output["entities"].push_back(std::move(fragmentEntity));
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
        std::string destinationText;
        if (SceneDocumentResult result = SceneDocument::serialize(scene, assets, options, destinationText); !result) {
            return result;
        }
        Json destination = Json::parse(destinationText);
        uint64_t nextId = 1;
        for (const Json& entity : destination.at("entities")) {
            nextId = std::max(nextId, entity.at("id").get<uint64_t>() + 1);
        }
        std::unordered_map<uint64_t, uint64_t> remap;
        for (const Json& entity : source.at("entities")) {
            remap.emplace(entity.at("id").get<uint64_t>(), nextId++);
        }
        // A transient parent is not in the document the paste rebuilds from,
        // so pasting under one pastes at the top level instead.
        const SceneEntityId parentId =
            parent == entt::null || scene.isTransient(parent) ? SceneEntityId{} : scene.id(parent);
        std::unordered_map<uint64_t, std::string> sourceCollections;
        for (const Json& collection : source.value("collections", Json::array())) {
            sourceCollections.emplace(collection.at("id").get<uint64_t>(), collection.at("name").get<std::string>());
        }
        const ComponentTypeRegistry& types = componentTypesFor(options);
        for (Json entity : source.at("entities")) {
            remapCollections(scene.collections(), sourceCollections, entity);
            // References between pasted entities follow to the copies;
            // references to anything else keep naming the same entity.
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
            destination["entities"].push_back(std::move(entity));
        }
        Scene staged;
        const std::string merged = destination.dump();
        if (SceneDocumentResult result = SceneDocument::deserialize(merged, assets, options, staged); !result) {
            return result;
        }
        pastedRoots.clear();
        for (const Json& root : source.at("roots")) {
            pastedRoots.push_back({remap.at(root.get<uint64_t>())});
        }
        scene.replaceWith(std::move(staged));
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
