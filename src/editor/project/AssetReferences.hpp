#pragma once

#include "editor/project/AssetCatalog.hpp"
#include "engine/scene/SceneEntityId.hpp"

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// Finding what uses an asset: the project's scenes, prefabs, and materials
// that reference it, for the Asset Browser's Find References and the guard
// that asks before an asset still in use is moved to the trash.

// The assets one document references, as absolute normalized paths
// (normalizedCatalogPath) and GUIDs.
struct FileAssetReferences {
    std::set<std::filesystem::path> paths;
    std::set<AssetGuid> guids;
};

// Reads what one file of the given kind references:
// - a scene or prefab: every asset reference object (`{"guid", "path"}`,
//   collected by collectSceneAssetUsage) and legacy path strings in
//   component keys ending in `_asset`, both relative to assetRoot, plus a
//   prefab's nested prefab sources, relative to the prefab's folder;
// - a material (.zmat): its texture references, relative to its folder.
// Other kinds reference nothing. Returns an empty string on success, else why
// the text could not be read.
std::string collectFileAssetReferences(EditorAssetKind kind, std::string_view text, const std::filesystem::path& file,
                                       const std::filesystem::path& assetRoot, FileAssetReferences& output);

// Whether references name the asset at absolutePath or with guid (null
// matches by path only).
bool referencesAsset(const FileAssetReferences& references, const std::filesystem::path& absolutePath, AssetGuid guid);

// A project file that references an asset.
struct AssetReferrer {
    std::filesystem::path file;
    EditorAssetKind kind{};

    bool operator==(const AssetReferrer&) const = default;
};

// The scene, prefab, and material files of a catalog, read once each and
// read again only when the catalog reports a different size or write time,
// so repeated searches are cheap.
class AssetReferenceIndex {
  public:
    // Every scene, prefab, and material file of catalog that references item
    // (an absolute path: an asset, or a folder for everything inside it),
    // sorted by path. Files inside item are not listed, so a folder's own
    // contents do not keep it in use. A file that cannot be read is skipped
    // and named in warnings, once per change of the file.
    std::vector<AssetReferrer> findReferences(const AssetCatalog& catalog, const std::filesystem::path& item,
                                              std::vector<std::string>& warnings);

  private:
    struct CachedFile {
        uintmax_t size{0};
        std::filesystem::file_time_type writeTime{};
        FileAssetReferences references;
        bool readable{false};
    };

    const CachedFile& cached(const EditorAssetRecord& record, const std::filesystem::path& assetRoot,
                             std::vector<std::string>& warnings);

    std::map<std::filesystem::path, CachedFile> m_files;
};

// The entities of a serialized scene (EditorDocument capture) whose
// components reference the asset at absolutePath or with guid, in document
// order. Empty when the document cannot be read.
std::vector<SceneEntityId> sceneEntitiesReferencing(std::string_view document, const std::filesystem::path& assetRoot,
                                                    const std::filesystem::path& absolutePath, AssetGuid guid);
