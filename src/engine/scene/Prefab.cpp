#include "Prefab.hpp"

#include <glm/gtc/type_ptr.hpp>
#include <nlohmann/json.hpp>

#include <fstream>
#include <functional>
#include <sstream>
#include <unordered_set>

namespace {
using Json = nlohmann::json;

Json matrix(const glm::mat4& value) {
    Json result = Json::array();
    for (size_t index = 0; index < 16; ++index) {
        result.push_back(glm::value_ptr(value)[index]);
    }
    return result;
}

bool readMatrix(const Json& value, glm::mat4& output) {
    if (!value.is_array() || value.size() != 16) {
        return false;
    }
    for (size_t index = 0; index < 16; ++index) {
        glm::value_ptr(output)[index] = value[index].get<float>();
    }
    return true;
}

uint64_t remap(uint64_t instance, uint64_t local) {
    uint64_t value = instance ^ (local + 0x9e3779b97f4a7c15ULL + (instance << 6U) + (instance >> 2U));
    return value == 0 ? 1 : value;
}

bool hasHierarchyCycle(const std::vector<PrefabEntity>& entities) {
    std::unordered_map<uint64_t, std::optional<uint64_t>> parents;
    for (const PrefabEntity& entity : entities) {
        parents.emplace(entity.id.value, entity.parent ? std::optional(entity.parent->value) : std::nullopt);
    }
    std::unordered_set<uint64_t> visiting;
    std::unordered_set<uint64_t> visited;
    std::function<bool(uint64_t)> visit = [&](uint64_t id) {
        if (visited.contains(id)) {
            return false;
        }
        if (!visiting.insert(id).second) {
            return true;
        }
        const auto parent = parents.at(id);
        if (parent && visit(*parent)) {
            return true;
        }
        visiting.erase(id);
        visited.insert(id);
        return false;
    };
    for (const PrefabEntity& entity : entities) {
        if (visit(entity.id.value)) {
            return true;
        }
    }
    return false;
}
} // namespace

PrefabResult serializePrefab(const PrefabDocument& prefab, std::string& output) {
    Json document = {
        {"version", PrefabDocument::currentVersion}, {"entities", Json::array()}, {"nested", Json::array()}};
    for (const PrefabEntity& entity : prefab.entities) {
        Json encoded = {{"id", entity.id.value},
                        {"name", entity.name},
                        {"parent", entity.parent ? Json(entity.parent->value) : Json(nullptr)},
                        {"localTransform", matrix(entity.localTransform)}};
        if (entity.modelAsset) {
            if (entity.modelAsset->empty() || entity.modelAsset->is_absolute()) {
                return {.error = "Prefab model assets must be project-relative"};
            }
            encoded["modelAsset"] = entity.modelAsset->generic_string();
        }
        Json components = Json::object();
        for (const auto& [type, payload] : entity.components) {
            try {
                components[type] = Json::parse(payload);
            } catch (const Json::exception&) {
                return {.error = "Prefab component contains invalid JSON: " + type};
            }
        }
        encoded["components"] = std::move(components);
        document["entities"].push_back(std::move(encoded));
    }
    for (const NestedPrefab& nested : prefab.nested) {
        Json overrides = Json::array();
        for (const PrefabOverride& overrideValue : nested.overrides) {
            Json value;
            try {
                value = Json::parse(overrideValue.jsonValue);
            } catch (const Json::exception&) {
                return {.error = "Prefab override contains invalid JSON"};
            }
            overrides.push_back({{"entity", overrideValue.entity.value},
                                 {"property", overrideValue.property},
                                 {"value", std::move(value)}});
        }
        document["nested"].push_back({{"id", nested.id},
                                      {"source", nested.source.generic_string()},
                                      {"parent", nested.parent ? Json(nested.parent->value) : Json(nullptr)},
                                      {"overrides", std::move(overrides)}});
    }
    output = document.dump(2);
    return {.success = true};
}

