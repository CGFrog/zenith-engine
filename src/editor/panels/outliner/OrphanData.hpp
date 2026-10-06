#pragma once

// Orphan Data mode: project assets the scene does not use.

#include "editor/project/AssetCatalog.hpp"
#include "engine/scene/Collections.hpp"

#include <filesystem>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class Scene;

// The outliner's Orphan Data view: project assets the open scene does not
// reference, and collections nothing is linked to. Everything here is pure, so
// it runs without a window; the panel caches the results per document
// revision because finding asset references serializes the scene.

// The asset references found in a scene document.
struct SceneAssetUsage {
    // Project-relative paths with forward slashes, as documents store them.
    std::set<std::string> paths;
    std::set<AssetGuid> guids;
};

// Collects every asset reference of a serialized scene: each `path` string
// and `guid` inside the entities' component objects, at any depth (the key
// `path` is reserved for asset references; see SceneAssetReferences.hpp).
// Returns false with error set when the document is not JSON with an entities
// array.
bool collectSceneAssetUsage(std::string_view document, SceneAssetUsage& usage, std::string& error);

struct OrphanAsset {
    // Relative to assetRoot, with forward slashes.
    std::string path;
    EditorAssetKind kind{EditorAssetKind::Model};
};

// The models, textures, and audio clips under assetRoot that usage names
// neither by path nor by GUID, sorted by path. Scenes are never orphans: a
// scene is opened, not referenced.
std::vector<OrphanAsset> findOrphanAssets(std::span<const EditorAssetRecord> assets,
                                          const std::filesystem::path& assetRoot, const SceneAssetUsage& usage);

// Collections no entity is linked to, directly or through a collection nested
// inside them, in ascending id order.
std::vector<SceneCollectionId> findEmptyCollections(const Scene& scene);
