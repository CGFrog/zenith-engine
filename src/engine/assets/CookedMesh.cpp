#include "engine/assets/CookedMesh.hpp"
#include "engine/assets/ContentHash.hpp"
#include "engine/platform/FileContents.hpp"

#include <array>
#include <bit>
#include <cstring>
#include <meshoptimizer.h>

namespace {
// Layout, every integer little-endian and every float stored as its bits:
//   header: magic[8], u32 format version, u32 mesh count, u64 payload bytes,
//           u64 payload FNV-1a hash
//   per mesh: u32 present; when present u32 vertex, index, primitive, and
//           morph target counts, then the vertices (20 x u32 each), the
//           indices, the primitives (source primitive, first index, index
//           count, simplified LOD count, three LODs of first index, index
//           count, geometric error), and the morph targets (name length,
//           name bytes, then position, normal, and tangent deltas, each a
//           u32 count and that many float3).
constexpr std::array<uint8_t, 8> magic{'Z', 'M', 'S', 'H', 'C', 'O', 'O', 'K'};
constexpr size_t headerBytes = 32;
constexpr size_t vertexWords = 20;
constexpr size_t primitiveWords = 4 + 3 * (maximumMeshLods - 1);
constexpr size_t meshCountWords = 4;

class ByteWriter {
  public:
    void put32(uint32_t value) {
        for (uint32_t shift = 0; shift < 32; shift += 8) {
            m_bytes.push_back(static_cast<uint8_t>(value >> shift));
        }
    }

    void putFloat(float value) {
        put32(std::bit_cast<uint32_t>(value));
    }

    void putVec(const float* values, size_t count) {
        for (size_t index = 0; index < count; ++index) {
            putFloat(values[index]);
        }
    }

    void putText(std::string_view text) {
        put32(static_cast<uint32_t>(text.size()));
        m_bytes.insert(m_bytes.end(), text.begin(), text.end());
    }

    std::vector<uint8_t>& bytes() {
        return m_bytes;
    }

  private:
    std::vector<uint8_t> m_bytes;
};

// Reads from untrusted bytes. Every read past the end yields zero and marks
// the reader failed; callers check has() before sizing containers from counts.
class ByteReader {
  public:
    explicit ByteReader(std::span<const uint8_t> bytes)
        : m_bytes(bytes) {}

    bool has(uint64_t count) const {
        return !m_failed && count <= m_bytes.size() - m_offset;
    }

    uint32_t get32() {
        if (!has(4)) {
            m_failed = true;
            return 0;
        }
        uint32_t value = 0;
        for (uint32_t shift = 0; shift < 32; shift += 8) {
            value |= uint32_t(m_bytes[m_offset++]) << shift;
        }
        return value;
    }

    float getFloat() {
        return std::bit_cast<float>(get32());
    }

    void getVec(float* values, size_t count) {
        for (size_t index = 0; index < count; ++index) {
            values[index] = getFloat();
        }
    }

    bool getText(std::string& text) {
        const uint32_t length = get32();
        if (!has(length)) {
            m_failed = true;
            return false;
        }
        text.assign(reinterpret_cast<const char*>(m_bytes.data() + m_offset), length);
        m_offset += length;
        return true;
    }

    bool failed() const {
        return m_failed;
    }

    bool atEnd() const {
        return m_offset == m_bytes.size();
    }