PrefabResult deserializePrefab(std::string_view input, PrefabDocument& prefab) {
    try {
        const Json document = Json::parse(input);
        if (!document.is_object() || document.value("version", 0) != PrefabDocument::currentVersion ||
            !document.contains("entities") || !document["entities"].is_array()) {
            return {.error = "Unsupported or malformed prefab document"};
        }
        PrefabDocument loaded;
        std::unordered_set<uint64_t> ids;
        for (const Json& item : document["entities"]) {
            PrefabEntity entity;
            entity.id.value = item.value("id", uint64_t{0});
            entity.name = item.value("name", std::string{});
            if (!entity.id || !ids.insert(entity.id.value).second) {
                return {.error = "Prefab entity IDs must be non-zero and unique"};
            }
            if (!item.at("parent").is_null()) {
                entity.parent = PrefabEntityId{item.at("parent").get<uint64_t>()};
            }
            if (!readMatrix(item.at("localTransform"), entity.localTransform)) {
                return {.error = "Prefab localTransform must contain 16 numbers"};
            }
            if (const auto model = item.find("modelAsset"); model != item.end()) {
                if (!model->is_string()) {
                    return {.error = "Prefab modelAsset must be a relative path"};
                }
                entity.modelAsset = model->get<std::string>();
                if (entity.modelAsset->empty() || entity.modelAsset->is_absolute() ||
                    *entity.modelAsset->lexically_normal().begin() == "..") {
                    return {.error = "Prefab modelAsset must stay within the project root"};
                }
            }
            if (const auto components = item.find("components"); components != item.end()) {
                if (!components->is_object()) {
                    return {.error = "Prefab components must be an object"};
                }
                for (auto component = components->begin(); component != components->end(); ++component) {
                    entity.components.emplace_back(component.key(), component.value().dump());
                }
                std::ranges::sort(entity.components,
                                  [](const auto& left, const auto& right) { return left.first < right.first; });
            }
            loaded.entities.push_back(std::move(entity));
        }
        for (const PrefabEntity& entity : loaded.entities) {
            if (entity.parent && !ids.contains(entity.parent->value)) {
                return {.error = "Prefab entity refers to a missing parent"};
            }
        }
        if (hasHierarchyCycle(loaded.entities)) {
            return {.error = "Prefab entity hierarchy contains a cycle"};
        }
        std::unordered_set<uint64_t> nestedIds;
        for (const Json& item : document.value("nested", Json::array())) {
            NestedPrefab nested;
            nested.id = item.value("id", uint64_t{0});
            nested.source = item.value("source", std::string{});
            if (nested.id == 0 || !nestedIds.insert(nested.id).second || nested.source.empty() ||
                nested.source.is_absolute()) {
                return {.error = "Nested prefab requires a unique non-zero ID and relative source"};
            }
            if (!item.at("parent").is_null()) {
                nested.parent = PrefabEntityId{item.at("parent").get<uint64_t>()};
                if (!ids.contains(nested.parent->value)) {
                    return {.error = "Nested prefab refers to a missing parent"};
                }
            }
            for (const Json& overrideJson : item.value("overrides", Json::array())) {
                PrefabOverride overrideValue{PrefabEntityId{overrideJson.value("entity", uint64_t{0})},
                                             overrideJson.value("property", std::string{}),
                                             overrideJson.at("value").dump()};
                if (!overrideValue.entity || overrideValue.property.empty()) {
                    return {.error = "Prefab overrides require an entity ID and property"};
                }
                nested.overrides.push_back(std::move(overrideValue));
            }
            loaded.nested.push_back(std::move(nested));
        }
        prefab = std::move(loaded);
        return {.success = true};
    } catch (const Json::exception& error) {
        return {.error = std::string("Could not parse prefab: ") + error.what()};
    }
}

PrefabResult savePrefab(const std::filesystem::path& path, const PrefabDocument& prefab) {
    std::string serialized;
    if (PrefabResult result = serializePrefab(prefab, serialized); !result) {
        return result;
    }
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        return {.error = "Could not write prefab: " + path.string()};
    }
    output << serialized << '\n';
    return output ? PrefabResult{.success = true} : PrefabResult{.error = "Could not finish writing prefab"};
}

PrefabResult loadPrefab(const std::filesystem::path& path, PrefabDocument& prefab) {
    std::ifstream input(path);
    if (!input) {
        return {.error = "Could not open prefab: " + path.string()};
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    return deserializePrefab(contents.str(), prefab);
}

PrefabSystem::PrefabSystem(std::filesystem::path prefabRoot, AssetManager* assets)
    : m_root(std::filesystem::absolute(std::move(prefabRoot)).lexically_normal())
    , m_assets(assets) {}

PrefabResult PrefabSystem::resolve(const std::filesystem::path& reference, std::filesystem::path& path) const {
    if (reference.empty() || reference.is_absolute()) {
        return {.error = "Prefab source must be project-relative"};
    }
    path = (m_root / reference).lexically_normal();
    const std::filesystem::path relative = path.lexically_relative(m_root);
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") {
        return {.error = "Prefab source escapes the configured root"};
    }
    return {.success = true};
}

