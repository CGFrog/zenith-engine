// Checks the procedural showcase coverage content (assets/showcase/coverage/) and
// its wiring in projects/scenes/showcase.zenith-scene: the skinned tentacle must
// import with a usable skin and looping clip and animate once loaded from the
// committed scene, and the gold mirror must stay a mirror-smooth tinted metal.
#include "engine/animation/AnimationSystem.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/assets/PackedVertexStreams.hpp"
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
    expect(countEntities(animated) == 1, "the showcase has one animated model root");
    for (const Entity root : animated) {
        const AnimationComponent& animation = animated.get<const AnimationComponent>(root);
        expect(animation.looping && animation.playing && animation.playbackSpeed > 0.0f,
               "the tentacle animation plays and loops");
    }

    const auto skins = registry.view<const SkinComponent, const MeshRendererComponent>();
    expect(countEntities(skins) == 1, "the tentacle mesh renders with a skin");
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
} // namespace

int main() {
    const std::filesystem::path assetRoot = ZENITH_ASSET_DIR;
    testTentacleImport(assetRoot);
    testMirrorImport(assetRoot);
    testShowcaseScene(assetRoot, std::filesystem::path(ZENITH_PROJECT_DIR) / "scenes" / "showcase.zenith-scene");
    return failures == 0 ? 0 : 1;
}
