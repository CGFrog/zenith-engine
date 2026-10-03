// Registers the engine's components. Field keys are the scene document format,
// so renaming one breaks saved scenes unless a schema migration is added.
// Presentation hints mirror the ranges the inspector has always offered.

#include "../assets/IrradianceVolumeAsset.hpp"
#include "CollectionComponentTypes.hpp"
#include "ComponentTypes.hpp"
#include "SceneAssetReferences.hpp"

#include <glm/gtc/constants.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <type_traits>

namespace {
using Json = nlohmann::json;

constexpr uint32_t maximumLocalShadowResolution = 16384;
constexpr size_t maximumMorphTargets = 64;

bool finiteNonNegative(float value) {
    return std::isfinite(value) && value >= 0.0f;
}

bool finitePositive(float value) {
    return std::isfinite(value) && value > 0.0f;
}

bool unitInterval(float value) {
    return value >= 0.0f && value <= 1.0f;
}

std::string validateLocalShadowResolution(uint32_t resolution) {
    if (resolution > maximumLocalShadowResolution) {
        return "Local light shadow resolution must be a whole number of at most 16384 texels";
    }
    return {};
}

Json vector3(const glm::vec3& value) {
    return Json::array({value.x, value.y, value.z});
}

bool readVector3(const Json& json, glm::vec3& value) {
    if (!json.is_array() || json.size() != 3) {
        return false;
    }
    value = {json[0].get<float>(), json[1].get<float>(), json[2].get<float>()};
    return true;
}

SceneDocumentResult modelReference(const AssetManager& assets, ModelHandle handle, const SceneDocumentOptions& options,
                                   const char* component, std::string& output) {
    const std::optional<ModelAssetSource> source = assets.modelSource(handle);
    if (!source) {
        return {.error = std::string("Cannot save a ") + component + " component with an unknown runtime handle"};
    }
    return projectRelativeAssetPath(source->path, options, output);
}

// Resolves a reference object (by GUID, then path) to an existing asset file.
SceneDocumentResult resolveAssetReference(const Json& json, const ComponentLoadContext& context,
                                          std::filesystem::path& path) {
    std::string reference;
    if (SceneDocumentResult result = readAssetReference(json, context.options, reference); !result) {
        return result;
    }
    return resolveProjectAssetPath(reference, context.options, path);
}

SceneDocumentResult loadModelReference(const Json& json, const ComponentLoadContext& context, ModelHandle& output) {
    std::filesystem::path path;
    if (SceneDocumentResult result = resolveAssetReference(json, context, path); !result) {
        return result;
    }
    const AssetLoadResult<ModelHandle> loaded = context.assets.loadModel(path);
    if (!loaded) {
        return {.error = loaded.error};
    }
    output = loaded.handle;
    return {.success = true};
}

SceneDocumentResult meshReference(const AssetManager& assets, MeshHandle handle, const SceneDocumentOptions& options,
                                  Json& output) {
    const std::optional<MeshAssetSource> source = assets.meshSource(handle);
    if (!source) {
        return {.error = "Cannot save a mesh component with an unknown runtime handle"};
    }
    std::string relative;
    if (SceneDocumentResult result = projectRelativeAssetPath(source->path, options, relative); !result) {
        return result;
    }
    output = {{"mesh", source->meshIndex}};
    writeAssetReference(relative, options, output);
    return {.success = true};
}

SceneDocumentResult readMesh(const Json& json, const ComponentLoadContext& context, MeshHandle& output) {
    std::filesystem::path path;
    if (SceneDocumentResult result = resolveAssetReference(json, context, path); !result) {
        return result;
    }
    const AssetLoadResult<MeshHandle> loaded = context.assets.loadMesh(path, json.at("mesh").get<uint32_t>());
    if (!loaded) {
        return {.error = loaded.error};
    }
    output = loaded.handle;
    return {.success = true};
}

ComponentType modelType() {
    return ComponentTypeBuilder<ModelComponent>("model", "Model")
        .category("Rendering")
        .internal()
        .codec(
            [](const ModelComponent& model, const ComponentSaveContext& context, Json& output) {
                std::string relative;
                if (SceneDocumentResult result =
                        modelReference(context.assets, model.model, context.options, "model", relative);
                    !result) {
                    return result;
                }
                output = Json::object();
                writeAssetReference(relative, context.options, output);
                return SceneDocumentResult{.success = true};
            },
            [](const Json& input, ModelComponent& model, const ComponentLoadContext& context) {
                return loadModelReference(input, context, model.model);
            })
        .build();
}

ComponentType modelNodeType() {
    return ComponentTypeBuilder<ModelNodeComponent>("model_node", "Model Node")
        .category("Rendering")
        .internal()
        .codec(
            [](const ModelNodeComponent& node, const ComponentSaveContext& context, Json& output) {
                std::string relative;
                if (SceneDocumentResult result =
                        modelReference(context.assets, node.model, context.options, "model-node", relative);
                    !result) {
                    return result;
                }
                output = {{"instance_root", node.instanceRoot.value}, {"node", node.nodeIndex}};
                writeAssetReference(relative, context.options, output);
                return SceneDocumentResult{.success = true};
            },
            [](const Json& input, ModelNodeComponent& node, const ComponentLoadContext& context) {
                if (SceneDocumentResult result = loadModelReference(input, context, node.model); !result) {
                    return result;
                }
                node.instanceRoot = SceneEntityId{input.at("instance_root").get<uint64_t>()};
                node.nodeIndex = input.at("node").get<uint32_t>();
                return SceneDocumentResult{.success = true};
            })
        .build();
}

ComponentType animationType() {
    return ComponentTypeBuilder<AnimationComponent>("animation", "Animation")
        .category("Animation")
        .codec(
            [](const AnimationComponent& animation, const ComponentSaveContext&, Json& output) {
                Json blendClip = animation.blendClipIndex ? Json(*animation.blendClipIndex) : Json(nullptr);
                output = {{"clip", animation.clipIndex},       {"time", animation.time},
                          {"speed", animation.playbackSpeed},  {"looping", animation.looping},
                          {"playing", animation.playing},      {"blend_clip", std::move(blendClip)},
                          {"blend_time", animation.blendTime}, {"blend_duration", animation.blendDuration}};
                return SceneDocumentResult{.success = true};
            },
            [](const Json& input, AnimationComponent& animation, const ComponentLoadContext&) {
                animation.clipIndex = input.at("clip").get<uint32_t>();
                animation.time = input.at("time").get<float>();
                animation.playbackSpeed = input.at("speed").get<float>();
                animation.looping = input.at("looping").get<bool>();
                animation.playing = input.at("playing").get<bool>();
                if (const auto blend = input.find("blend_clip"); blend != input.end() && !blend->is_null()) {
                    animation.blendClipIndex = blend->get<uint32_t>();
                }
                animation.blendTime = input.value("blend_time", 0.0f);
                animation.blendDuration = input.value("blend_duration", 0.0f);
                return SceneDocumentResult{.success = true};
            })
        .build();
}

ComponentType morphWeightsType() {
    return ComponentTypeBuilder<MorphWeightsComponent>("morph_weights", "Morph Weights")
        .category("Animation")
        .codec(
            [](const MorphWeightsComponent& morph, const ComponentSaveContext&, Json& output) {
                output = {{"weights", morph.weights}};
                return SceneDocumentResult{.success = true};
            },
            [](const Json& input, MorphWeightsComponent& morph, const ComponentLoadContext&) {
                morph.weights = input.at("weights").get<std::vector<float>>();
                return SceneDocumentResult{.success = true};
            })
        .validate([](const MorphWeightsComponent& morph) {
            return morph.weights.size() > maximumMorphTargets
                       ? std::string("Morph-weight component exceeds the 64-target safety limit")
                       : std::string{};
        })
        .build();
}

ComponentType skinType() {
    return ComponentTypeBuilder<SkinComponent>("skin", "Skin")
        .category("Animation")
        .internal()
        .codec(
            [](const SkinComponent& skin, const ComponentSaveContext& context, Json& output) {
                std::string relative;
                if (SceneDocumentResult result =
                        modelReference(context.assets, skin.model, context.options, "skin", relative);
                    !result) {
                    return result;
                }
                output = {{"instance_root", skin.instanceRoot.value}, {"skin", skin.skinIndex}};
                writeAssetReference(relative, context.options, output);
                return SceneDocumentResult{.success = true};
            },
            [](const Json& input, SkinComponent& skin, const ComponentLoadContext& context) {
                if (SceneDocumentResult result = loadModelReference(input, context, skin.model); !result) {
                    return result;
                }
                skin.instanceRoot = SceneEntityId{input.at("instance_root").get<uint64_t>()};
                skin.skinIndex = input.at("skin").get<uint32_t>();
                return SceneDocumentResult{.success = true};
            })
        .build();
}

ComponentType meshRendererType() {
    return ComponentTypeBuilder<MeshRendererComponent>("mesh_renderer", "Mesh Renderer")
        .category("Rendering")
        .internal()
        .codec([](const MeshRendererComponent& mesh, const ComponentSaveContext& context,
                  Json& output) { return meshReference(context.assets, mesh.mesh, context.options, output); },
               [](const Json& input, MeshRendererComponent& mesh, const ComponentLoadContext& context) {
                   return readMesh(input, context, mesh.mesh);
               })
        .build();
}

ComponentType cameraType() {
    using C = CameraComponent;
    return ComponentTypeBuilder<C>("camera", "Camera")
        .category("Rendering")
        .fields(Fields<C>()
                    .add("vertical_fov", &C::verticalFovRadians)
                    .label("Vertical FOV")
                    .angle()
                    .drag(0.25f, 1.0, 179.0)
                    .format("%.1f deg")
                    .add("near_plane", &C::nearPlane)
                    .drag(0.01f, 0.001, 100000.0)
                    .add("far_plane", &C::farPlane)
                    .drag(1.0f, 0.001, 100000.0)
                    .add("primary", &C::primary))
        .sanitize([](C& camera) { camera.farPlane = std::max(camera.farPlane, camera.nearPlane); })
        .build();
}

ComponentType directionalLightType() {
    using C = DirectionalLightComponent;
    using S = DirectionalShadowSettings;
    return ComponentTypeBuilder<C>("directional_light", "Directional Light")
        .category("Lights")
        .fields(Fields<C>()
                    .add("direction", &C::direction)
                    .drag(0.01f, -1.0, 1.0)
                    .tooltip("Direction from a shaded surface toward the light, in world space.")
                    .add("color", &C::color)
                    .color()
                    .add("intensity", &C::intensity)
                    .drag(0.05f, 0.0, 10000.0)
                    .add("primary", &C::primary)
                    .structure("shadows", &C::shadows,
                               Fields<S>()
                                   .add("enabled", &S::enabled)
                                   .label("Cast shadows")
                                   .add("half_extent", &S::halfExtent)
                                   .drag(0.1f, 0.1, 10000.0)
                                   .add("focus_distance", &S::focusDistance)
                                   .drag(0.1f, 0.0, 10000.0)
                                   .add("depth_range", &S::depthRange)
                                   .drag(0.5f, 1.0, 10000.0)
                                   .add("constant_bias", &S::constantBias)
                                   .drag(0.01f, 0.0, 10.0)
                                   .add("slope_bias", &S::slopeBias)
                                   .drag(0.01f, 0.0, 10.0)
                                   .add("normal_bias", &S::normalBias)
                                   .drag(0.0005f, 0.0, 1.0)
                                   .format("%.4f")))
        .build();
}

ComponentType environmentLightType() {
    using C = EnvironmentLightComponent;
    return ComponentTypeBuilder<C>("environment_light", "Environment Light")
        .category("Lights")
        .fields(Fields<C>()
                    .add("sky_color", &C::skyColor)
                    .color()
                    .add("ground_color", &C::groundColor)
                    .color()
                    .add("diffuse_intensity", &C::diffuseIntensity)
                    .drag(0.01f, 0.0, 100.0)
                    .add("specular_intensity", &C::specularIntensity)
                    .drag(0.01f, 0.0, 100.0)
                    .add("primary", &C::primary)
                    .add("environment_asset", &C::environmentAsset)
                    .optional()
                    .readOnly())
        .texture(&C::environmentAsset, &C::environmentMap)
        .build();
}

ComponentType volumetricFogType() {
    using C = VolumetricFogComponent;
    return ComponentTypeBuilder<C>("volumetric_fog", "Volumetric Fog")
        .category("Environment")
        .fields(Fields<C>()
                    .add("density", &C::density)
                    .drag(0.001f, 0.0, 10.0)
                    .format("%.4f")
                    .add("height_falloff", &C::heightFalloff)
                    .drag(0.005f, 0.0, 10.0)
                    .format("%.3f")
                    .add("base_height", &C::baseHeight)
                    .drag(0.05f, -10000.0, 10000.0)
                    .add("albedo", &C::albedo)
                    .color()
                    .add("anisotropy", &C::anisotropy)
                    .slider(-0.95, 0.95)
                    .format("%.2f")
                    .add("noise_amount", &C::noiseAmount)
                    .optional()
                    .slider(0.0, 1.0)
                    .format("%.2f")
                    .add("noise_scale", &C::noiseScale)
                    .optional()
                    .drag(0.05f, 0.05, 1000.0)
                    .add("primary", &C::primary))
        .validate([](const C& fog) {
            // Infinite coefficients would turn the integrated fog into NaN.
            const bool valid = finiteNonNegative(fog.density) && finiteNonNegative(fog.heightFalloff) &&
                               std::abs(fog.anisotropy) < 1.0f && unitInterval(fog.noiseAmount) &&
                               finitePositive(fog.noiseScale) && finiteNonNegative(fog.albedo.r) &&
                               finiteNonNegative(fog.albedo.g) && finiteNonNegative(fog.albedo.b) &&
                               std::isfinite(fog.baseHeight);
            return valid ? std::string{} : std::string("Volumetric fog parameters are invalid");
        })
        .build();
}

ComponentType particleEmitterType() {
    using C = ParticleEmitterComponent;
    const auto maximumParticles = static_cast<double>(maximumParticlesPerEmitter);
    // Every field is optional so emitters keep loading as the component grows.
    return ComponentTypeBuilder<C>("particle_emitter", "Particle Emitter")
        .category("Effects")
        .fields(Fields<C>()
                    .add("max_particles", &C::maxParticles)
                    .optional()
                    .drag(16.0f, 1.0, maximumParticles)
                    .add("emitting", &C::emitting)
                    .optional()
                    .add("spawn_rate", &C::spawnRate)
                    .optional()
                    .drag(0.5f, 0.0, 100000.0)
                    .format("%.1f /s")
                    .add("burst_count", &C::burstCount)
                    .optional()
                    .drag(1.0f, 0.0, maximumParticles)
                    .add("burst_interval", &C::burstInterval)
                    .optional()
                    .drag(0.01f, 0.0, 3600.0)
                    .format("%.2f s")
                    .range("lifetime", &C::lifetimeMin, &C::lifetimeMax)
                    .optional()
                    .drag(0.01f, 0.001, 3600.0)
                    .add("direction", &C::direction)
                    .optional()
                    .drag(0.01f, -1.0, 1.0)
                    .add("cone_angle", &C::coneAngleRadians)
                    .optional()
                    .angle()
                    .slider(0.0, 180.0)
                    .range("speed", &C::speedMin, &C::speedMax)
                    .optional()
                    .drag(0.01f, 0.0, 10000.0)
                    .add("spawn_radius", &C::spawnRadius)
                    .optional()
                    .drag(0.01f, 0.0, 10000.0)
                    .add("gravity", &C::gravity)
                    .optional()
                    .drag(0.05f, -1000.0, 1000.0)
                    .add("drag", &C::drag)
                    .optional()
                    .drag(0.01f, 0.0, 100.0)
                    .add("start_size", &C::startSize)
                    .optional()
                    .drag(0.005f, 0.0, 1000.0)
                    .add("end_size", &C::endSize)
                    .optional()
                    .drag(0.005f, 0.0, 1000.0)
                    .add("angular_speed", &C::angularSpeed)
                    .optional()
                    .drag(0.01f, 0.0, 100.0)
                    .format("%.2f rad/s")
                    .add("start_color", &C::startColor)
                    .optional()
                    .color()
                    .add("end_color", &C::endColor)
                    .optional()
                    .color()
                    .add("emissive_intensity", &C::emissiveIntensity)
                    .optional()
                    .drag(0.05f, 0.0, 10000.0)
                    .add("lit", &C::lit)
                    .optional()
                    .add("blend_mode", &C::blendMode)
                    .optional()
                    .options({{"alpha", "Alpha", static_cast<int64_t>(ParticleBlendMode::Alpha)},
                              {"additive", "Additive", static_cast<int64_t>(ParticleBlendMode::Additive)}})
                    .add("space", &C::space)
                    .optional()
                    .label("Simulation space")
                    .options({{"world", "World", static_cast<int64_t>(ParticleSimulationSpace::World)},
                              {"local", "Local", static_cast<int64_t>(ParticleSimulationSpace::Local)}})
                    .add("soft_distance", &C::softDistance)
                    .optional()
                    .drag(0.01f, 0.0, 100.0)
                    .add("texture_asset", &C::textureAsset)
                    .optional()
                    .label("Sprite")
                    .readOnly())
        .texture(&C::textureAsset, &C::texture)
        .validate([](const C& emitter) { return validateParticleEmitter(emitter); })
        .sanitize([](C& emitter) {
            if (glm::dot(emitter.direction, emitter.direction) < 1e-8f) {
                emitter.direction = {0.0f, 1.0f, 0.0f};
            }
        })
        .build();
}

// Local-light shadow fields are optional so documents written before
// local-light shadows still load with shadows off.
template <typename Light> Fields<Light>& localLightShadowFields(Fields<Light>& fields) {
    return fields.add("casts_shadows", &Light::castsShadows)
        .optional()
        .label("Cast shadows")
        .add("shadow_resolution", &Light::shadowResolution)
        .optional()
        .drag(8.0f, 0.0, 4096.0)
        .format("%d texels")
        .tooltip("Requested shadow tile edge in texels; 0 uses the quality default.")
        .add("shadow_priority", &Light::shadowPriority)
        .optional()
        .drag(0.1f)
        .tooltip("Lights with a higher priority keep their shadow atlas space first.");
}

ComponentType pointLightType() {
    using C = PointLightComponent;
    Fields<C> fields;
    fields.add("color", &C::color)
        .color()
        .add("intensity", &C::intensity)
        .drag(0.05f, 0.0, 10000.0)
        .add("range", &C::range)
        .drag(0.05f, 0.01, 10000.0);
    return ComponentTypeBuilder<C>("point_light", "Point Light")
        .category("Lights")
        .fields(std::move(localLightShadowFields(fields)))
        .validate([](const C& light) {
            if (light.range <= 0.0f) {
                return std::string("Point light range must be positive");
            }
            return validateLocalShadowResolution(light.shadowResolution);
        })
        .build();
}

ComponentType spotLightType() {
    using C = SpotLightComponent;
    Fields<C> fields;
    fields.add("direction", &C::direction)
        .drag(0.01f, -1.0, 1.0)
        .add("color", &C::color)
        .color()
        .add("intensity", &C::intensity)
        .drag(0.05f, 0.0, 10000.0)
        .add("range", &C::range)
        .drag(0.05f, 0.01, 10000.0)
        .add("inner_cone", &C::innerConeRadians)
        .angle()
        .slider(0.0, 89.0)
        .add("outer_cone", &C::outerConeRadians)
        .angle()
        .slider(0.0, 89.0);
    return ComponentTypeBuilder<C>("spot_light", "Spot Light")
        .category("Lights")
        .fields(std::move(localLightShadowFields(fields)))
        .validate([](const C& light) {
            if (light.range <= 0.0f || light.innerConeRadians < 0.0f ||
                light.outerConeRadians < light.innerConeRadians) {
                return std::string("Spot light range and cone angles are invalid");
            }
            return validateLocalShadowResolution(light.shadowResolution);
        })
        .sanitize([](C& light) { light.outerConeRadians = std::max(light.outerConeRadians, light.innerConeRadians); })
        .build();
}

ComponentType reflectionProbeType() {
    using C = ReflectionProbeComponent;
    return ComponentTypeBuilder<C>("reflection_probe", "Reflection Probe")
        .category("Lights")
        .fields(Fields<C>()
                    .add("half_extents", &C::halfExtents)
                    .drag(0.01f, 0.01, 10000.0)
                    .add("blend_distance", &C::blendDistance)
                    .drag(0.01f, 0.0, 100.0)
                    .add("priority", &C::priority)
                    .drag(0.1f)
                    .add("environment_asset", &C::environmentAsset)
                    .optional()
                    .readOnly())
        .texture(&C::environmentAsset, &C::environmentMap)
        .validate([](const C& probe) {
            if (glm::any(glm::lessThanEqual(probe.halfExtents, glm::vec3(0.0f))) || probe.blendDistance < 0.0f) {
                return std::string("Reflection probe volume is invalid");
            }
            return std::string{};
        })
        .build();
}

ComponentType decalType() {
    using C = DecalComponent;
    // Every field but half_extents is optional and defaults as in DecalComponent.
    return ComponentTypeBuilder<C>("decal", "Decal")
        .category("Effects")
        .fields(Fields<C>()
                    .add("half_extents", &C::halfExtents)
                    .drag(0.01f, 0.01, 10000.0)
                    .add("tint", &C::tint)
                    .optional()
                    .color()
                    .add("opacity", &C::opacity)
                    .optional()
                    .slider(0.0, 1.0)
                    .add("base_color_strength", &C::baseColorStrength)
                    .optional()
                    .slider(0.0, 1.0)
                    .add("normal_strength", &C::normalStrength)
                    .optional()
                    .slider(0.0, 1.0)
                    .add("roughness", &C::roughness)
                    .optional()
                    .slider(0.0, 1.0)
                    .add("roughness_strength", &C::roughnessStrength)
                    .optional()
                    .slider(0.0, 1.0)
                    .add("metallic", &C::metallic)
                    .optional()
                    .slider(0.0, 1.0)
                    .add("metallic_strength", &C::metallicStrength)
                    .optional()
                    .slider(0.0, 1.0)
                    .add("angle_fade_start", &C::angleFadeStartRadians)
                    .optional()
                    .angle()
                    .drag(0.25f, 0.0, 180.0)
                    .format("%.1f deg")
                    .add("angle_fade_end", &C::angleFadeEndRadians)
                    .optional()
                    .angle()
                    .drag(0.25f, 0.0, 180.0)
                    .format("%.1f deg")
                    .add("sort_order", &C::sortOrder)
                    .optional()
                    .drag(0.1f)
                    .add("albedo_asset", &C::albedoAsset)
                    .optional()
                    .label("Albedo")
                    .readOnly()
                    .add("normal_asset", &C::normalAsset)
                    .optional()
                    .label("Normal")
                    .readOnly())
        .texture(&C::albedoAsset, &C::albedoTexture)
        .texture(&C::normalAsset, &C::normalTexture)
        .validate([](const C& decal) {
            const bool validFade = decal.angleFadeStartRadians >= 0.0f &&
                                   decal.angleFadeStartRadians <= decal.angleFadeEndRadians &&
                                   decal.angleFadeEndRadians <= glm::pi<float>();
            const bool valid = finitePositive(decal.halfExtents.x) && finitePositive(decal.halfExtents.y) &&
                               finitePositive(decal.halfExtents.z) && finiteNonNegative(decal.tint.r) &&
                               finiteNonNegative(decal.tint.g) && finiteNonNegative(decal.tint.b) &&
                               unitInterval(decal.opacity) && unitInterval(decal.baseColorStrength) &&
                               unitInterval(decal.normalStrength) && unitInterval(decal.roughness) &&
                               unitInterval(decal.roughnessStrength) && unitInterval(decal.metallic) &&
                               unitInterval(decal.metallicStrength) && validFade;
            return valid ? std::string{} : std::string("Decal parameters are invalid");
        })
        .sanitize([](C& decal) {
            decal.halfExtents = glm::max(decal.halfExtents, glm::vec3(0.01f));
            decal.tint = glm::max(decal.tint, glm::vec3(0.0f));
            decal.angleFadeEndRadians = std::max(decal.angleFadeEndRadians, decal.angleFadeStartRadians);
        })
        .build();
}

// Baked lighting is derived data: a reference that is well formed but whose
// file is gone loads as an unbaked volume (shaded by the environment) so the
// scene can still be opened and re-baked. Corrupt files still fail.
SceneDocumentResult loadIrradianceBake(IrradianceVolumeComponent& volume, const ComponentLoadContext& context) {
    if (volume.bakeAsset.empty()) {
        return {.success = true};
    }
    const std::filesystem::path reference(volume.bakeAsset);
    if (reference.is_absolute() || reference.empty() || *reference.begin() == "..") {
        return {.error = "Irradiance volume bake reference must be project-relative: " + volume.bakeAsset};
    }
    std::error_code error;
    if (!std::filesystem::exists(context.options.assetRoot / reference, error)) {
        return {.success = true};
    }
    std::filesystem::path path;
    if (SceneDocumentResult result = resolveProjectAssetPath(volume.bakeAsset, context.options, path); !result) {
        return result;
    }
    auto data = std::make_shared<IrradianceVolumeData>();
    if (IrradianceVolumeResult loaded = loadIrradianceVolume(path, *data); !loaded) {
        return {.error = loaded.error};
    }
    volume.bakedData = std::move(data);
    return {.success = true};
}

ComponentType irradianceVolumeType() {
    using C = IrradianceVolumeComponent;
    return ComponentTypeBuilder<C>("irradiance_volume", "Irradiance Volume")
        .category("Lights")
        .fields(Fields<C>()
                    .add("half_extents", &C::halfExtents)
                    .drag(0.01f, 0.01, 10000.0)
                    .add("probe_counts", &C::probeCounts)
                    .drag(0.05f, 1.0, static_cast<double>(maximumIrradianceProbesPerAxis))
                    .add("intensity", &C::intensity)
                    .drag(0.01f, 0.0, 100.0)
                    .add("blend_distance", &C::blendDistance)
                    .drag(0.01f, 0.0, 100.0)
                    .add("normal_bias", &C::normalBias)
                    .drag(0.005f, 0.0, 10.0)
                    .add("view_bias", &C::viewBias)
                    .drag(0.005f, 0.0, 10.0)
                    .add("bake_asset", &C::bakeAsset)
                    .optional()
                    .label("Bake")
                    .readOnly())
        .validate([](const C& volume) {
            const uint64_t probeCount =
                static_cast<uint64_t>(volume.probeCounts.x) * volume.probeCounts.y * volume.probeCounts.z;
            if (glm::any(glm::lessThanEqual(volume.halfExtents, glm::vec3(0.0f))) || probeCount == 0 ||
                glm::any(glm::greaterThan(volume.probeCounts, glm::uvec3(maximumIrradianceProbesPerAxis))) ||
                probeCount > maximumIrradianceProbes || volume.intensity < 0.0f || volume.blendDistance < 0.0f ||
                volume.normalBias < 0.0f || volume.viewBias < 0.0f) {
                return std::string("Irradiance volume extents, probe counts, intensity, or biases are invalid");
            }
            return std::string{};
        })
        .assetReference(&C::bakeAsset)
        .afterLoad(loadIrradianceBake)
        .build();
}

ComponentType colliderType() {
    return ComponentTypeBuilder<ColliderComponent>("collider", "Collider")
        .category("Physics")
        .codec(
            [](const ColliderComponent& collider, const ComponentSaveContext& context, Json& output) {
                if (const auto* triangleMesh = std::get_if<TriangleMeshCollider>(&collider.shape)) {
                    Json reference;
                    if (SceneDocumentResult result =
                            meshReference(context.assets, triangleMesh->mesh, context.options, reference);
                        !result) {
                        return result;
                    }
                    output = {{"type", "triangle_mesh"}, {"asset", std::move(reference)}};
                    return SceneDocumentResult{.success = true};
                }
                std::visit(
                    [&output](const auto& shape) {
                        using Shape = std::decay_t<decltype(shape)>;
                        if constexpr (std::is_same_v<Shape, BoxCollider>) {
                            output = {{"type", "box"}, {"half_extents", vector3(shape.halfExtents)}};
                        } else if constexpr (std::is_same_v<Shape, SphereCollider>) {
                            output = {{"type", "sphere"}, {"radius", shape.radius}};
                        } else if constexpr (std::is_same_v<Shape, CapsuleCollider>) {
                            output = {{"type", "capsule"}, {"half_height", shape.halfHeight}, {"radius", shape.radius}};
                        }
                    },
                    collider.shape);
                return SceneDocumentResult{.success = true};
            },
            [](const Json& input, ColliderComponent& collider, const ComponentLoadContext& context) {
                const std::string type = input.at("type").get<std::string>();
                if (type == "box") {
                    BoxCollider box;
                    if (!readVector3(input.at("half_extents"), box.halfExtents)) {
                        return SceneDocumentResult{.error = "Box collider half_extents must contain three numbers"};
                    }
                    collider.shape = box;
                } else if (type == "sphere") {
                    collider.shape = SphereCollider{input.at("radius").get<float>()};
                } else if (type == "capsule") {
                    collider.shape =
                        CapsuleCollider{input.at("half_height").get<float>(), input.at("radius").get<float>()};
                } else if (type == "triangle_mesh") {
                    MeshHandle mesh;
                    if (SceneDocumentResult result = readMesh(input.at("asset"), context, mesh); !result) {
                        return result;
                    }
                    collider.shape = TriangleMeshCollider{mesh};
                } else {
                    return SceneDocumentResult{.error = "Unsupported collider type: " + type};
                }
                return SceneDocumentResult{.success = true};
            })
        .build();
}

ComponentType rigidBodyType() {
    using C = RigidBodyComponent;
    return ComponentTypeBuilder<C>("rigid_body", "Rigid Body")
        .category("Physics")
        .fields(Fields<C>()
                    .add("motion_type", &C::motionType)
                    .label("Motion")
                    .options({{"static", "Static", static_cast<int64_t>(PhysicsMotionType::Static)},
                              {"kinematic", "Kinematic", static_cast<int64_t>(PhysicsMotionType::Kinematic)},
                              {"dynamic", "Dynamic", static_cast<int64_t>(PhysicsMotionType::Dynamic)}},
                             EnumEncoding::Integer)
                    .add("layer", &C::layer)
                    .options({{"static", "Static", static_cast<int64_t>(PhysicsLayer::Static)},
                              {"dynamic", "Dynamic", static_cast<int64_t>(PhysicsLayer::Dynamic)},
                              {"player", "Player", static_cast<int64_t>(PhysicsLayer::Player)}},
                             EnumEncoding::Integer)
                    .add("mass", &C::mass)
                    .drag(0.05f, 0.001, 100000.0)
                    .add("friction", &C::friction)
                    .slider(0.0, 1.0)
                    .add("restitution", &C::restitution)
                    .slider(0.0, 1.0)
                    .add("linear_damping", &C::linearDamping)
                    .drag(0.01f, 0.0, 100.0)
                    .add("angular_damping", &C::angularDamping)
                    .drag(0.01f, 0.0, 100.0)
                    .add("gravity_factor", &C::gravityFactor)
                    .drag(0.01f, -10.0, 10.0)
                    .add("sensor", &C::sensor)
                    .add("allow_sleeping", &C::allowSleeping)
                    .add("continuous_collision", &C::continuousCollision)
                    .add("lock_rotation", &C::lockRotation))
        .build();
}
} // namespace

SceneDocumentResult registerBuiltInComponentTypes(ComponentTypeRegistry& registry) {
    // Registration order is the order documents are read in.
    for (ComponentType type :
         {modelType(), modelNodeType(), animationType(), morphWeightsType(), skinType(), meshRendererType(),
          cameraType(), directionalLightType(), environmentLightType(), volumetricFogType(), particleEmitterType(),
          pointLightType(), spotLightType(), reflectionProbeType(), decalType(), irradianceVolumeType(), colliderType(),
          rigidBodyType()}) {
        if (SceneDocumentResult result = registry.add(std::move(type)); !result) {
            return result;
        }
    }
    // Registered from their own translation unit to keep this one small.
    return registerCollectionComponentTypes(registry);
}