  private:
    std::span<const uint8_t> m_bytes;
    size_t m_offset{0};
    bool m_failed{false};
};

uint32_t read32(std::span<const uint8_t> bytes, size_t offset) {
    return uint32_t(bytes[offset]) | (uint32_t(bytes[offset + 1]) << 8u) | (uint32_t(bytes[offset + 2]) << 16u) |
           (uint32_t(bytes[offset + 3]) << 24u);
}

uint64_t read64(std::span<const uint8_t> bytes, size_t offset) {
    return uint64_t(read32(bytes, offset)) | (uint64_t(read32(bytes, offset + 4)) << 32u);
}

void write64(std::vector<uint8_t>& bytes, size_t offset, uint64_t value) {
    for (size_t index = 0; index < 8; ++index) {
        bytes[offset + index] = static_cast<uint8_t>(value >> (8u * index));
    }
}

// Float settings enter the key as their exact bits.
std::string settingBits(float value) {
    return formatContentHash(std::bit_cast<uint32_t>(value));
}

void writeVertex(ByteWriter& writer, const Vertex& vertex) {
    writer.putVec(&vertex.position.x, 3);
    writer.putVec(&vertex.normal.x, 3);
    writer.putVec(&vertex.texCoord.x, 2);
    writer.putVec(&vertex.tangent.x, 4);
    for (int component = 0; component < 4; ++component) {
        writer.put32(vertex.joints[component]);
    }
    writer.putVec(&vertex.weights.x, 4);
}

Vertex readVertex(ByteReader& reader) {
    Vertex vertex;
    reader.getVec(&vertex.position.x, 3);
    reader.getVec(&vertex.normal.x, 3);
    reader.getVec(&vertex.texCoord.x, 2);
    reader.getVec(&vertex.tangent.x, 4);
    for (int component = 0; component < 4; ++component) {
        vertex.joints[component] = reader.get32();
    }
    reader.getVec(&vertex.weights.x, 4);
    return vertex;
}

void writeMesh(ByteWriter& writer, const CookedMesh& cooked) {
    const MeshAsset& mesh = cooked.mesh;
    writer.put32(static_cast<uint32_t>(mesh.vertices.size()));
    writer.put32(static_cast<uint32_t>(mesh.indices.size()));
    writer.put32(static_cast<uint32_t>(mesh.primitives.size()));
    writer.put32(static_cast<uint32_t>(mesh.morphTargets.size()));
    for (const Vertex& vertex : mesh.vertices) {
        writeVertex(writer, vertex);
    }
    for (const uint32_t index : mesh.indices) {
        writer.put32(index);
    }
    for (size_t primitiveIndex = 0; primitiveIndex < mesh.primitives.size(); ++primitiveIndex) {
        const MeshPrimitive& primitive = mesh.primitives[primitiveIndex];
        writer.put32(primitiveIndex < cooked.sourcePrimitives.size() ? cooked.sourcePrimitives[primitiveIndex] : 0);
        writer.put32(primitive.firstIndex);
        writer.put32(primitive.indexCount);
        writer.put32(primitive.simplifiedLodCount);
        for (const MeshLod& lod : primitive.simplifiedLods) {
            writer.put32(lod.firstIndex);
            writer.put32(lod.indexCount);
            writer.putFloat(lod.geometricError);
        }
    }
    for (const MorphTarget& target : mesh.morphTargets) {
        writer.putText(target.name);
        for (const std::vector<glm::vec3>* deltas :
             {&target.positionDeltas, &target.normalDeltas, &target.tangentDeltas}) {
            writer.put32(static_cast<uint32_t>(deltas->size()));
            for (const glm::vec3& delta : *deltas) {
                writer.putVec(&delta.x, 3);
            }
        }
    }
}

bool rangeFits(uint32_t first, uint32_t count, size_t size) {
    return uint64_t(first) + count <= size;
}

// Returns an error when the decoded mesh's ranges would address past its
// buffers, so a damaged entry can never reach the renderer.
std::string meshRangeError(const MeshAsset& mesh) {
    for (const uint32_t index : mesh.indices) {
        if (index >= mesh.vertices.size()) {
            return "an index addresses past the vertices";
        }
    }
    for (const MeshPrimitive& primitive : mesh.primitives) {
        if (!rangeFits(primitive.firstIndex, primitive.indexCount, mesh.indices.size()) ||
            primitive.simplifiedLodCount >= maximumMeshLods) {
            return "a primitive range does not fit the indices";
        }
        for (uint32_t level = 0; level < primitive.simplifiedLodCount; ++level) {
            const MeshLod& lod = primitive.simplifiedLods[level];
            if (!rangeFits(lod.firstIndex, lod.indexCount, mesh.indices.size())) {
                return "a LOD range does not fit the indices";
            }
        }
    }
    return {};
}

std::string readMesh(ByteReader& reader, CookedMesh& cooked) {
    MeshAsset& mesh = cooked.mesh;
    const uint32_t vertexCount = reader.get32();
    const uint32_t indexCount = reader.get32();
    const uint32_t primitiveCount = reader.get32();
    const uint32_t morphTargetCount = reader.get32();
    const uint64_t fixedWords =
        uint64_t(vertexCount) * vertexWords + indexCount + uint64_t(primitiveCount) * primitiveWords;
    if (!reader.has(fixedWords * 4)) {
        return "mesh counts exceed the entry";
    }
    mesh.vertices.resize(vertexCount);
    for (Vertex& vertex : mesh.vertices) {
        vertex = readVertex(reader);
    }
    mesh.indices.resize(indexCount);
    for (uint32_t& index : mesh.indices) {
        index = reader.get32();
    }
    mesh.primitives.resize(primitiveCount);
    cooked.sourcePrimitives.resize(primitiveCount);
    for (uint32_t primitiveIndex = 0; primitiveIndex < primitiveCount; ++primitiveIndex) {
        MeshPrimitive& primitive = mesh.primitives[primitiveIndex];
        cooked.sourcePrimitives[primitiveIndex] = reader.get32();
        primitive.firstIndex = reader.get32();
        primitive.indexCount = reader.get32();
        primitive.simplifiedLodCount = reader.get32();
        for (MeshLod& lod : primitive.simplifiedLods) {
            lod.firstIndex = reader.get32();
            lod.indexCount = reader.get32();
            lod.geometricError = reader.getFloat();
        }
    }
    for (uint32_t targetIndex = 0; targetIndex < morphTargetCount; ++targetIndex) {
        MorphTarget target;
        if (!reader.getText(target.name)) {
            return "morph targets exceed the entry";
        }
        for (std::vector<glm::vec3>* deltas : {&target.positionDeltas, &target.normalDeltas, &target.tangentDeltas}) {
            const uint32_t deltaCount = reader.get32();
            if (deltaCount > vertexCount || !reader.has(uint64_t(deltaCount) * 3 * 4)) {
                return "morph targets exceed the entry";
            }
            deltas->resize(deltaCount);
            for (glm::vec3& delta : *deltas) {
                reader.getVec(&delta.x, 3);
            }
        }
        mesh.morphTargets.push_back(std::move(target));
    }
    return reader.failed() ? "the entry is truncated" : meshRangeError(mesh);
}
} // namespace

