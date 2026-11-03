#pragma once

// glTF 2.0 import (cgltf) into CPU models, meshes, and materials; model outlines for tools.

#include "Assets.hpp"
#include "MeshOptimization.hpp"

class ModelImporter {
  public:
    explicit ModelImporter(AssetManager& assets)
        : m_assets(assets) {}

    AssetLoadResult<ModelHandle> load(const std::filesystem::path& path);

  private:
    AssetManager& m_assets;
};

// How a model's optimized meshes were obtained.
enum class MeshCookOutcome : uint8_t {
    // Read from a current cooked entry; meshoptimizer did not run.
    Cached,
    // Imported from the source and optimized (and, with a derived root,
    // written to the cache).
    Cooked,
};

struct ModelMeshCookResult {
    MeshCookOutcome outcome{MeshCookOutcome::Cooked};
    bool entryWritten{false};
    // Diagnostics that did not fail the cook: skipped primitives, a rejected
    // (stale or damaged) entry that was replaced, or an entry that could not
    // be written.
    std::vector<std::string> warnings;
    std::string error;

    explicit operator bool() const {
        return error.empty();
    }
};

// Cooks a glTF model's optimized meshes and LOD chains into the derived-asset
// cache at derivedRoot (see CookedMesh.hpp) unless a current entry exists, as
// a model import with AssetManager::setCookedMeshRoot would. Loads buffers
// but no images; zenith_asset_build uses it to cook at build time.
ModelMeshCookResult cookModelMeshes(const std::filesystem::path& modelPath, const std::filesystem::path& derivedRoot,
                                    const MeshOptimizationSettings& settings = {});

struct ModelTextureSource {
    std::filesystem::path path;
    TextureUsage usage{TextureUsage::Color};
};

struct ModelTextureSourcesResult {
    std::vector<ModelTextureSource> sources;
    std::string error;

    explicit operator bool() const {
        return error.empty();
    }
};

// Lists a glTF model's external image files with the usage its materials give
// them. Embedded and data-URI images have no source file to cook and are
// omitted, as are normal maps also referenced as color or data (one cooked
// format cannot serve both, so those import as RGBA8).
ModelTextureSourcesResult collectModelTextureSources(const std::filesystem::path& modelPath);

// One triangle primitive of a glTF mesh: the material it draws with.
struct ModelOutlinePrimitive {
    // Index into ModelOutline::materials; -1 for the default material.
    int32_t material{-1};
};

// A glTF mesh as the importer turns it into a MeshAsset.
struct ModelOutlineMesh {
    std::string name;
    // The triangle primitives, in MeshAsset::primitives order (the importer
    // skips other primitive types).
    std::vector<ModelOutlinePrimitive> primitives;
    // Some primitive has joints and weights, so the mesh is drawn skinned.
    bool skinned{false};
    uint32_t morphTargetCount{0};
};

// What a model contains, by name, for tools that let users choose a mesh:
// its meshes by glTF index (AssetManager::loadMesh's meshIndex) and the names
// of the materials their primitives use. Names are empty where the file has
// none.
struct ModelOutline {
    std::vector<ModelOutlineMesh> meshes;
    std::vector<std::string> materials;
    std::string error;

    explicit operator bool() const {
        return error.empty();
    }
};

// Reads a glTF model's outline from its JSON alone, without loading buffers
// or images, so an inspector can afford it on demand.
ModelOutline readModelOutline(const std::filesystem::path& modelPath);
