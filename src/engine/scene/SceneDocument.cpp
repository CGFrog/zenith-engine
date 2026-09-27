#include "SceneDocument.hpp"

#include "../assets/Assets.hpp"

#include <glm/gtc/type_ptr.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <optional>
#include <ranges>
#include <sstream>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace {
using Json = nlohmann::json;

Json vector3(const glm::vec3& value) {
    return Json::array({value.x, value.y, value.z});
}

Json matrix4(const glm::mat4& value) {
    Json result = Json::array();
    const float* elements = glm::value_ptr(value);
    for (size_t index = 0; index < 16; ++index) {
        result.push_back(elements[index]);
    }
    return result;
}

bool readVector3(const Json& json, glm::vec3& value) {
    if (!json.is_array() || json.size() != 3) {
        return false;
    }
    value = {json[0].get<float>(), json[1].get<float>(), json[2].get<float>()};
    return true;
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

SceneDocumentResult projectRelativePath(const std::filesystem::path& source, const SceneDocumentOptions& options,
                                        std::string& output) {
    std::error_code error;
    const std::filesystem::path root = std::filesystem::weakly_canonical(options.assetRoot, error);
    if (error) {
        return {.error = "Could not resolve asset root: " + error.message()};
    }
    const std::filesystem::path canonicalSource = std::filesystem::weakly_canonical(source, error);
    if (error) {
        return {.error = "Could not resolve asset path " + source.string() + ": " + error.message()};
    }
    const std::filesystem::path relative = canonicalSource.lexically_relative(root);
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") {
        return {.error = "Asset is outside the project asset root: " + canonicalSource.string()};
    }
    output = relative.generic_string();
    return {.success = true};
}

SceneDocumentResult resolveAssetPath(std::string_view reference, const SceneDocumentOptions& options,
                                     std::filesystem::path& output) {
    const std::filesystem::path relative(reference);
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") {
        return {.error = "Asset reference must be project-relative: " + std::string(reference)};
    }
    std::error_code error;
    const std::filesystem::path root = std::filesystem::weakly_canonical(options.assetRoot, error);
    if (error) {
        return {.error = "Could not resolve asset root: " + error.message()};
    }
    output = std::filesystem::weakly_canonical(root / relative, error);
    if (error || !std::filesystem::is_regular_file(output, error)) {
        return {.error = "Missing asset: " + (root / relative).string()};
    }
    const std::filesystem::path verifiedRelative = output.lexically_relative(root);
    if (verifiedRelative.empty() || verifiedRelative.is_absolute() || *verifiedRelative.begin() == "..") {
        return {.error = "Asset reference escapes the project asset root: " + std::string(reference)};
    }
    return {.success = true};
}

