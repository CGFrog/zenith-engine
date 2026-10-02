#include "SceneDocument.hpp"

#include "../assets/Assets.hpp"
#include "../assets/IrradianceVolumeAsset.hpp"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
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

// Shadow fields are optional so documents written before local-light shadows
// still load with shadows off.
SceneDocumentResult readLocalLightShadows(const Json& light, bool& castsShadows, uint32_t& resolution,
                                          int32_t& priority) {
    constexpr uint32_t maximumShadowResolution = 16384;
    castsShadows = light.value("casts_shadows", false);
    priority = light.value("shadow_priority", 0);
    const auto found = light.find("shadow_resolution");
    if (found == light.end()) {
        resolution = 0;
        return {.success = true};
    }
    if (!found->is_number_unsigned() || found->get<uint64_t>() > maximumShadowResolution) {
        return {.error = "Local light shadow resolution must be a whole number of at most 16384 texels"};
    }
    resolution = found->get<uint32_t>();
    return {.success = true};
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

// The project-relative path of an optional texture: the loaded handle's source
// when there is one, else the authored path (which may name a missing file).
SceneDocumentResult textureReference(const AssetManager& assets, TextureHandle handle, const std::string& authored,
                                     const SceneDocumentOptions& options, std::string& output) {
    output = authored;
    if (!handle) {
        return {.success = true};
    }
    const auto source = assets.textureSource(handle);
    if (!source) {
        return {.error = "Cannot save a texture reference with an unknown runtime handle"};
    }
    return projectRelativePath(source->path, options, output);
}

// Loads the texture a component names by project-relative path; an empty path
// leaves the handle empty.
SceneDocumentResult loadTextureReference(const std::string& reference, AssetManager& assets,
                                         const SceneDocumentOptions& options, TextureHandle& output) {
    output = {};
    if (reference.empty()) {
        return {.success = true};
    }
    std::filesystem::path path;
    if (SceneDocumentResult result = resolveAssetPath(reference, options, path); !result) {
        return result;
    }
    const AssetLoadResult<TextureHandle> loaded = assets.loadTexture(path);
    if (!loaded) {
        return {.error = loaded.error};
    }
    output = loaded.handle;
    return {.success = true};
}

Json decalDocument(const DecalComponent& decal, std::string albedoAsset, std::string normalAsset) {
    return {{"half_extents", vector3(decal.halfExtents)},
            {"albedo_asset", std::move(albedoAsset)},
            {"normal_asset", std::move(normalAsset)},
            {"tint", vector3(decal.tint)},
            {"opacity", decal.opacity},
            {"base_color_strength", decal.baseColorStrength},
            {"normal_strength", decal.normalStrength},
            {"roughness", decal.roughness},
            {"roughness_strength", decal.roughnessStrength},
            {"metallic", decal.metallic},
            {"metallic_strength", decal.metallicStrength},
            {"angle_fade_start", decal.angleFadeStartRadians},
            {"angle_fade_end", decal.angleFadeEndRadians},
            {"sort_order", decal.sortOrder}};
}

// Every field but half_extents is optional and defaults as in DecalComponent.
SceneDocumentResult readDecal(const Json& json, AssetManager& assets, const SceneDocumentOptions& options,
                              DecalComponent& decal) {
    if (!readVector3(json.at("half_extents"), decal.halfExtents)) {
        return {.error = "Decal half extents must contain three numbers"};
    }
    if (const auto tint = json.find("tint"); tint != json.end() && !readVector3(*tint, decal.tint)) {
        return {.error = "Decal tint must contain three numbers"};
    }
    decal.albedoAsset = json.value("albedo_asset", std::string{});
    decal.normalAsset = json.value("normal_asset", std::string{});
    decal.opacity = json.value("opacity", decal.opacity);
    decal.baseColorStrength = json.value("base_color_strength", decal.baseColorStrength);
    decal.normalStrength = json.value("normal_strength", decal.normalStrength);
    decal.roughness = json.value("roughness", decal.roughness);
    decal.roughnessStrength = json.value("roughness_strength", decal.roughnessStrength);
    decal.metallic = json.value("metallic", decal.metallic);
    decal.metallicStrength = json.value("metallic_strength", decal.metallicStrength);
    decal.angleFadeStartRadians = json.value("angle_fade_start", decal.angleFadeStartRadians);
    decal.angleFadeEndRadians = json.value("angle_fade_end", decal.angleFadeEndRadians);
    decal.sortOrder = json.value("sort_order", decal.sortOrder);
    const auto unit = [](float value) { return value >= 0.0f && value <= 1.0f; };
    const auto finitePositive = [](float value) { return std::isfinite(value) && value > 0.0f; };
    const auto finiteNonNegative = [](float value) { return std::isfinite(value) && value >= 0.0f; };
    const bool validFade = decal.angleFadeStartRadians >= 0.0f &&
                           decal.angleFadeStartRadians <= decal.angleFadeEndRadians &&
                           decal.angleFadeEndRadians <= glm::pi<float>();
    if (!finitePositive(decal.halfExtents.x) || !finitePositive(decal.halfExtents.y) ||
        !finitePositive(decal.halfExtents.z) || !finiteNonNegative(decal.tint.r) || !finiteNonNegative(decal.tint.g) ||
        !finiteNonNegative(decal.tint.b) || !unit(decal.opacity) || !unit(decal.baseColorStrength) ||
        !unit(decal.normalStrength) || !unit(decal.roughness) || !unit(decal.roughnessStrength) ||
        !unit(decal.metallic) || !unit(decal.metallicStrength) || !validFade) {
        return {.error = "Decal parameters are invalid"};
    }
    if (SceneDocumentResult result = loadTextureReference(decal.albedoAsset, assets, options, decal.albedoTexture);
        !result) {
        return result;
    }
    return loadTextureReference(decal.normalAsset, assets, options, decal.normalTexture);
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
    if (const auto* node = registry.try_get<ModelNodeComponent>(entity)) {
        const std::optional<ModelAssetSource> source = assets.modelSource(node->model);
        if (!source) {
            return {.error = "Cannot save a model-node component with an unknown runtime handle"};
        }
        std::string relative;
        if (SceneDocumentResult result = projectRelativePath(source->path, options, relative); !result) {
            return result;
        }
        components["model_node"] = {
            {"path", relative}, {"instance_root", node->instanceRoot.value}, {"node", node->nodeIndex}};
    }
    if (const auto* animation = registry.try_get<AnimationComponent>(entity)) {
        Json blendClip = animation->blendClipIndex ? Json(*animation->blendClipIndex) : Json(nullptr);
        components["animation"] = {{"clip", animation->clipIndex},       {"time", animation->time},
                                   {"speed", animation->playbackSpeed},  {"looping", animation->looping},
                                   {"playing", animation->playing},      {"blend_clip", std::move(blendClip)},
                                   {"blend_time", animation->blendTime}, {"blend_duration", animation->blendDuration}};
    }
    if (const auto* morph = registry.try_get<MorphWeightsComponent>(entity)) {
        components["morph_weights"] = {{"weights", morph->weights}};
    }
    if (const auto* skin = registry.try_get<SkinComponent>(entity)) {
        const std::optional<ModelAssetSource> source = assets.modelSource(skin->model);
        if (!source) {
            return {.error = "Cannot save a skin component with an unknown runtime handle"};
        }
        std::string relative;
        if (SceneDocumentResult result = projectRelativePath(source->path, options, relative); !result) {
            return result;
        }
        components["skin"] = {
            {"path", relative}, {"instance_root", skin->instanceRoot.value}, {"skin", skin->skinIndex}};
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
        std::string environmentAsset = light->environmentAsset;
        if (light->environmentMap) {
            const auto source = assets.textureSource(light->environmentMap);
            if (!source) {
                return {.error = "Cannot save an environment light with an unknown texture handle"};
            }
            if (SceneDocumentResult result = projectRelativePath(source->path, options, environmentAsset); !result) {
                return result;
            }
        }
        components["environment_light"] = {{"sky_color", vector3(light->skyColor)},
                                           {"ground_color", vector3(light->groundColor)},
                                           {"diffuse_intensity", light->diffuseIntensity},
                                           {"specular_intensity", light->specularIntensity},
                                           {"primary", light->primary},
                                           {"environment_asset", environmentAsset}};
    }
    if (const auto* fog = registry.try_get<VolumetricFogComponent>(entity)) {
        components["volumetric_fog"] = {{"density", fog->density},        {"height_falloff", fog->heightFalloff},
                                        {"base_height", fog->baseHeight}, {"albedo", vector3(fog->albedo)},
                                        {"anisotropy", fog->anisotropy},  {"noise_amount", fog->noiseAmount},
                                        {"noise_scale", fog->noiseScale}, {"primary", fog->primary}};
    }
    if (const auto* light = registry.try_get<PointLightComponent>(entity)) {
        components["point_light"] = {{"color", vector3(light->color)},
                                     {"intensity", light->intensity},
                                     {"range", light->range},
                                     {"casts_shadows", light->castsShadows},
                                     {"shadow_resolution", light->shadowResolution},
                                     {"shadow_priority", light->shadowPriority}};
    }
    if (const auto* light = registry.try_get<SpotLightComponent>(entity)) {
        components["spot_light"] = {
            {"direction", vector3(light->direction)},  {"color", vector3(light->color)},
            {"intensity", light->intensity},           {"range", light->range},
            {"inner_cone", light->innerConeRadians},   {"outer_cone", light->outerConeRadians},
            {"casts_shadows", light->castsShadows},    {"shadow_resolution", light->shadowResolution},
            {"shadow_priority", light->shadowPriority}};
    }
    if (const auto* probe = registry.try_get<ReflectionProbeComponent>(entity)) {
        std::string environmentAsset = probe->environmentAsset;
        if (probe->environmentMap) {
            const auto source = assets.textureSource(probe->environmentMap);
            if (!source) {
                return {.error = "Cannot save a reflection probe with an unknown texture handle"};
            }
            if (SceneDocumentResult result = projectRelativePath(source->path, options, environmentAsset); !result) {
                return result;
            }
        }
        components["reflection_probe"] = {{"half_extents", vector3(probe->halfExtents)},
                                          {"blend_distance", probe->blendDistance},
                                          {"priority", probe->priority},
                                          {"environment_asset", environmentAsset}};
    }
    if (const auto* decal = registry.try_get<DecalComponent>(entity)) {
        std::string albedoAsset;
        std::string normalAsset;
        if (SceneDocumentResult result =
                textureReference(assets, decal->albedoTexture, decal->albedoAsset, options, albedoAsset);
            !result) {
            return result;
        }
        if (SceneDocumentResult result =
                textureReference(assets, decal->normalTexture, decal->normalAsset, options, normalAsset);
            !result) {
            return result;
        }
        components["decal"] = decalDocument(*decal, std::move(albedoAsset), std::move(normalAsset));
    }
    if (const auto* volume = registry.try_get<IrradianceVolumeComponent>(entity)) {
        components["irradiance_volume"] = {
            {"half_extents", vector3(volume->halfExtents)},
            {"probe_counts", Json::array({volume->probeCounts.x, volume->probeCounts.y, volume->probeCounts.z})},
            {"intensity", volume->intensity},
            {"blend_distance", volume->blendDistance},
            {"normal_bias", volume->normalBias},
            {"view_bias", volume->viewBias},
            {"bake_asset", volume->bakeAsset}};
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

SceneDocumentResult readIrradianceVolume(const Json& json, const SceneDocumentOptions& options,
                                         IrradianceVolumeComponent& volume) {
    const Json& counts = json.at("probe_counts");
    if (!readVector3(json.at("half_extents"), volume.halfExtents) || !counts.is_array() || counts.size() != 3) {
        return {.error = "Irradiance volume half extents and probe counts must each contain three numbers"};
    }
    volume.probeCounts = {counts[0].get<uint32_t>(), counts[1].get<uint32_t>(), counts[2].get<uint32_t>()};
    volume.intensity = json.at("intensity").get<float>();
    volume.blendDistance = json.at("blend_distance").get<float>();
    volume.normalBias = json.at("normal_bias").get<float>();
    volume.viewBias = json.at("view_bias").get<float>();
    volume.bakeAsset = json.value("bake_asset", std::string{});
    const uint64_t probeCount =
        static_cast<uint64_t>(volume.probeCounts.x) * volume.probeCounts.y * volume.probeCounts.z;
    if (glm::any(glm::lessThanEqual(volume.halfExtents, glm::vec3(0.0f))) || probeCount == 0 ||
        glm::any(glm::greaterThan(volume.probeCounts, glm::uvec3(maximumIrradianceProbesPerAxis))) ||
        probeCount > maximumIrradianceProbes || volume.intensity < 0.0f || volume.blendDistance < 0.0f ||
        volume.normalBias < 0.0f || volume.viewBias < 0.0f) {
        return {.error = "Irradiance volume extents, probe counts, intensity, or biases are invalid"};
    }
    if (volume.bakeAsset.empty()) {
        return {.success = true};
    }
    // Baked lighting is derived data: a reference that is well formed but whose
    // file is gone loads as an unbaked volume (shaded by the environment) so
    // the scene can still be opened and re-baked. Corrupt files still fail.
    const std::filesystem::path reference(volume.bakeAsset);
    if (reference.is_absolute() || reference.empty() || *reference.begin() == "..") {
        return {.error = "Irradiance volume bake reference must be project-relative: " + volume.bakeAsset};
    }
    std::error_code error;
    if (!std::filesystem::exists(options.assetRoot / reference, error)) {
        return {.success = true};
    }
    std::filesystem::path path;
    if (SceneDocumentResult result = resolveAssetPath(volume.bakeAsset, options, path); !result) {
        return result;
    }
    auto data = std::make_shared<IrradianceVolumeData>();
    if (IrradianceVolumeResult loaded = loadIrradianceVolume(path, *data); !loaded) {
        return {.error = loaded.error};
    }
    volume.bakedData = std::move(data);
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
    if (const auto found = components.find("model_node"); found != components.end()) {
        std::filesystem::path path;
        if (SceneDocumentResult result = resolveAssetPath(found->at("path").get<std::string>(), options, path);
            !result) {
            return result;
        }
        const AssetLoadResult<ModelHandle> loaded = assets.loadModel(path);
        if (!loaded) {
            return {.error = loaded.error};
        }
        registry.emplace<ModelNodeComponent>(entity, loaded.handle,
                                             SceneEntityId{found->at("instance_root").get<uint64_t>()},
                                             found->at("node").get<uint32_t>());
    }
    if (const auto found = components.find("animation"); found != components.end()) {
        AnimationComponent animation;
        animation.clipIndex = found->at("clip").get<uint32_t>();
        animation.time = found->at("time").get<float>();
        animation.playbackSpeed = found->at("speed").get<float>();
        animation.looping = found->at("looping").get<bool>();
        animation.playing = found->at("playing").get<bool>();
        if (const auto blend = found->find("blend_clip"); blend != found->end() && !blend->is_null()) {
            animation.blendClipIndex = blend->get<uint32_t>();
        }
        animation.blendTime = found->value("blend_time", 0.0f);
        animation.blendDuration = found->value("blend_duration", 0.0f);
        registry.emplace<AnimationComponent>(entity, animation);
    }
    if (const auto found = components.find("morph_weights"); found != components.end()) {
        std::vector<float> weights = found->at("weights").get<std::vector<float>>();
        if (weights.size() > 64) {
            return {.error = "Morph-weight component exceeds the 64-target safety limit"};
        }
        registry.emplace<MorphWeightsComponent>(entity, std::move(weights));
    }
    if (const auto found = components.find("skin"); found != components.end()) {
        std::filesystem::path path;
        if (SceneDocumentResult result = resolveAssetPath(found->at("path").get<std::string>(), options, path);
            !result) {
            return result;
        }
        const AssetLoadResult<ModelHandle> loaded = assets.loadModel(path);
        if (!loaded) {
            return {.error = loaded.error};
        }
        registry.emplace<SkinComponent>(entity, loaded.handle,
                                        SceneEntityId{found->at("instance_root").get<uint64_t>()},
                                        found->at("skin").get<uint32_t>());
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
        light.environmentAsset = found->value("environment_asset", std::string{});
        if (!light.environmentAsset.empty()) {
            std::filesystem::path environmentPath;
            if (SceneDocumentResult result = resolveAssetPath(light.environmentAsset, options, environmentPath);
                !result) {
                return result;
            }
            const AssetLoadResult<TextureHandle> loaded = assets.loadTexture(environmentPath);
            if (!loaded) {
                return {.error = loaded.error};
            }
            light.environmentMap = loaded.handle;
        }
        registry.emplace<EnvironmentLightComponent>(entity, light);
    }
    if (const auto found = components.find("volumetric_fog"); found != components.end()) {
        VolumetricFogComponent fog;
        if (!readVector3(found->at("albedo"), fog.albedo)) {
            return {.error = "Volumetric fog albedo must contain three numbers"};
        }
        fog.density = found->at("density").get<float>();
        fog.heightFalloff = found->at("height_falloff").get<float>();
        fog.baseHeight = found->at("base_height").get<float>();
        fog.anisotropy = found->at("anisotropy").get<float>();
        fog.noiseAmount = found->value("noise_amount", fog.noiseAmount);
        fog.noiseScale = found->value("noise_scale", fog.noiseScale);
        fog.primary = found->at("primary").get<bool>();
        // Infinite coefficients would turn the integrated fog into NaN.
        const auto finiteNonNegative = [](float value) { return std::isfinite(value) && value >= 0.0f; };
        if (!finiteNonNegative(fog.density) || !finiteNonNegative(fog.heightFalloff) ||
            !(std::abs(fog.anisotropy) < 1.0f) || !(fog.noiseAmount >= 0.0f && fog.noiseAmount <= 1.0f) ||
            !finiteNonNegative(fog.noiseScale) || fog.noiseScale == 0.0f || !finiteNonNegative(fog.albedo.r) ||
            !finiteNonNegative(fog.albedo.g) || !finiteNonNegative(fog.albedo.b) || !std::isfinite(fog.baseHeight)) {
            return {.error = "Volumetric fog parameters are invalid"};
        }
        registry.emplace<VolumetricFogComponent>(entity, fog);
    }
    if (const auto found = components.find("point_light"); found != components.end()) {
        PointLightComponent light;
        if (!readVector3(found->at("color"), light.color)) {
            return {.error = "Point light color must contain three numbers"};
        }
        light.intensity = found->at("intensity").get<float>();
        light.range = found->at("range").get<float>();
        if (light.range <= 0.0f) {
            return {.error = "Point light range must be positive"};
        }
        if (SceneDocumentResult result =
                readLocalLightShadows(*found, light.castsShadows, light.shadowResolution, light.shadowPriority);
            !result) {
            return result;
        }
        registry.emplace<PointLightComponent>(entity, light);
    }
    if (const auto found = components.find("spot_light"); found != components.end()) {
        SpotLightComponent light;
        if (!readVector3(found->at("direction"), light.direction) || !readVector3(found->at("color"), light.color)) {
            return {.error = "Spot light vectors must each contain three numbers"};
        }
        light.intensity = found->at("intensity").get<float>();
        light.range = found->at("range").get<float>();
        light.innerConeRadians = found->at("inner_cone").get<float>();
        light.outerConeRadians = found->at("outer_cone").get<float>();
        if (light.range <= 0.0f || light.innerConeRadians < 0.0f || light.outerConeRadians < light.innerConeRadians) {
            return {.error = "Spot light range and cone angles are invalid"};
        }
        if (SceneDocumentResult result =
                readLocalLightShadows(*found, light.castsShadows, light.shadowResolution, light.shadowPriority);
            !result) {
            return result;
        }
        registry.emplace<SpotLightComponent>(entity, light);
    }
    if (const auto found = components.find("reflection_probe"); found != components.end()) {
        ReflectionProbeComponent probe;
        if (!readVector3(found->at("half_extents"), probe.halfExtents)) {
            return {.error = "Reflection probe half extents must contain three numbers"};
        }
        probe.blendDistance = found->at("blend_distance").get<float>();
        probe.priority = found->at("priority").get<int32_t>();
        probe.environmentAsset = found->value("environment_asset", std::string{});
        if (!probe.environmentAsset.empty()) {
            std::filesystem::path environmentPath;
            if (SceneDocumentResult result = resolveAssetPath(probe.environmentAsset, options, environmentPath);
                !result) {
                return result;
            }
            const AssetLoadResult<TextureHandle> loaded = assets.loadTexture(environmentPath);
            if (!loaded) {
                return {.error = loaded.error};
            }
            probe.environmentMap = loaded.handle;
        }
        if (glm::any(glm::lessThanEqual(probe.halfExtents, glm::vec3(0.0f))) || probe.blendDistance < 0.0f) {
            return {.error = "Reflection probe volume is invalid"};
        }
        registry.emplace<ReflectionProbeComponent>(entity, probe);
    }
    if (const auto found = components.find("decal"); found != components.end()) {
        DecalComponent decal;
        if (SceneDocumentResult result = readDecal(*found, assets, options, decal); !result) {
            return result;
        }
        registry.emplace<DecalComponent>(entity, std::move(decal));
    }
    if (const auto found = components.find("irradiance_volume"); found != components.end()) {
        IrradianceVolumeComponent volume;
        if (SceneDocumentResult result = readIrradianceVolume(*found, options, volume); !result) {
            return result;
        }
        registry.emplace<IrradianceVolumeComponent>(entity, std::move(volume));
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
