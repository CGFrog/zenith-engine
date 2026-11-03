// Cooked model meshes in the derived-asset cache: misses cook and write an
// entry, hits skip meshoptimizer and reproduce a fresh cook exactly, and a
// changed source, settings, or format version or a damaged entry falls back
// to cooking and rewrites the entry.

#include "engine/assets/Assets.hpp"
#include "engine/assets/CookedMesh.hpp"
#include "engine/assets/ModelImporter.hpp"

#include "support/Doctest.hpp"
#include "support/Files.hpp"

#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

TEST_SUITE_BEGIN("CookedMesh");

namespace {

constexpr uint32_t gridQuads = 24;

void appendFloats(std::vector<uint8_t>& bytes, std::initializer_list<float> values) {
    for (const float value : values) {
        uint8_t raw[4];
        std::memcpy(raw, &value, 4);
        bytes.insert(bytes.end(), raw, raw + 4);
    }
}

void appendIndex(std::vector<uint8_t>& bytes, uint32_t value) {
    uint8_t raw[4];
    std::memcpy(raw, &value, 4);
    bytes.insert(bytes.end(), raw, raw + 4);
}

// Writes model.gltf and its external model.bin: a gently waved grid large
// enough to get simplified LODs, drawn by a point primitive the importer skips
// and a triangle primitive with a material, so cooked primitives must keep
// their glTF primitive index to get the right material back.
std::filesystem::path writeGridModel(const std::filesystem::path& directory, float waveHeight = 0.05f,
                                     float red = 0.25f) {
    constexpr uint32_t side = gridQuads + 1;
    constexpr uint32_t vertexCount = side * side;
    std::vector<uint8_t> positions;
    std::vector<uint8_t> normals;
    std::vector<uint8_t> texCoords;
    float minimumHeight = 0.0f;
    float maximumHeight = 0.0f;
    for (uint32_t row = 0; row < side; ++row) {
        for (uint32_t column = 0; column < side; ++column) {
            const float x = static_cast<float>(column) / gridQuads * 2.0f - 1.0f;
            const float z = static_cast<float>(row) / gridQuads * 2.0f - 1.0f;
            const float y = waveHeight * std::sin(x * 3.0f) * std::cos(z * 2.0f);
            minimumHeight = std::min(minimumHeight, y);
            maximumHeight = std::max(maximumHeight, y);
            appendFloats(positions, {x, y, z});
            appendFloats(normals, {0.0f, 1.0f, 0.0f});
            appendFloats(texCoords, {(x + 1.0f) * 0.5f, (z + 1.0f) * 0.5f});
        }
    }
    std::vector<uint8_t> indices;
    for (uint32_t row = 0; row < gridQuads; ++row) {
        for (uint32_t column = 0; column < gridQuads; ++column) {
            const uint32_t a = row * side + column;
            const uint32_t b = a + side;
            for (const uint32_t index : {a, b, a + 1, a + 1, b, b + 1}) {
                appendIndex(indices, index);
            }
        }
    }
    const uint32_t indexCount = static_cast<uint32_t>(indices.size() / 4);
    std::vector<uint8_t> buffer = positions;
    buffer.insert(buffer.end(), normals.begin(), normals.end());
    buffer.insert(buffer.end(), texCoords.begin(), texCoords.end());
    buffer.insert(buffer.end(), indices.begin(), indices.end());
    testsupport::writeBytes(directory / "model.bin", buffer);

    const auto text = [](auto value) { return std::to_string(value); };
    const std::string json =
        R"({"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}],
  "nodes": [{"mesh": 0}],
  "materials": [{"pbrMetallicRoughness": {"baseColorFactor": [)" +
        text(red) + R"(, 0.5, 0.75, 1.0]}}],
  "meshes": [{"primitives": [
    {"attributes": {"POSITION": 0}, "mode": 0},
    {"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "material": 0}]}],
  "buffers": [{"uri": "model.bin", "byteLength": )" +
        text(buffer.size()) + R"(}],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": )" +
        text(positions.size()) + R"(},
    {"buffer": 0, "byteOffset": )" +
        text(positions.size()) + R"(, "byteLength": )" + text(normals.size()) + R"(},
    {"buffer": 0, "byteOffset": )" +
        text(positions.size() + normals.size()) + R"(, "byteLength": )" + text(texCoords.size()) + R"(},
    {"buffer": 0, "byteOffset": )" +
        text(positions.size() + normals.size() + texCoords.size()) + R"(, "byteLength": )" + text(indices.size()) +
        R"(}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": )" +
        text(vertexCount) + R"(, "type": "VEC3", "min": [-1, )" + text(minimumHeight) + R"(, -1], "max": [1, )" +
        text(maximumHeight) + R"(, 1]},
    {"bufferView": 1, "componentType": 5126, "count": )" +
        text(vertexCount) + R"(, "type": "VEC3"},
    {"bufferView": 2, "componentType": 5126, "count": )" +
        text(vertexCount) + R"(, "type": "VEC2"},
    {"bufferView": 3, "componentType": 5125, "count": )" +
        text(indexCount) + R"(, "type": "SCALAR"}]})";
    const std::filesystem::path model = directory / "model.gltf";
    testsupport::writeText(model, json);
    return model;
}

const MeshAsset* firstMesh(const AssetManager& assets, ModelHandle model) {
    const ModelAsset* asset = assets.model(model);
    return asset && !asset->nodes.empty() ? assets.mesh(asset->nodes[0].mesh) : nullptr;
}

// Loads the model with a fresh manager reading and writing derivedRoot (or
// without a cache for an empty root), and keeps the manager for inspection.
struct ModelLoad {
    AssetManager assets;
    const MeshAsset* mesh{nullptr};

    ModelLoad(const std::filesystem::path& model, const std::filesystem::path& derivedRoot) {
        assets.setCookedMeshRoot(derivedRoot);
        const AssetLoadResult<ModelHandle> loaded = assets.loadModel(model);
        REQUIRE_MESSAGE(loaded, loaded.error);
        mesh = firstMesh(assets, loaded.handle);
        REQUIRE(mesh != nullptr);
    }

    const MeshCookStatistics& statistics() const {
        return assets.meshCookStatistics();
    }
};

std::vector<std::filesystem::path> cacheEntries(const std::filesystem::path& derivedRoot) {
    std::vector<std::filesystem::path> entries;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(derivedRoot, error)) {
        if (entry.path().extension() == ".zderived") {
            entries.push_back(entry.path());
        }
    }
    return entries;
}

std::vector<uint8_t> vertexBytes(const MeshAsset& mesh) {
    std::vector<uint8_t> bytes(mesh.vertices.size() * sizeof(Vertex));
    std::memcpy(bytes.data(), mesh.vertices.data(), bytes.size());
    return bytes;
}

// Checks that two imports of the model hold identical mesh data.
void checkSameMesh(const ModelLoad& expected, const ModelLoad& actual) {
    const MeshAsset& left = *expected.mesh;
    const MeshAsset& right = *actual.mesh;
    CHECK(vertexBytes(left) == vertexBytes(right));
    CHECK(left.indices == right.indices);
    REQUIRE(left.primitives.size() == right.primitives.size());
    for (size_t index = 0; index < left.primitives.size(); ++index) {
        CAPTURE(index);
        const MeshPrimitive& a = left.primitives[index];
        const MeshPrimitive& b = right.primitives[index];
        CHECK(a.firstIndex == b.firstIndex);
        CHECK(a.indexCount == b.indexCount);
        REQUIRE(a.lodCount() == b.lodCount());
        for (uint32_t level = 1; level < a.lodCount(); ++level) {
            const MeshLod leftLod = a.lod(level);
            const MeshLod rightLod = b.lod(level);
            CHECK(leftLod.firstIndex == rightLod.firstIndex);
            CHECK(leftLod.indexCount == rightLod.indexCount);
            CHECK(std::bit_cast<uint32_t>(leftLod.geometricError) == std::bit_cast<uint32_t>(rightLod.geometricError));
        }
        const MaterialAsset* leftMaterial = expected.assets.material(a.material);
        const MaterialAsset* rightMaterial = actual.assets.material(b.material);
        REQUIRE(leftMaterial != nullptr);
        REQUIRE(rightMaterial != nullptr);
        CHECK(leftMaterial->baseColor == rightMaterial->baseColor);
    }
    CHECK(left.bounds.center == right.bounds.center);
    CHECK(left.bounds.radius == right.bounds.radius);
}

void patchByte(const std::filesystem::path& file, size_t offset, uint8_t value) {
    std::vector<uint8_t> bytes = testsupport::readBytes(file);
    REQUIRE(offset < bytes.size());
    bytes[offset] = value;
    testsupport::writeBytes(file, bytes);
}

} // namespace

TEST_CASE("CookedMesh: a cache miss cooks the meshes and writes an entry") {
    const testsupport::TempDirectory scratch("cooked-mesh-miss");
    const std::filesystem::path model = writeGridModel(scratch.path());
    const std::filesystem::path derived = scratch / "derived";

    const ModelLoad load(model, derived);

    CHECK(load.statistics().cookedModels == 1);
    CHECK(load.statistics().cachedModels == 0);
    CHECK(load.statistics().optimizedMeshes == 1);
    CHECK(load.statistics().entriesWritten == 1);
    CHECK(cacheEntries(derived).size() == 1);
    REQUIRE(load.mesh->primitives.size() == 1);
    CHECK_MESSAGE(load.mesh->primitives[0].lodCount() > 1, "the grid is large enough to get simplified LODs");
}

TEST_CASE("CookedMesh: a cache hit skips meshoptimizer and matches a fresh cook") {
    const testsupport::TempDirectory scratch("cooked-mesh-hit");
    const std::filesystem::path model = writeGridModel(scratch.path());
    const std::filesystem::path derived = scratch / "derived";
    const ModelLoad uncached(model, {});
    const ModelLoad cooked(model, derived);

    const ModelLoad cached(model, derived);

    CHECK(cached.statistics().cachedModels == 1);
    CHECK(cached.statistics().cookedModels == 0);
    CHECK_MESSAGE(cached.statistics().optimizedMeshes == 0, "a hit never runs optimizeMesh");
    CHECK(cached.statistics().entriesWritten == 0);
    CHECK(uncached.statistics().entriesWritten == 0);
    checkSameMesh(uncached, cooked);
    checkSameMesh(uncached, cached);
    const MaterialAsset* material = cached.assets.material(cached.mesh->primitives[0].material);
    REQUIRE(material != nullptr);
    CHECK_MESSAGE(material->baseColor.r == doctest::Approx(0.25f),
                  "the triangle primitive keeps its own material, not the skipped point primitive's default");
}

TEST_CASE("CookedMesh: a changed model or buffer file misses the entry") {
    const testsupport::TempDirectory scratch("cooked-mesh-source");
    const std::filesystem::path derived = scratch / "derived";
    const std::filesystem::path model = writeGridModel(scratch.path());
    {
        const ModelLoad first(model, derived);
    }

    SUBCASE("the buffer file") {
        // Nudges the first vertex's height; the glTF file is unchanged.
        const std::vector<uint8_t> buffer = testsupport::readBytes(scratch / "model.bin");
        patchByte(scratch / "model.bin", 4, static_cast<uint8_t>(buffer[4] ^ 0x10));
    }
    SUBCASE("the glTF file") {
        writeGridModel(scratch.path(), 0.05f, 0.5f);
    }
    const ModelLoad changed(model, derived);

    CHECK(changed.statistics().cachedModels == 0);
    CHECK(changed.statistics().optimizedMeshes == 1);
    CHECK(changed.statistics().entriesWritten == 1);
    CHECK(cacheEntries(derived).size() == 2);
    const ModelLoad again(model, derived);
    CHECK(again.statistics().cachedModels == 1);
    checkSameMesh(changed, again);
}

TEST_CASE("CookedMesh: changed optimization settings miss the entry") {
    const testsupport::TempDirectory scratch("cooked-mesh-settings");
    const std::filesystem::path model = writeGridModel(scratch.path());
    const std::filesystem::path derived = scratch / "derived";
    {
        const ModelLoad first(model, derived);
    }

    const ModelMeshCookResult same = cookModelMeshes(model, derived);
    const ModelMeshCookResult coarser = cookModelMeshes(model, derived, {.lodTargetRatio = 0.25f});
    const ModelMeshCookResult coarserAgain = cookModelMeshes(model, derived, {.lodTargetRatio = 0.25f});

    REQUIRE_MESSAGE(same, same.error);
    REQUIRE_MESSAGE(coarser, coarser.error);
    CHECK(same.outcome == MeshCookOutcome::Cached);
    CHECK(coarser.outcome == MeshCookOutcome::Cooked);
    CHECK(coarser.entryWritten);
    CHECK(coarserAgain.outcome == MeshCookOutcome::Cached);
    CHECK(cacheEntries(derived).size() == 2);
}

TEST_CASE("CookedMesh: the format version is part of the key and the entry") {
    const testsupport::TempDirectory scratch("cooked-mesh-version");
    const std::filesystem::path model = writeGridModel(scratch.path());
    const std::filesystem::path derived = scratch / "derived";
    {
        const ModelLoad first(model, derived);
    }
    const std::vector<std::filesystem::path> entries = cacheEntries(derived);
    REQUIRE(entries.size() == 1);

    CHECK(cookedMeshToolVersion().starts_with("zenith-mesh-cook-" + std::to_string(cookedMeshFormatVersion) + "-"));
    DerivedAssetRequest request = cookedMeshRequest(model, {}, "buffers");
    const DerivedAssetCache cache(derived);
    std::string current;
    std::string other;
    REQUIRE(cache.key(request, current));
    request.toolVersion = "zenith-mesh-cook-0";
    REQUIRE(cache.key(request, other));
    CHECK_MESSAGE(current != other, "another tool version names another entry");

    // An entry whose header claims another version is stale, whatever its key.
    patchByte(entries[0], 8, static_cast<uint8_t>(cookedMeshFormatVersion + 1));
    const ModelLoad stale(model, derived);
    CHECK(stale.statistics().rejectedEntries == 1);
    CHECK(stale.statistics().optimizedMeshes == 1);
    CHECK(stale.statistics().entriesWritten == 1);
    const ModelLoad rewritten(model, derived);
    CHECK(rewritten.statistics().cachedModels == 1);
    checkSameMesh(stale, rewritten);
}

TEST_CASE("CookedMesh: a damaged entry falls back to cooking and is rewritten") {
    const testsupport::TempDirectory scratch("cooked-mesh-damaged");
    const std::filesystem::path model = writeGridModel(scratch.path());
    const std::filesystem::path derived = scratch / "derived";
    const ModelLoad fresh(model, {});
    {
        const ModelLoad first(model, derived);
    }
    const std::vector<std::filesystem::path> entries = cacheEntries(derived);
    REQUIRE(entries.size() == 1);
    const std::vector<uint8_t> good = testsupport::readBytes(entries[0]);

    SUBCASE("a flipped payload byte") {
        patchByte(entries[0], good.size() / 2, static_cast<uint8_t>(good[good.size() / 2] ^ 0x5a));
    }
    SUBCASE("a truncated file") {
        testsupport::writeBytes(entries[0], std::span(good).first(good.size() / 3));
    }
    SUBCASE("an empty file") {
        testsupport::writeBytes(entries[0], {});
    }
    const ModelLoad damaged(model, derived);

    CHECK(damaged.statistics().rejectedEntries == 1);
    CHECK(damaged.statistics().optimizedMeshes == 1);
    CHECK(damaged.statistics().entriesWritten == 1);
    checkSameMesh(fresh, damaged);
    CHECK(testsupport::readBytes(entries[0]) == good);
    const ModelLoad rewritten(model, derived);
    CHECK(rewritten.statistics().cachedModels == 1);
}

TEST_CASE("CookedMesh: the format round-trips every stream exactly") {
    CookedMesh cooked;
    for (int index = 0; index < 4; ++index) {
        Vertex vertex;
        vertex.position = {float(index), -0.0f, 0.1f * float(index)};
        vertex.joints = {uint32_t(index), 2u, 3u, 65535u};
        vertex.weights = {0.5f, 0.25f, 0.125f, 0.125f};
        cooked.mesh.vertices.push_back(vertex);
    }
    cooked.mesh.indices = {0, 1, 2, 2, 1, 3, 0, 1, 2};
    MeshPrimitive primitive{.firstIndex = 0, .indexCount = 6};
    primitive.simplifiedLods[0] = {.firstIndex = 6, .indexCount = 3, .geometricError = 0.125f};
    primitive.simplifiedLodCount = 1;
    cooked.mesh.primitives.push_back(primitive);
    cooked.sourcePrimitives.push_back(2);
    cooked.mesh.morphTargets.push_back({.name = "Smile",
                                        .positionDeltas = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}, {0, 0, 1}},
                                        .normalDeltas = {{0, 1, 0}},
                                        .tangentDeltas = {}});
    CookedModelMeshes model;
    model.meshes.emplace_back();
    model.meshes.emplace_back(cooked);

    const std::vector<uint8_t> bytes = writeCookedModelMeshes(model);
    const CookedMeshDecodeResult decoded = readCookedModelMeshes(bytes);

    REQUIRE_MESSAGE(decoded, decoded.error);
    REQUIRE(decoded.model.meshes.size() == 2);
    CHECK_FALSE(decoded.model.meshes[0].has_value());
    REQUIRE(decoded.model.meshes[1].has_value());
    const CookedMesh& read = *decoded.model.meshes[1];
    CHECK(vertexBytes(read.mesh) == vertexBytes(cooked.mesh));
    CHECK(read.sourcePrimitives == cooked.sourcePrimitives);
    REQUIRE(read.mesh.morphTargets.size() == 1);
    CHECK(read.mesh.morphTargets[0].name == "Smile");
    CHECK(read.mesh.morphTargets[0].positionDeltas == cooked.mesh.morphTargets[0].positionDeltas);
    CHECK(read.mesh.morphTargets[0].normalDeltas.size() == 1);
    CHECK(read.mesh.morphTargets[0].tangentDeltas.empty());
    CHECK(read.mesh.primitives[0].lod(1).geometricError == 0.125f);
    CHECK_MESSAGE(writeCookedModelMeshes(decoded.model) == bytes, "reading and writing again changes nothing");
}

TEST_CASE("CookedMesh: an entry whose indices leave the vertices is rejected") {
    CookedMesh cooked;
    cooked.mesh.vertices.resize(3);
    cooked.mesh.indices = {0, 1, 7};
    cooked.mesh.primitives.push_back({.firstIndex = 0, .indexCount = 3});
    cooked.sourcePrimitives.push_back(0);
    CookedModelMeshes model;
    model.meshes.emplace_back(cooked);

    const CookedMeshDecodeResult decoded = readCookedModelMeshes(writeCookedModelMeshes(model));

    CHECK_FALSE(decoded);
    CHECK(decoded.error.find("index") != std::string::npos);
}

TEST_SUITE_END();