SceneDocumentResult meshReference(const AssetManager& assets, MeshHandle handle, const SceneDocumentOptions& options,
                                  Json& output) {
    const std::optional<MeshAssetSource> source = assets.meshSource(handle);
    if (!source) {
        return {.error = "Cannot save a mesh component with an unknown runtime handle"};
    }
    std::string relative;
    if (SceneDocumentResult result = projectRelativePath(source->path, options, relative); !result) {
        return result;
    }
    output = {{"path", relative}, {"mesh", source->meshIndex}};
    return {.success = true};
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

    if (const auto* model = registry.try_get<ModelComponent>(entity)) {
        const std::optional<ModelAssetSource> source = assets.modelSource(model->model);
        if (!source) {
            return {.error = "Cannot save a model component with an unknown runtime handle"};
        }
        std::string relative;
        if (SceneDocumentResult result = projectRelativePath(source->path, options, relative); !result) {
            return result;
        }
        components["model"] = {{"path", relative}};
    }
    if (const auto* mesh = registry.try_get<MeshRendererComponent>(entity)) {
        Json reference;
        if (SceneDocumentResult result = meshReference(assets, mesh->mesh, options, reference); !result) {
            return result;
        }
        components["mesh_renderer"] = std::move(reference);
    }
    if (const auto* camera = registry.try_get<CameraComponent>(entity)) {
        components["camera"] = {{"vertical_fov", camera->verticalFovRadians},
                                {"near_plane", camera->nearPlane},
                                {"far_plane", camera->farPlane},
                                {"primary", camera->primary}};
    }
    if (const auto* light = registry.try_get<DirectionalLightComponent>(entity)) {
        components["directional_light"] = {{"direction", vector3(light->direction)},
                                           {"color", vector3(light->color)},
                                           {"intensity", light->intensity},
                                           {"primary", light->primary},
                                           {"shadows",
                                            {{"enabled", light->shadows.enabled},
                                             {"half_extent", light->shadows.halfExtent},
                                             {"focus_distance", light->shadows.focusDistance},
                                             {"depth_range", light->shadows.depthRange},
                                             {"constant_bias", light->shadows.constantBias},
                                             {"slope_bias", light->shadows.slopeBias},
                                             {"normal_bias", light->shadows.normalBias}}}};
    }
    if (const auto* light = registry.try_get<EnvironmentLightComponent>(entity)) {
        components["environment_light"] = {{"sky_color", vector3(light->skyColor)},
                                           {"ground_color", vector3(light->groundColor)},
                                           {"diffuse_intensity", light->diffuseIntensity},
                                           {"specular_intensity", light->specularIntensity},
                                           {"primary", light->primary}};
    }
    if (const auto* collider = registry.try_get<ColliderComponent>(entity)) {
        Json value;
        std::visit(
            [&](const auto& shape) {
                using Shape = std::decay_t<decltype(shape)>;
                if constexpr (std::is_same_v<Shape, BoxCollider>) {
                    value = {{"type", "box"}, {"half_extents", vector3(shape.halfExtents)}};
                } else if constexpr (std::is_same_v<Shape, SphereCollider>) {
                    value = {{"type", "sphere"}, {"radius", shape.radius}};
                } else if constexpr (std::is_same_v<Shape, CapsuleCollider>) {
                    value = {{"type", "capsule"}, {"half_height", shape.halfHeight}, {"radius", shape.radius}};
                }
            },
            collider->shape);
        if (const auto* triangleMesh = std::get_if<TriangleMeshCollider>(&collider->shape)) {
            Json reference;
            if (SceneDocumentResult result = meshReference(assets, triangleMesh->mesh, options, reference); !result) {
                return result;
            }
            value = {{"type", "triangle_mesh"}, {"asset", std::move(reference)}};
        }
        components["collider"] = std::move(value);
    }
    if (const auto* body = registry.try_get<RigidBodyComponent>(entity)) {
        components["rigid_body"] = {{"motion_type", static_cast<uint8_t>(body->motionType)},
                                    {"layer", static_cast<uint16_t>(body->layer)},
                                    {"mass", body->mass},
                                    {"friction", body->friction},
                                    {"restitution", body->restitution},
                                    {"linear_damping", body->linearDamping},
                                    {"angular_damping", body->angularDamping},
                                    {"gravity_factor", body->gravityFactor},
                                    {"sensor", body->sensor},
                                    {"allow_sleeping", body->allowSleeping},
                                    {"continuous_collision", body->continuousCollision},
                                    {"lock_rotation", body->lockRotation}};
    }
    output["components"] = std::move(components);
    return {.success = true};
}

SceneDocumentResult readMesh(const Json& json, AssetManager& assets, const SceneDocumentOptions& options,
                             MeshHandle& output) {
    std::filesystem::path path;
    if (SceneDocumentResult result = resolveAssetPath(json.at("path").get<std::string>(), options, path); !result) {
        return result;
    }
    const AssetLoadResult<MeshHandle> loaded = assets.loadMesh(path, json.at("mesh").get<uint32_t>());
    if (!loaded) {
        return {.error = loaded.error};
    }
    output = loaded.handle;
    return {.success = true};
}

