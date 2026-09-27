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

struct EditorAssetRecord {
    std::filesystem::path relativePath;
    std::filesystem::path absolutePath;
    EditorAssetKind kind{};
    uintmax_t byteSize{0};
    std::filesystem::file_time_type lastWriteTime{};
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

class AssetCatalog {
  public:
    AssetCatalog(std::filesystem::path assetRoot, std::filesystem::path sceneRoot);

    AssetCatalogResult refresh();
    const std::vector<EditorAssetRecord>& assets() const;

  private:
    std::filesystem::path m_assetRoot;
    std::filesystem::path m_sceneRoot;
    std::vector<EditorAssetRecord> m_assets;
};
