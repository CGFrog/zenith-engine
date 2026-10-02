#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

enum class EditorAssetKind {
    Model,
    Texture,
    Audio,
    Scene,
};

// Paths in records and folders are absolute and lexically normal, so they can
// be compared directly. relativePath is relative to the root that holds it.
struct EditorAssetRecord {
    std::filesystem::path relativePath;
    std::filesystem::path absolutePath;
    EditorAssetKind kind{};
    uintmax_t byteSize{0};
    std::filesystem::file_time_type lastWriteTime{};
};

// A directory under one of the catalog roots, including empty directories and
// the roots themselves (whose relativePath is empty).
struct EditorAssetFolder {
    std::filesystem::path relativePath;
    std::filesystem::path absolutePath;
};

enum class AssetChangeKind {
    Added,
    Modified,
    Removed,
};

struct AssetCatalogChange {
    AssetChangeKind kind{};
    EditorAssetRecord asset;
};

struct AssetCatalogResult {
    bool success{false};
    std::string error;
    std::vector<AssetCatalogChange> changes;

    explicit operator bool() const {
        return success;
    }
};

// The project's authored files: supported assets and every folder under the
// asset and scene roots. refresh() rescans both roots and reports asset
// changes; folders are listed but not diffed.
class AssetCatalog {
  public:
    // Relative roots resolve against the current directory.
    AssetCatalog(std::filesystem::path assetRoot, std::filesystem::path sceneRoot);

    AssetCatalogResult refresh();
    // Sorted by absolute path.
    const std::vector<EditorAssetRecord>& assets() const;
    // Every existing folder, sorted by absolute path; roots included.
    const std::vector<EditorAssetFolder>& folders() const;
    // The asset root first, then the scene root when it differs.
    const std::vector<std::filesystem::path>& roots() const;

    const EditorAssetFolder* findFolder(const std::filesystem::path& absolutePath) const;
    const EditorAssetRecord* findAsset(const std::filesystem::path& absolutePath) const;
    // Direct children of a folder, in path order.
    std::vector<const EditorAssetFolder*> subfolders(const std::filesystem::path& folder) const;
    std::vector<const EditorAssetRecord*> assetsInFolder(const std::filesystem::path& folder) const;

  private:
    std::vector<std::filesystem::path> m_roots;
    std::vector<EditorAssetRecord> m_assets;
    std::vector<EditorAssetFolder> m_folders;
};

// Absolute, lexically normal, and without a trailing separator, matching the
// paths the catalog reports. Falls back to a lexical result if the current
// directory cannot be read.
std::filesystem::path normalizedCatalogPath(const std::filesystem::path& path);