SceneDocumentResult deserializeComponents(const Json& components, Entity entity, AssetManager& assets,
                                          const SceneDocumentOptions& options, Scene& scene) {
    entt::registry& registry = scene.registry();
    if (const auto found = components.find("model"); found != components.end()) {
        std::filesystem::path path;
        if (SceneDocumentResult result = resolveAssetPath(found->at("path").get<std::string>(), options, path);
            !result) {
            return result;
        }
        const AssetLoadResult<ModelHandle> loaded = assets.loadModel(path);
        if (!loaded) {
            return {.error = loaded.error};
        }
        registry.emplace<ModelComponent>(entity, loaded.handle);
    }
    if (const auto found = components.find("mesh_renderer"); found != components.end()) {
        MeshHandle mesh;
        if (SceneDocumentResult result = readMesh(*found, assets, options, mesh); !result) {
            return result;
        }
        registry.emplace<MeshRendererComponent>(entity, mesh);
    }
    if (const auto found = components.find("camera"); found != components.end()) {
        registry.emplace<CameraComponent>(entity, found->at("vertical_fov").get<float>(),
                                          found->at("near_plane").get<float>(), found->at("far_plane").get<float>(),
                                          found->at("primary").get<bool>());
    }
    if (const auto found = components.find("directional_light"); found != components.end()) {
        DirectionalLightComponent light;
        if (!readVector3(found->at("direction"), light.direction) || !readVector3(found->at("color"), light.color)) {
            return {.error = "Directional light vectors must each contain three numbers"};
        }
        light.intensity = found->at("intensity").get<float>();
        light.primary = found->at("primary").get<bool>();
        const Json& shadows = found->at("shadows");
        light.shadows = {.enabled = shadows.at("enabled").get<bool>(),
                         .halfExtent = shadows.at("half_extent").get<float>(),
                         .focusDistance = shadows.at("focus_distance").get<float>(),
                         .depthRange = shadows.at("depth_range").get<float>(),
                         .constantBias = shadows.at("constant_bias").get<float>(),
                         .slopeBias = shadows.at("slope_bias").get<float>(),
                         .normalBias = shadows.at("normal_bias").get<float>()};
        registry.emplace<DirectionalLightComponent>(entity, light);
    }
    if (const auto found = components.find("environment_light"); found != components.end()) {
        EnvironmentLightComponent light;
        if (!readVector3(found->at("sky_color"), light.skyColor) ||
            !readVector3(found->at("ground_color"), light.groundColor)) {
            return {.error = "Environment light colors must each contain three numbers"};
        }
        light.diffuseIntensity = found->at("diffuse_intensity").get<float>();
        light.specularIntensity = found->at("specular_intensity").get<float>();
        light.primary = found->at("primary").get<bool>();
        registry.emplace<EnvironmentLightComponent>(entity, light);
    }
    if (const auto found = components.find("collider"); found != components.end()) {
        ColliderComponent collider;
        const std::string type = found->at("type").get<std::string>();
        if (type == "box") {
            BoxCollider box;
            if (!readVector3(found->at("half_extents"), box.halfExtents)) {
                return {.error = "Box collider half_extents must contain three numbers"};
            }
            collider.shape = box;
        } else if (type == "sphere") {
            collider.shape = SphereCollider{found->at("radius").get<float>()};
        } else if (type == "capsule") {
            collider.shape = CapsuleCollider{found->at("half_height").get<float>(), found->at("radius").get<float>()};
        } else if (type == "triangle_mesh") {
            MeshHandle mesh;
            if (SceneDocumentResult result = readMesh(found->at("asset"), assets, options, mesh); !result) {
                return result;
            }
            collider.shape = TriangleMeshCollider{mesh};
        } else {
            return {.error = "Unsupported collider type: " + type};
        }
        registry.emplace<ColliderComponent>(entity, collider);
    }
    if (const auto found = components.find("rigid_body"); found != components.end()) {
        const uint8_t motionType = found->at("motion_type").get<uint8_t>();
        const uint16_t layer = found->at("layer").get<uint16_t>();
        if (motionType > static_cast<uint8_t>(PhysicsMotionType::Dynamic) ||
            layer > static_cast<uint16_t>(PhysicsLayer::Player)) {
            return {.error = "Rigid body contains an unsupported enum value"};
        }
        RigidBodyComponent body;
        body.motionType = static_cast<PhysicsMotionType>(motionType);
        body.layer = static_cast<PhysicsLayer>(layer);
        body.mass = found->at("mass").get<float>();
        body.friction = found->at("friction").get<float>();
        body.restitution = found->at("restitution").get<float>();
        body.linearDamping = found->at("linear_damping").get<float>();
        body.angularDamping = found->at("angular_damping").get<float>();
        body.gravityFactor = found->at("gravity_factor").get<float>();
        body.sensor = found->at("sensor").get<bool>();
        body.allowSleeping = found->at("allow_sleeping").get<bool>();
        body.continuousCollision = found->at("continuous_collision").get<bool>();
        body.lockRotation = found->at("lock_rotation").get<bool>();
        registry.emplace<RigidBodyComponent>(entity, body);
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
    try {
        const Json document = Json::parse(input);
        if (!document.is_object() || !document.contains("version") || !document.contains("entities") ||
            !document["entities"].is_array()) {
            return {.error = "Scene document must contain version and entities fields"};
        }
        const uint32_t version = document["version"].get<uint32_t>();
        if (version != currentVersion) {
            return {.error = "Unsupported scene document version " + std::to_string(version)};
        }

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
            if (!entities.contains(parent)) {
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
        return {.error = "Invalid scene document: " + std::string(error.what())};
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
