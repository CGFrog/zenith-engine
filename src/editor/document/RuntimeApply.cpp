#include "RuntimeApply.hpp"

#include "engine/assets/Assets.hpp"

#include <glm/gtc/type_ptr.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace {
using Json = nlohmann::json;

// A serialized scene's entities by stable id.
SceneDocumentResult serializedEntities(const Scene& scene, const AssetManager& assets,
                                       const SceneDocumentOptions& options, std::map<uint64_t, Json>& entities) {
    std::string text;
    if (SceneDocumentResult result = SceneDocument::serialize(scene, assets, options, text); !result) {
        return result;
    }
    Json document = Json::parse(text, nullptr, false);
    if (document.is_discarded() || !document.contains("entities") || !document["entities"].is_array()) {
        return {.error = "the scene document could not be read back"};
    }
    for (Json& entity : document["entities"]) {
        const uint64_t id = entity.at("id").get<uint64_t>();
        entities.emplace(id, std::move(entity));
    }
    return {.success = true};
}

// The entity's world matrix from its local transforms up the hierarchy, so a
// const scene with stale world transforms still gives the current placement.
glm::mat4 worldMatrix(const Scene& scene, SceneEntityId id) {
    glm::mat4 world(1.0f);
    for (Entity entity = scene.findEntity(id); entity != entt::null;
         entity = scene.registry().get<RelationshipComponent>(entity).parent) {
        world = scene.registry().get<TransformComponent>(entity).local * world;
    }
    return world;
}

// A matrix in the scene document's form: 16 column-major numbers.
Json matrixJson(const glm::mat4& matrix) {
    Json elements = Json::array();
    const float* values = glm::value_ptr(matrix);
    for (size_t index = 0; index < 16; ++index) {
        elements.push_back(values[index]);
    }
    return elements;
}

std::string entityLabel(const Json& entity) {
    return "'" + entity.at("name").get<std::string>() + "' (" + std::to_string(entity.at("id").get<uint64_t>()) + ")";
}

// The edit entity with one component taken from the runtime entity.
SceneDocumentResult withRuntimeComponent(const Json& runtimeEntity, const Json* editEntity, const std::string& key,
                                         Json& after) {
    if (editEntity == nullptr) {
        return {.error = "entity " + entityLabel(runtimeEntity) +
                         " exists only in the running scene; apply the whole entity instead"};
    }
    const Json& runtimeComponents = runtimeEntity.at("components");
    const auto component = runtimeComponents.find(key);
    if (component == runtimeComponents.end()) {
        return {.error =
                    "entity " + entityLabel(runtimeEntity) + " has no '" + key + "' component in the running scene"};
    }
    after = *editEntity;
    after["components"][key] = *component;
    return {.success = true};
}
} // namespace

SceneDocumentResult runtimeChangesForEditScene(const Scene& runtimeScene, const Scene& editScene,
                                               const AssetManager& assets, const SceneDocumentOptions& options,
                                               const RuntimeApplyRequest& request, SceneChanges& changes) {
    changes = {};
    try {
        std::map<uint64_t, Json> runtimeEntities;
        std::map<uint64_t, Json> editEntities;
        if (SceneDocumentResult result = serializedEntities(runtimeScene, assets, options, runtimeEntities); !result) {
            return {.error = "Could not read the running scene: " + result.error};
        }
        if (SceneDocumentResult result = serializedEntities(editScene, assets, options, editEntities); !result) {
            return {.error = "Could not read the edit scene: " + result.error};
        }
        std::set<uint64_t> requested;
        for (const SceneEntityId id : request.entities) {
            if (!runtimeEntities.contains(id.value)) {
                return {.error = "Entity " + std::to_string(id.value) + " is not in the running scene"};
            }
            requested.insert(id.value);
        }
        for (const uint64_t id : requested) {
            const Json& runtimeEntity = runtimeEntities.at(id);
            const auto edit = editEntities.find(id);
            const Json* editEntity = edit == editEntities.end() ? nullptr : &edit->second;
            Json after;
            if (!request.component.empty()) {
                if (SceneDocumentResult result =
                        withRuntimeComponent(runtimeEntity, editEntity, request.component, after);
                    !result) {
                    return result;
                }
            } else {
                after = runtimeEntity;
                const Json& runtimeParent = runtimeEntity.at("parent");
                if (editEntity != nullptr) {
                    // Hierarchy changes made while running stay behind.
                    after["parent"] = editEntity->at("parent");
                } else if (!runtimeParent.is_null() && !editEntities.contains(runtimeParent.get<uint64_t>()) &&
                           !requested.contains(runtimeParent.get<uint64_t>())) {
                    after["parent"] = nullptr;
                }
                if (after["parent"] != runtimeParent) {
                    // Under another parent, the entity keeps its running world
                    // placement rather than its running parent-relative one.
                    const Json& parent = after["parent"];
                    const glm::mat4 parentWorld = parent.is_null() ? glm::mat4(1.0f)
                                                  : requested.contains(parent.get<uint64_t>())
                                                      ? worldMatrix(runtimeScene, SceneEntityId{parent.get<uint64_t>()})
                                                      : worldMatrix(editScene, SceneEntityId{parent.get<uint64_t>()});
                    after["local_transform"] =
                        matrixJson(glm::inverse(parentWorld) * worldMatrix(runtimeScene, SceneEntityId{id}));
                }
            }
            if (editEntity != nullptr && *editEntity == after) {
                continue;
            }
            changes.entities.push_back(
                {.id = {id},
                 .before = editEntity != nullptr ? std::optional<std::string>(editEntity->dump()) : std::nullopt,
                 .after = after.dump()});
        }
        return {.success = true};
    } catch (const nlohmann::json::exception& error) {
        return {.error = "Invalid scene document: " + std::string(error.what())};
    }
}