PrefabInstanceResult PrefabSystem::instantiate(const std::filesystem::path& reference, PrefabInstanceId instanceId,
                                               Scene& scene, Entity parent, std::span<const PrefabOverride> overrides) {
    std::filesystem::path path;
    if (PrefabResult result = resolve(reference, path); !result) {
        return {.error = result.error};
    }
    // Resolve the complete nested graph in isolation first. A missing source,
    // cycle, malformed override, or duplicate ID therefore cannot partially
    // mutate the destination scene.
    Scene validationScene;
    std::vector<std::filesystem::path> validationStack;
    if (PrefabInstanceResult validation =
            instantiateRecursive(path, instanceId, validationScene, entt::null, overrides, validationStack);
        !validation) {
        return validation;
    }
    std::vector<std::filesystem::path> stack;
    return instantiateRecursive(path, instanceId, scene, parent, overrides, stack);
}

PrefabInstanceResult PrefabSystem::refresh(const std::filesystem::path& reference, PrefabInstanceId instanceId,
                                           Scene& scene, Entity existingRoot, Entity parent,
                                           std::span<const PrefabOverride> overrides) {
    Scene validationScene;
    PrefabInstanceResult validation = instantiate(reference, instanceId, validationScene, entt::null, overrides);
    if (!validation) {
        return validation;
    }
    if (existingRoot != entt::null && scene.registry().valid(existingRoot)) {
        scene.destroyEntity(existingRoot);
    }
    return instantiate(reference, instanceId, scene, parent, overrides);
}

