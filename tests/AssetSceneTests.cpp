#include "engine/assets/Assets.hpp"
#include "engine/scene/FirstPersonController.hpp"
#include "engine/scene/Scene.hpp"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

void writeU32(std::ofstream& stream, uint32_t value) {
    stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

std::filesystem::path writeGlbFixture() {
    std::string json =
        R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],"buffers":[{"byteLength":72}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":48},{"buffer":0,"byteOffset":48,"byteLength":24}],"accessors":[{"bufferView":0,"componentType":5126,"count":4,"type":"VEC3","min":[-1,-1,-1],"max":[1,1,1]},{"bufferView":1,"componentType":5123,"count":12,"type":"SCALAR"}]})";
    while (json.size() % 4 != 0) {
        json.push_back(' ');
    }
    const float positions[]{0, 1, 0, -1, -1, 1, 1, -1, 1, 0, -1, -1};
    const uint16_t indices[]{0, 1, 2, 0, 2, 3, 0, 3, 1, 1, 3, 2};
    const uint32_t binarySize = sizeof(positions) + sizeof(indices);
    const uint32_t totalSize = 12 + 8 + static_cast<uint32_t>(json.size()) + 8 + binarySize;
    const auto path = std::filesystem::temp_directory_path() / "zenith_model_loading_test.glb";
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    writeU32(stream, 0x46546C67);
    writeU32(stream, 2);
    writeU32(stream, totalSize);
    writeU32(stream, static_cast<uint32_t>(json.size()));
    writeU32(stream, 0x4E4F534A);
    stream.write(json.data(), json.size());
    writeU32(stream, binarySize);
    writeU32(stream, 0x004E4942);
    stream.write(reinterpret_cast<const char*>(positions), sizeof(positions));
    stream.write(reinterpret_cast<const char*>(indices), sizeof(indices));
    return path;
}
} // namespace

int main() {
    AssetManager assets;
    const std::filesystem::path sample = std::filesystem::path{ZENITH_ASSET_DIR} / "sample.gltf";
    const auto first = assets.loadModel(sample);
    expect(static_cast<bool>(first), "sample glTF loads");
    const auto second = assets.loadModel(sample);
    expect(static_cast<bool>(second) && first.handle == second.handle, "normalized path cache returns the same handle");
    const auto missing = assets.loadModel(sample.parent_path() / "missing.gltf");
    expect(!missing && !missing.error.empty(), "missing model returns a diagnostic");
    const auto map = assets.loadModel(sample.parent_path() / "basic_map.gltf");
    expect(static_cast<bool>(map), "basic map glTF loads");
    const ModelAsset* mapModel = assets.model(map.handle);
    expect(mapModel && mapModel->nodes.size() == 8, "basic map geometry remains asset-driven");
    const auto glbPath = writeGlbFixture();
    AssetManager glbAssets;
    expect(static_cast<bool>(glbAssets.loadModel(glbPath)), "binary GLB loads through the same importer");
    std::error_code removeError;
    std::filesystem::remove(glbPath, removeError);

    const ModelAsset* model = assets.model(first.handle);
    expect(model && model->nodes.size() == 1, "model hierarchy is imported");
    const MeshAsset* mesh = model && !model->nodes.empty() ? assets.mesh(model->nodes[0].mesh) : nullptr;
    expect(mesh && mesh->vertices.size() == 4 && mesh->indices.size() == 12, "geometry and indices are imported");
    if (mesh) {
        bool normalsValid = true;
        for (const Vertex& vertex : mesh->vertices) {
            normalsValid &= glm::dot(vertex.normal, vertex.normal) > 0.9f;
        }
        expect(normalsValid, "missing normals are generated and normalized");
        const MaterialAsset* material = assets.material(mesh->primitives.front().material);
        expect(material && std::abs(material->baseColor.b - 0.95f) < 0.001f, "base-color factor is imported");
    }

    Scene scene;
    const Entity root = scene.createEntity("Root");
    expect(static_cast<bool>(scene.instantiateModel(root, first.handle, assets)), "model instantiates into the scene");
    scene.updateWorldTransforms();
    const auto models = scene.registry().view<ModelComponent>();
    const auto renderers = scene.registry().view<MeshRendererComponent>();
    expect(std::distance(models.begin(), models.end()) == 1, "root receives a model component");
    expect(std::distance(renderers.begin(), renderers.end()) == 1, "drawable node receives a mesh component");
    scene.destroyEntity(root);
    const auto remaining = scene.registry().view<TransformComponent>();
    expect(std::distance(remaining.begin(), remaining.end()) == 0,
           "destroying a model root cascades to imported children");

    const Entity camera = scene.createEntity("Test Camera");
    const Entity cameraView = scene.createEntity("Test Camera View");
    scene.setParent(cameraView, camera);
    CharacterControllerComponent character;
    character.camera = cameraView;
    scene.registry().emplace<CharacterControllerComponent>(camera, character);
    FirstPersonControllerSystem controllerSystem;
    controllerSystem.updateView(scene, {.look = {100.0f, 20.0f}});
    const auto& controller = scene.registry().get<CharacterControllerComponent>(camera);
    expect(controller.yawRadians < 0.0f && controller.pitchRadians < 0.0f, "mouse input rotates the controlled entity");
    return failures == 0 ? 0 : 1;
}
