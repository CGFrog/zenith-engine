// Checks the procedural showcase coverage content (assets/showcase/coverage/) and
// its wiring in projects/scenes/showcase.zenith-scene: the skinned tentacle must
// import with a usable skin and looping clip and animate once loaded from the
// committed scene, and the gold mirror must stay a mirror-smooth tinted metal.
// The feature content (tools/showcase/build_features.py) and the project's render
// settings must keep every renderer feature represented in the showcase.
#include "engine/animation/AnimationSystem.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/assets/PackedVertexStreams.hpp"
#include "engine/project/ProjectConfig.hpp"
#include "engine/scene/ParticleEmitter.hpp"
#include "engine/scene/SceneDocument.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string_view>

namespace {
int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

constexpr std::string_view tentaclePath = "showcase/coverage/tentacle.gltf";
constexpr std::string_view mirrorPath = "showcase/coverage/gold_mirror.gltf";
constexpr std::string_view galleryPath = "showcase/materials/material_gallery.gltf";
constexpr uint32_t tentacleJointCount = 6;

template <typename View> size_t countEntities(const View& view) {
    size_t count = 0;
    for ([[maybe_unused]] const Entity entity : view) {
        ++count;
    }
    return count;
}

bool nearlyEqual(float first, float second, float tolerance = 1.0e-4f) {
    return std::abs(first - second) <= tolerance;
}

void testTentacleImport(const std::filesystem::path& assetRoot) {
    AssetManager assets;
    const AssetLoadResult<ModelHandle> loaded = assets.loadModel(assetRoot / tentaclePath);
    expect(static_cast<bool>(loaded), loaded.error);
    const ModelAsset* model = assets.model(loaded.handle);
    if (!model) {
        return;
    }

    expect(model->skins.size() == 1 && model->skins[0].joints.size() == tentacleJointCount,
           "the tentacle has one skin over a six-joint chain");
    if (model->skins.size() == 1 && model->skins[0].joints.size() == tentacleJointCount) {
        const SkinAsset& skin = model->skins[0];
        bool chainValid = true;
        for (uint32_t joint = 0; joint < tentacleJointCount; ++joint) {
            const uint32_t node = skin.joints[joint];
            chainValid &= node < model->nodes.size();
            // Each joint's inverse bind undoes its rest height of joint * 0.3 m.
            chainValid &= nearlyEqual(skin.inverseBindMatrices[joint][3].y, -0.3f * static_cast<float>(joint));
            if (joint > 0 && node < model->nodes.size()) {
                chainValid &= model->nodes[node].parent == static_cast<int32_t>(skin.joints[joint - 1]);
            }
        }
        expect(chainValid, "tentacle joints form a parented chain with matching inverse-bind matrices");
    }

    expect(model->animations.size() == 1 && nearlyEqual(model->animations[0].duration, 4.0f) &&
               model->animations[0].channels.size() == tentacleJointCount,
           "the tentacle has one 4 s clip with a channel per joint");
    if (!model->animations.empty()) {
        bool rotationsLoop = true;
        bool rotationsMove = true;
        for (const AnimationChannel& channel : model->animations[0].channels) {
            rotationsLoop &=
                channel.path == AnimationPath::Rotation && channel.values.size() > 2 &&
                glm::all(glm::lessThan(glm::abs(channel.values.front() - channel.values.back()), glm::vec4(1.0e-4f)));
            rotationsMove &= channel.values.size() > 2 &&
                             glm::length(channel.values.front() - channel.values[channel.values.size() / 4]) > 0.05f;
        }
        expect(rotationsLoop, "every joint rotation ends where it starts so the clip loops seamlessly");
        expect(rotationsMove, "every joint rotates noticeably during the clip");
    }

    const ModelNode* meshNode = nullptr;
    for (const ModelNode& node : model->nodes) {
        if (node.mesh && node.skin == 0) {
            meshNode = &node;
        }
    }
    const MeshAsset* mesh = meshNode ? assets.mesh(meshNode->mesh) : nullptr;
    expect(mesh != nullptr && !mesh->vertices.empty(), "the skinned mesh node references an imported mesh");
    if (!mesh) {
        return;
    }
    bool influencesValid = true;
    bool blendsJoints = false;
    bool tangentsValid = true;
    for (const Vertex& vertex : mesh->vertices) {
        const float total = vertex.weights.x + vertex.weights.y + vertex.weights.z + vertex.weights.w;
        influencesValid &= nearlyEqual(total, 1.0f, 1.0e-3f);
        for (int influence = 0; influence < 4; ++influence) {
            influencesValid &= vertex.weights[influence] == 0.0f || vertex.joints[influence] < tentacleJointCount;
        }
        blendsJoints |= vertex.weights.x > 0.05f && vertex.weights.y > 0.05f;
        tangentsValid &=
            nearlyEqual(glm::length(glm::vec3(vertex.tangent)), 1.0f, 1.0e-2f) && std::abs(vertex.tangent.w) == 1.0f;
    }
    expect(influencesValid, "tentacle weights are normalized and reference valid joints");
    expect(blendsJoints, "tentacle vertices blend between neighbouring joints");
    expect(tangentsValid, "tentacle tangents are unit length with a handedness sign");

    std::string packError;
    const std::optional<PackedVertexStreams> streams = vertex_packing::packVertexStreams(mesh->vertices, packError);
    expect(streams && streams->skinned() && streams->skin.size() == mesh->vertices.size(),
           "the tentacle packs into a skin vertex stream");
}

void testMirrorImport(const std::filesystem::path& assetRoot) {
    AssetManager assets;
    const AssetLoadResult<MeshHandle> loaded = assets.loadMesh(assetRoot / mirrorPath, 0);
    expect(static_cast<bool>(loaded), loaded.error);
    const MeshAsset* mesh = assets.mesh(loaded.handle);
    const MaterialAsset* material =
        mesh && !mesh->primitives.empty() ? assets.material(mesh->primitives.front().material) : nullptr;
    expect(material && material->metallic == 1.0f && material->roughness <= 0.05f,
           "the gold mirror is a fully metallic, mirror-smooth surface");
    expect(material && material->baseColor.r > material->baseColor.b + 0.3f,
           "the gold mirror is a tinted metal so reflections pick up its colour");
}

// Loads only the coverage entities of the committed showcase scene, so the test
// checks the scene wiring without decoding the rest of the showcase content.
std::string coverageSceneDocument(const std::filesystem::path& scenePath) {
    std::ifstream input(scenePath, std::ios::binary);
    std::stringstream buffer;
    buffer << input.rdbuf();
    nlohmann::json document = nlohmann::json::parse(buffer.str(), nullptr, false);
    if (document.is_discarded() || !document.contains("entities")) {
        return {};
    }
    nlohmann::json coverage = nlohmann::json::array();
    for (const nlohmann::json& entity : document["entities"]) {
        for (const auto& [name, component] : entity.at("components").items()) {
            if (component.is_object() && component.contains("path") &&
                (component["path"] == std::string(tentaclePath) || component["path"] == std::string(mirrorPath))) {
                coverage.push_back(entity);
                break;
            }
        }
    }
    document["entities"] = std::move(coverage);
    return document.dump();
}

void testShowcaseScene(const std::filesystem::path& assetRoot, const std::filesystem::path& scenePath) {
    const std::string document = coverageSceneDocument(scenePath);
    expect(!document.empty(), "the showcase scene document parses");
    AssetManager assets;
    Scene scene;
    const SceneDocumentResult result =
        SceneDocument::deserialize(document, assets, SceneDocumentOptions{.assetRoot = assetRoot}, scene);
    expect(result.success, result.error);
    if (!result) {
        return;
    }

    entt::registry& registry = scene.registry();
    const auto animated = registry.view<const AnimationComponent, const ModelComponent>();
    // The coverage tentacle and the faster motion-blur instance (build_features.py).
    expect(countEntities(animated) == 2, "the showcase has two animated tentacle roots");
    for (const Entity root : animated) {
        const AnimationComponent& animation = animated.get<const AnimationComponent>(root);
        expect(animation.looping && animation.playing && animation.playbackSpeed > 0.0f,
               "every tentacle animation plays and loops");
    }

    const auto skins = registry.view<const SkinComponent, const MeshRendererComponent>();
    expect(countEntities(skins) == 2, "both tentacle meshes render with a skin");
    const auto palette = [&]() {
        std::vector<glm::mat4> joints;
        for (const Entity entity : skins) {
            joints = skins.get<const SkinComponent>(entity).jointPalette;
        }
        return joints;
    };
    updateAnimations(scene, assets, 0.0f);
    const std::vector<glm::mat4> start = palette();
    updateAnimations(scene, assets, 1.0f);
    const std::vector<glm::mat4> later = palette();
    expect(start.size() == tentacleJointCount && later.size() == tentacleJointCount,
           "the scene's skin resolves a palette entry for every joint");
    if (start.size() == tentacleJointCount && later.size() == tentacleJointCount) {
        // The tip joint sweeps furthest; its skinning matrix must move it by centimetres, not rounding noise.
        const glm::vec4 tip{0.0f, 1.75f, 0.0f, 1.0f};
        expect(glm::length(glm::vec3(later.back() * tip - start.back() * tip)) > 0.1f,
               "the loaded tentacle animates its skinning palette over time");
    }
}

// The Milestone 24/25 feature entities (tools/showcase/build_features.py): every
// feature the renderer has must have content in the showcase scene.
std::string featureSceneDocument(const std::filesystem::path& scenePath) {
    std::ifstream input(scenePath, std::ios::binary);
    std::stringstream buffer;
    buffer << input.rdbuf();
    nlohmann::json document = nlohmann::json::parse(buffer.str(), nullptr, false);
    if (document.is_discarded() || !document.contains("entities")) {
        return {};
    }
    nlohmann::json features = nlohmann::json::array();
    for (const nlohmann::json& entity : document["entities"]) {
        const nlohmann::json& components = entity.at("components");
        const bool gallery = components.contains("mesh_renderer") &&
                             components["mesh_renderer"].value("path", std::string{}) == std::string(galleryPath);
        if (gallery || components.contains("decal") || components.contains("particle_emitter") ||
            components.contains("irradiance_volume")) {
            features.push_back(entity);
        }
    }
    document["entities"] = std::move(features);
    return document.dump();
}

void testShowcaseFeatures(const std::filesystem::path& assetRoot, const std::filesystem::path& scenePath) {
    const std::string document = featureSceneDocument(scenePath);
    expect(!document.empty(), "the showcase scene document parses");
    AssetManager assets;
    Scene scene;
    const SceneDocumentResult result =
        SceneDocument::deserialize(document, assets, SceneDocumentOptions{.assetRoot = assetRoot}, scene);
    expect(result.success, result.error);
    if (!result) {
        return;
    }
    entt::registry& registry = scene.registry();

    const auto decals = registry.view<const DecalComponent>();
    expect(countEntities(decals) >= 5, "the showcase has floor, wall and corner decals");
    bool decalTexturesLoaded = true;
    bool normalMappedDecal = false;
    for (const Entity entity : decals) {
        const DecalComponent& decal = decals.get<const DecalComponent>(entity);
        decalTexturesLoaded &= static_cast<bool>(decal.albedoTexture);
        normalMappedDecal |= static_cast<bool>(decal.normalTexture);
    }
    expect(decalTexturesLoaded, "every showcase decal loads its albedo texture");
    expect(normalMappedDecal, "one showcase decal is normal mapped");

    const auto emitters = registry.view<const ParticleEmitterComponent>();
    bool additiveUnlit = false;
    bool alphaLit = false;
    for (const Entity entity : emitters) {
        const ParticleEmitterComponent& emitter = emitters.get<const ParticleEmitterComponent>(entity);
        additiveUnlit |= emitter.blendMode == ParticleBlendMode::Additive && !emitter.lit;
        alphaLit |= emitter.blendMode == ParticleBlendMode::Alpha && emitter.lit;
    }
    expect(additiveUnlit && alphaLit, "the showcase has additive unlit embers and alpha-blended lit smoke");

    const auto galleries = registry.view<const MeshRendererComponent>();
    expect(countEntities(galleries) == 1, "the showcase places the KHR material gallery");
    bool extensionsImported = false;
    for (const Entity entity : galleries) {
        const MeshAsset* mesh = assets.mesh(galleries.get<const MeshRendererComponent>(entity).mesh);
        if (mesh == nullptr) {
            continue;
        }
        bool clearcoat = false;
        bool sheen = false;
        bool transmission = false;
        for (const MeshPrimitive& primitive : mesh->primitives) {
            const MaterialAsset* material = assets.material(primitive.material);
            clearcoat |= material && material->clearcoat.factor > 0.0f;
            sheen |= material && glm::any(glm::greaterThan(material->sheen.color, glm::vec3(0.0f)));
            transmission |= material && material->transmission.factor > 0.0f;
        }
        extensionsImported = clearcoat && sheen && transmission;
    }
    expect(extensionsImported, "the gallery imports clearcoat, sheen and transmission materials");

    const auto volumes = registry.view<const IrradianceVolumeComponent>();
    expect(countEntities(volumes) == 1, "the showcase has one irradiance volume");
    for (const Entity entity : volumes) {
        expect(volumes.get<const IrradianceVolumeComponent>(entity).bakedData != nullptr,
               "the showcase irradiance volume is baked and its bake asset is committed");
    }
}

// The project's render settings select every optional feature the scene shows.
void testShowcaseRenderSettings(const std::filesystem::path& projectPath) {
    ProjectConfig project;
    const ProjectConfigResult loaded = loadProjectConfig(projectPath, project);
    expect(loaded.success, loaded.error);
    const RenderSettings& settings = project.renderSettings;
    expect(settings.antialiasing == AntialiasingMode::Taa, "the showcase uses TAA");
    expect(settings.ambientOcclusion == AmbientOcclusionQuality::HalfResolution,
           "the showcase uses the recommended half-resolution GTAO");
    expect(settings.indirectDiffuse == IndirectDiffuseMode::ProbeVolume, "the showcase samples its irradiance volume");
    expect(settings.decals == DecalQuality::On, "the showcase renders decals");
    expect(settings.particles != ParticleQuality::Off, "the showcase renders GPU particles");
    expect(settings.volumetricFog != VolumetricFogQuality::Off, "the showcase renders volumetric fog");
    expect(settings.motionBlur != MotionBlurQuality::Off, "the showcase renders motion blur");
    expect(settings.depthOfField.quality != DepthOfFieldQuality::Off, "the showcase renders depth of field");
    expect(settings.colorLut.enabled && !settings.colorLut.path.empty() && settings.vignette.enabled &&
               settings.filmGrain.enabled,
           "the showcase grades with a LUT, a vignette and film grain");
}
} // namespace

int main() {
    const std::filesystem::path assetRoot = ZENITH_ASSET_DIR;
    testTentacleImport(assetRoot);
    testMirrorImport(assetRoot);
    const std::filesystem::path projectDirectory = ZENITH_PROJECT_DIR;
    testShowcaseScene(assetRoot, projectDirectory / "scenes" / "showcase.zenith-scene");
    testShowcaseFeatures(assetRoot, projectDirectory / "scenes" / "showcase.zenith-scene");
    testShowcaseRenderSettings(projectDirectory / "showcase.zenith-project");
    return failures == 0 ? 0 : 1;
}
