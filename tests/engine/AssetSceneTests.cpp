#include "engine/assets/Assets.hpp"
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

std::filesystem::path writeAnimatedGlbFixture() {
    std::string json = R"({
"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0,1]}],
"nodes":[{"name":"Joint"},{"name":"Mesh","mesh":0,"skin":0}],
"materials":[{"alphaMode":"MASK","alphaCutoff":0.35,"doubleSided":true}],
"meshes":[{"primitives":[{"attributes":{"POSITION":0,"JOINTS_0":1,"WEIGHTS_0":2},"indices":3,"material":0}]}],
"skins":[{"joints":[0],"skeleton":0,"inverseBindMatrices":4}],
"animations":[{"name":"Move","samplers":[{"input":5,"output":6,"interpolation":"LINEAR"}],"channels":[{"sampler":0,"target":{"node":0,"path":"translation"}}]}],
"buffers":[{"byteLength":200}],
"bufferViews":[
{"buffer":0,"byteOffset":0,"byteLength":36},
{"buffer":0,"byteOffset":36,"byteLength":12},
{"buffer":0,"byteOffset":48,"byteLength":48},
{"buffer":0,"byteOffset":96,"byteLength":6},
{"buffer":0,"byteOffset":104,"byteLength":64},
{"buffer":0,"byteOffset":168,"byteLength":8},
{"buffer":0,"byteOffset":176,"byteLength":24}],
"accessors":[
{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},
{"bufferView":1,"componentType":5121,"count":3,"type":"VEC4"},
{"bufferView":2,"componentType":5126,"count":3,"type":"VEC4"},
{"bufferView":3,"componentType":5123,"count":3,"type":"SCALAR"},
{"bufferView":4,"componentType":5126,"count":1,"type":"MAT4"},
{"bufferView":5,"componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[1]},
{"bufferView":6,"componentType":5126,"count":2,"type":"VEC3"}]})";
    while (json.size() % 4 != 0) {
        json.push_back(' ');
    }
    const float positions[]{0, 0, 0, 1, 0, 0, 0, 1, 0};
    const uint8_t joints[]{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const float weights[]{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    const uint16_t indices[]{0, 1, 2};
    const uint16_t padding = 0;
    const float identity[]{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float times[]{0, 1};
    const float translations[]{0, 0, 0, 2, 0, 0};
    constexpr uint32_t binarySize = 200;
    const uint32_t totalSize = 12 + 8 + static_cast<uint32_t>(json.size()) + 8 + binarySize;
    const auto path = std::filesystem::temp_directory_path() / "zenith_animated_loading_test.glb";
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
    stream.write(reinterpret_cast<const char*>(joints), sizeof(joints));
    stream.write(reinterpret_cast<const char*>(weights), sizeof(weights));
    stream.write(reinterpret_cast<const char*>(indices), sizeof(indices));
    stream.write(reinterpret_cast<const char*>(&padding), sizeof(padding));
    stream.write(reinterpret_cast<const char*>(identity), sizeof(identity));
    stream.write(reinterpret_cast<const char*>(times), sizeof(times));
    stream.write(reinterpret_cast<const char*>(translations), sizeof(translations));
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
    const MeshAsset* mapMesh =
        mapModel && !mapModel->nodes.empty() ? assets.mesh(mapModel->nodes.front().mesh) : nullptr;
    expect(mapMesh && mapMesh->bounds.radius > 0.0f, "imported meshes have a usable culling bound");
    const MaterialAsset* mapMaterial =
        mapMesh && !mapMesh->primitives.empty() ? assets.material(mapMesh->primitives.front().material) : nullptr;
    expect(mapMaterial && mapMaterial->baseColorTexture && mapMaterial->normalTexture &&
               mapMaterial->metallicRoughnessTexture,
           "glTF PBR texture bindings are imported");
    const TextureAsset* mapBaseColor = mapMaterial ? assets.texture(mapMaterial->baseColorTexture) : nullptr;
    expect(mapBaseColor && mapBaseColor->width == 1024 && mapBaseColor->height == 1024 &&
               mapBaseColor->rgba.size() == 1024 * 1024 * 4,
           "external PBR image is decoded to RGBA pixels");
    if (mapMesh) {
        bool tangentSpaceValid = true;
        for (const Vertex& vertex : mapMesh->vertices) {
            tangentSpaceValid &= glm::dot(glm::vec3(vertex.tangent), glm::vec3(vertex.tangent)) > 0.9f;
        }
        expect(tangentSpaceValid, "missing tangents are generated for textured primitives");
    }
    const auto glbPath = writeGlbFixture();
    AssetManager glbAssets;
    expect(static_cast<bool>(glbAssets.loadModel(glbPath)), "binary GLB loads through the same importer");
    std::error_code removeError;
    std::filesystem::remove(glbPath, removeError);

    const auto animatedPath = writeAnimatedGlbFixture();
    AssetManager animatedAssets;
    const auto animated = animatedAssets.loadModel(animatedPath);
    expect(static_cast<bool>(animated), "animated and skinned GLB loads");
    const ModelAsset* animatedModel = animatedAssets.model(animated.handle);
    expect(animatedModel && animatedModel->skins.size() == 1 && animatedModel->skins[0].joints.size() == 1,
           "skin joints and inverse-bind matrices are imported");
    expect(animatedModel && animatedModel->animations.size() == 1 &&
               animatedModel->animations[0].channels.size() == 1 &&
               std::abs(animatedModel->animations[0].duration - 1.0f) < 0.001f,
           "animation clips, channels, and duration are imported");
    const MeshAsset* animatedMesh = animatedModel ? animatedAssets.mesh(animatedModel->nodes[1].mesh) : nullptr;
    expect(animatedMesh && animatedMesh->vertices.front().joints.x == 0 &&
               std::abs(animatedMesh->vertices.front().weights.x - 1.0f) < 0.001f,
           "joint indices and normalized weights are imported");
    const MaterialAsset* animatedMaterial =
        animatedMesh ? animatedAssets.material(animatedMesh->primitives.front().material) : nullptr;
    expect(animatedMaterial && animatedMaterial->alphaMode == MaterialAlphaMode::Mask &&
               std::abs(animatedMaterial->alphaCutoff - 0.35f) < 0.001f && animatedMaterial->doubleSided,
           "alpha mode, cutoff, and double-sided state are imported");
    std::filesystem::remove(animatedPath, removeError);

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

    return failures == 0 ? 0 : 1;
}