std::string cookedMeshToolVersion() {
    return "zenith-mesh-cook-" + std::to_string(cookedMeshFormatVersion) + "-meshopt-" +
           std::to_string(MESHOPTIMIZER_VERSION);
}

DerivedAssetRequest cookedMeshRequest(const std::filesystem::path& model, const MeshOptimizationSettings& settings,
                                      std::string_view bufferFingerprint) {
    std::string importSettings = "mesh;lodTargetRatio=" + settingBits(settings.lodTargetRatio) +
                                 ";maximumLodRetainedRatio=" + settingBits(settings.maximumLodRetainedRatio) +
                                 ";maximumLodRelativeError=" + settingBits(settings.maximumLodRelativeError) +
                                 ";minimumLodSourceIndices=" + std::to_string(settings.minimumLodSourceIndices) +
                                 ";buffers=" + std::string(bufferFingerprint);
    return {.source = model,
            .importSettings = std::move(importSettings),
            .toolVersion = cookedMeshToolVersion(),
            .targetPlatform = std::string(cookedMeshPlatform)};
}

std::vector<uint8_t> writeCookedModelMeshes(const CookedModelMeshes& model) {
    ByteWriter writer;
    writer.bytes().assign(headerBytes, 0);
    for (const std::optional<CookedMesh>& mesh : model.meshes) {
        writer.put32(mesh ? 1u : 0u);
        if (mesh) {
            writeMesh(writer, *mesh);
        }
    }
    std::vector<uint8_t>& bytes = writer.bytes();
    const std::span<const uint8_t> payload(bytes.data() + headerBytes, bytes.size() - headerBytes);
    std::memcpy(bytes.data(), magic.data(), magic.size());
    const uint32_t meshCount = static_cast<uint32_t>(model.meshes.size());
    for (size_t index = 0; index < 4; ++index) {
        bytes[8 + index] = static_cast<uint8_t>(cookedMeshFormatVersion >> (8u * index));
        bytes[12 + index] = static_cast<uint8_t>(meshCount >> (8u * index));
    }
    write64(bytes, 16, payload.size());
    write64(bytes, 24, contentHash(payload));
    return std::move(bytes);
}