PrefabInstanceResult PrefabSystem::instantiateRecursive(const std::filesystem::path& path, PrefabInstanceId instanceId,
                                                        Scene& scene, Entity parent,
                                                        std::span<const PrefabOverride> overrides,
                                                        std::vector<std::filesystem::path>& stack) {
    if (std::ranges::find(stack, path) != stack.end()) {
        return {.error = "Nested prefab cycle detected at: " + path.string()};
    }
    PrefabDocument document;
    if (PrefabResult result = loadPrefab(path, document); !result) {
        return {.error = result.error};
    }
    stack.push_back(path);
    PrefabInstanceResult result{.success = true};
    std::unordered_map<uint64_t, Entity> entities;
    for (const PrefabEntity& source : document.entities) {
        Entity entity = entt::null;
        uint64_t stableId = remap(instanceId.value, source.id.value);
        while (scene.findEntity({stableId}) != entt::null) {
            ++stableId;
        }
        if (SceneResult created = scene.createEntityWithId({stableId}, source.name, entity); !created) {
            result.success = false;
            result.error = created.error;
            stack.pop_back();
            return result;
        }
        scene.registry().get<TransformComponent>(entity).local = source.localTransform;
        for (const auto& [type, payload] : source.components) {
            try {
                const Json component = Json::parse(payload);
                if (type == "pointLight") {
                    PointLightComponent light;
                    const auto color = component.at("color");
                    if (!color.is_array() || color.size() != 3) {
                        result.success = false;
                        result.error = "Prefab pointLight color must have three values";
                        stack.pop_back();
                        return result;
                    }
                    light.color = {color[0].get<float>(), color[1].get<float>(), color[2].get<float>()};
                    light.intensity = component.at("intensity").get<float>();
                    light.range = component.at("range").get<float>();
                    if (light.range <= 0.0f) {
                        result.success = false;
                        result.error = "Prefab pointLight range must be positive";
                        stack.pop_back();
                        return result;
                    }
                    scene.registry().emplace_or_replace<PointLightComponent>(entity, light);
                } else if (type == "camera") {
                    CameraComponent camera;
                    camera.verticalFovRadians = component.at("verticalFov").get<float>();
                    camera.nearPlane = component.at("nearPlane").get<float>();
                    camera.farPlane = component.at("farPlane").get<float>();
                    camera.primary = component.value("primary", false);
                    scene.registry().emplace_or_replace<CameraComponent>(entity, camera);
                } else {
                    result.success = false;
                    result.error = "Unsupported prefab component: " + type;
                    stack.pop_back();
                    return result;
                }
            } catch (const Json::exception& error) {
                result.success = false;
                result.error = "Invalid prefab component '" + type + "': " + error.what();
                stack.pop_back();
                return result;
            }
        }
        if (source.modelAsset) {
            if (m_assets == nullptr) {
                result.success = false;
                result.error = "Prefab contains a model asset but no asset manager was supplied";
                stack.pop_back();
                return result;
            }
            const AssetLoadResult<ModelHandle> loaded = m_assets->loadModel(m_root / *source.modelAsset);
            if (!loaded) {
                result.success = false;
                result.error = "Could not load prefab model asset: " + loaded.error;
                stack.pop_back();
                return result;
            }
            if (SceneResult instantiated = scene.instantiateModel(entity, loaded.handle, *m_assets); !instantiated) {
                result.success = false;
                result.error = instantiated.error;
                stack.pop_back();
                return result;
            }
        }
        entities.emplace(source.id.value, entity);
        result.entities.push_back(entity);
        if (result.root == entt::null && !source.parent) {
            result.root = entity;
        }
    }
    for (const PrefabEntity& source : document.entities) {
        const Entity entity = entities.at(source.id.value);
        scene.setParent(entity, source.parent ? entities.at(source.parent->value) : parent);
    }
    const auto applyOverrides = [&](std::span<const PrefabOverride> values) {
        for (const PrefabOverride& overrideValue : values) {
            const auto found = entities.find(overrideValue.entity.value);
            if (found == entities.end()) {
                result.diagnostics.push_back("Orphaned override for entity " +
                                             std::to_string(overrideValue.entity.value));
                continue;
            }
            try {
                const Json value = Json::parse(overrideValue.jsonValue);
                if (overrideValue.property == "name" && value.is_string()) {
                    scene.registry().get<NameComponent>(found->second).name = value.get<std::string>();
                } else if (overrideValue.property == "localTransform" &&
                           readMatrix(value, scene.registry().get<TransformComponent>(found->second).local)) {
                    scene.markTransformDirty(found->second);
                } else if (overrideValue.property == "component.pointLight") {
                    if (value.is_null()) {
                        scene.registry().remove<PointLightComponent>(found->second);
                    } else {
                        const Json color = value.at("color");
                        PointLightComponent light{{color[0].get<float>(), color[1].get<float>(), color[2].get<float>()},
                                                  value.at("intensity").get<float>(),
                                                  value.at("range").get<float>()};
                        if (light.range <= 0.0f) {
                            result.diagnostics.push_back("Invalid pointLight component override");
                        } else {
                            scene.registry().emplace_or_replace<PointLightComponent>(found->second, light);
                        }
                    }
                } else if (overrideValue.property == "component.camera") {
                    if (value.is_null()) {
                        scene.registry().remove<CameraComponent>(found->second);
                    } else {
                        CameraComponent camera{value.at("verticalFov").get<float>(), value.at("nearPlane").get<float>(),
                                               value.at("farPlane").get<float>(), value.value("primary", false)};
                        scene.registry().emplace_or_replace<CameraComponent>(found->second, camera);
                    }
                } else {
                    result.diagnostics.push_back("Unsupported prefab override property: " + overrideValue.property);
                }
            } catch (const Json::exception&) {
                result.diagnostics.push_back("Invalid JSON in prefab override: " + overrideValue.property);
            }
        }
    };
    applyOverrides(overrides);
    for (const NestedPrefab& nested : document.nested) {
        std::filesystem::path nestedPath;
        const std::filesystem::path nestedReference = path.parent_path().lexically_relative(m_root) / nested.source;
        if (PrefabResult resolved = resolve(nestedReference, nestedPath); !resolved) {
            result.success = false;
            result.error = resolved.error;
            stack.pop_back();
            return result;
        }
        const Entity nestedParent = nested.parent ? entities.at(nested.parent->value) : parent;
        PrefabInstanceResult child = instantiateRecursive(nestedPath, {remap(instanceId.value, nested.id)}, scene,
                                                          nestedParent, nested.overrides, stack);
        if (!child) {
            result.success = false;
            result.error = child.error;
            stack.pop_back();
            return result;
        }
        result.entities.insert(result.entities.end(), child.entities.begin(), child.entities.end());
        result.diagnostics.insert(result.diagnostics.end(), child.diagnostics.begin(), child.diagnostics.end());
    }
    stack.pop_back();
    scene.updateWorldTransforms();
    return result;
}
