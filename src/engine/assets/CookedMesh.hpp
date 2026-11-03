#pragma once

// Cooked model meshes: the optimized vertices, indices, and LOD ranges of a glTF model's meshes as a versioned CPU
// format stored in the DerivedAssetCache, so imports can skip meshoptimizer.

#include "Assets.hpp"
#include "MeshOptimization.hpp"
#include "ProductionAssetPipeline.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// A cooked entry is a DerivedAssetCache file keyed like a cooked texture:
// the model file's bytes, cookedMeshRequest()'s settings string (the
// MeshOptimizationSettings and a fingerprint of the model's external buffer
// files), the tool version (format version and meshoptimizer version), and
// the "cpu" target. Editing the model or its buffers, changing the settings,
// or changing the format therefore misses the cache. The entry's own header
// repeats the format version and checksums the payload, so a stale or damaged
// file is rejected rather than trusted.

// Increment when the serialized layout changes, or when the importer's
// geometry conversion or optimization changes its output.
inline constexpr uint32_t cookedMeshFormatVersion = 1;
inline constexpr std::string_view cookedMeshPlatform = "cpu";

// One glTF mesh after import and optimizeMesh. Materials are not cooked:
// mesh.primitives[i].material is left empty, and sourcePrimitives[i] names
// the glTF primitive it came from so the importer can assign its material.
// mesh.bounds and mesh.localBox are not stored; they are recomputed from the
// vertices.
struct CookedMesh {
    MeshAsset mesh;
    std::vector<uint32_t> sourcePrimitives;
};

// A model's meshes by glTF mesh index; empty where the glTF mesh has no
// importable triangle primitive.
struct CookedModelMeshes {
    std::vector<std::optional<CookedMesh>> meshes;
};

// The tool version string of the cache key: the format version and the
// meshoptimizer release that produced the optimization.
std::string cookedMeshToolVersion();

// The derived-asset request for a model's cooked meshes. bufferFingerprint
// identifies the content of the model's external buffer files (the model file
// itself is hashed by the cache key).
DerivedAssetRequest cookedMeshRequest(const std::filesystem::path& model, const MeshOptimizationSettings& settings,
                                      std::string_view bufferFingerprint);

// Serializes cooked meshes with a versioned, checksummed header.
std::vector<uint8_t> writeCookedModelMeshes(const CookedModelMeshes& model);

struct CookedMeshDecodeResult {
    CookedModelMeshes model;
    std::string error;

    explicit operator bool() const {
        return error.empty();
    }
};

// Reads writeCookedModelMeshes output. Fails, without allocating from
// untrusted counts, on a wrong magic or version, a checksum mismatch,
// truncation, or index and LOD ranges that do not fit their buffers.
CookedMeshDecodeResult readCookedModelMeshes(std::span<const uint8_t> bytes);

struct CookedMeshLookup {
    // Set when a valid entry exists for the request.
    std::optional<CookedModelMeshes> model;
    // Set when an entry exists but cannot be used. A missing entry is not an
    // error; the caller cooks the source instead.
    std::string error;
};

CookedMeshLookup loadCookedModelMeshes(const std::filesystem::path& derivedRoot, const DerivedAssetRequest& request);

// Writes (or replaces) the entry for request.
AssetPipelineResult storeCookedModelMeshes(const std::filesystem::path& derivedRoot, const DerivedAssetRequest& request,
                                           const CookedModelMeshes& model);
