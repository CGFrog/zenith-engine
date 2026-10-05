// Field keys are the scene document format, so renaming one breaks saved
// scenes unless a schema migration is added. Presentation hints mirror the
// ranges the inspector has always offered.

#include "RenderComponentTypes.hpp"

#include "ComponentTypeHelpers.hpp"
#include "ComponentTypes.hpp"
#include "SceneAssetReferences.hpp"

#include <glm/gtc/constants.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>

namespace {
using Json = nlohmann::json;
using component_type_helpers::finiteNonNegative;
using component_type_helpers::finitePositive;
using component_type_helpers::loadModelReference;
using component_type_helpers::meshReference;
using component_type_helpers::modelReference;
using component_type_helpers::readMesh;
using component_type_helpers::unitInterval;
} // namespace

ComponentType modelComponentType() {
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

ComponentType modelNodeComponentType() {
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

ComponentType meshRendererComponentType() {
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

ComponentType materialOverrideComponentType() {
    using C = MaterialOverrideComponent;
    // The replacement material handle is runtime-only and never saved; every
    // saved field is optional and defaults to "no change".
    return ComponentTypeBuilder<C>("material_override", "Material Override")
        .category("Rendering")
        .fields(Fields<C>()
                    .add("base_color_factor", &C::baseColorFactor)
                    .optional()
                    .label("Base color")
                    .color()
                    .add("emissive", &C::emissive)
                    .optional()
                    .color()
                    .add("emissive_strength", &C::emissiveStrength)
                    .optional()
                    .drag(0.05f, 0.0, 10000.0)
                    .add("roughness_factor", &C::roughnessFactor)
                    .optional()
                    .label("Roughness")
                    .drag(0.01f, 0.0, 10.0)
                    .add("metallic_factor", &C::metallicFactor)
                    .optional()
                    .label("Metallic")
                    .drag(0.01f, 0.0, 10.0))
        .validate([](const C& values) {
            const bool valid =
                finiteNonNegative(values.baseColorFactor.r) && finiteNonNegative(values.baseColorFactor.g) &&
                finiteNonNegative(values.baseColorFactor.b) && finiteNonNegative(values.baseColorFactor.a) &&
                finiteNonNegative(values.emissive.r) && finiteNonNegative(values.emissive.g) &&
                finiteNonNegative(values.emissive.b) && finiteNonNegative(values.emissiveStrength) &&
                finiteNonNegative(values.roughnessFactor) && finiteNonNegative(values.metallicFactor);
            return valid ? std::string{} : std::string("Material override factors must be finite and non-negative");
        })
        .sanitize([](C& values) {
            values.baseColorFactor = glm::max(values.baseColorFactor, glm::vec4(0.0f));
            values.emissive = glm::max(values.emissive, glm::vec3(0.0f));
            values.emissiveStrength = std::max(values.emissiveStrength, 0.0f);
            values.roughnessFactor = std::max(values.roughnessFactor, 0.0f);
            values.metallicFactor = std::max(values.metallicFactor, 0.0f);
        })
        .build();
}

ComponentType cameraComponentType() {
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

ComponentType particleEmitterComponentType() {
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

ComponentType decalComponentType() {
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