CookedMeshDecodeResult readCookedModelMeshes(std::span<const uint8_t> bytes) {
    if (bytes.size() < headerBytes || std::memcmp(bytes.data(), magic.data(), magic.size()) != 0) {
        return {.error = "not a cooked mesh entry"};
    }
    if (const uint32_t version = read32(bytes, 8); version != cookedMeshFormatVersion) {
        return {.error = "cooked mesh format version " + std::to_string(version) + " is not the supported version " +
                         std::to_string(cookedMeshFormatVersion)};
    }
    const std::span<const uint8_t> payload = bytes.subspan(headerBytes);
    if (read64(bytes, 16) != payload.size() || read64(bytes, 24) != contentHash(payload)) {
        return {.error = "cooked mesh entry is damaged (size or checksum mismatch)"};
    }
    const uint32_t meshCount = read32(bytes, 12);
    if (uint64_t(meshCount) * 4 > payload.size()) {
        return {.error = "cooked mesh count exceeds the entry"};
    }
    ByteReader reader(payload);
    CookedMeshDecodeResult result;
    result.model.meshes.resize(meshCount);
    for (std::optional<CookedMesh>& mesh : result.model.meshes) {
        const uint32_t present = reader.get32();
        if (present > 1 || (present == 1 && !reader.has(meshCountWords * 4))) {
            return {.error = "cooked mesh entry is malformed"};
        }
        if (present == 1) {
            mesh.emplace();
            if (const std::string error = readMesh(reader, *mesh); !error.empty()) {
                return {.error = "cooked mesh entry is malformed: " + error};
            }
        }
    }
    if (reader.failed() || !reader.atEnd()) {
        return {.error = "cooked mesh entry is malformed: unexpected length"};
    }
    return result;
}

CookedMeshLookup loadCookedModelMeshes(const std::filesystem::path& derivedRoot, const DerivedAssetRequest& request) {
    const DerivedAssetCache cache(derivedRoot);
    std::string key;
    DerivedAssetRecord record;
    if (!cache.key(request, key) || !cache.inspect(key, record)) {
        return {};
    }
    std::vector<uint8_t> bytes;
    if (readFileBytes(record.path, bytes) != FileReadStatus::Success) {
        return {.error = "Cannot read cooked meshes " + record.path.string()};
    }
    CookedMeshDecodeResult decoded = readCookedModelMeshes(bytes);
    if (!decoded) {
        return {.error = "Cooked meshes " + record.path.string() + " are invalid: " + decoded.error};
    }
    return {.model = std::move(decoded.model)};
}

AssetPipelineResult storeCookedModelMeshes(const std::filesystem::path& derivedRoot, const DerivedAssetRequest& request,
                                           const CookedModelMeshes& model) {
    DerivedAssetCache cache(derivedRoot);
    DerivedAssetRecord record;
    return cache.store(request, writeCookedModelMeshes(model), record);
}
