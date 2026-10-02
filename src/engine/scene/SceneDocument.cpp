#include "SceneDocument.hpp"

#include "../assets/Assets.hpp"
#include "ComponentTypes.hpp"

#include <glm/gtc/type_ptr.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <ranges>
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
} // namespace

struct ParsedSceneDocument::Impl {
    Json document;
};

SceneDocumentResult SceneDocument::serialize(const Scene& scene, const AssetManager& assets,
                                             const SceneDocumentOptions& options, std::string& output) {
    Json document = {{"version", currentVersion}, {"entities", Json::array()}};
    std::vector<Entity> entities;
    auto view = scene.registry().view<const SceneEntityIdComponent>();
    entities.assign(view.begin(), view.end());
    std::ranges::sort(entities, {}, [&scene](Entity entity) { return scene.id(entity).value; });

    for (Entity entity : entities) {
        Json serialized;
        if (SceneDocumentResult result = serializeEntity(scene, entity, assets, options, serialized); !result) {
            return result;
        }
        document["entities"].push_back(std::move(serialized));
    }
    output = document.dump(2) + '\n';
    return {.success = true};
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
        if (!document.is_object() || !document.contains("version") || !document.contains("entities") ||
            !document["entities"].is_array()) {
            return {.error = "Scene document must contain version and entities fields"};
        }
        const uint32_t version = document["version"].get<uint32_t>();
        if (version != currentVersion) {
            return {.error = "Unsupported scene document version " + std::to_string(version)};
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
        std::unordered_map<uint64_t, Entity> entities;
        std::unordered_map<uint64_t, uint64_t> parents;
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
                parents.emplace(id.value, serialized.at("parent").get<uint64_t>());
            }
        }

        for (const auto& [child, parent] : parents) {
            loaded.setParent(entities.at(child), entities.at(parent));
        }
        for (const Json& serialized : document["entities"]) {
            const uint64_t id = serialized.at("id").get<uint64_t>();
            if (SceneDocumentResult result =
                    deserializeComponents(serialized.at("components"), entities.at(id), assets, options, loaded);
                !result) {
                return {.error = "Entity " + std::to_string(id) + ": " + result.error};
            }
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
    if (SceneDocumentResult result = serialize(scene, assets, options, serialized); !result) {
        return result;
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
    return {.success = true};
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
