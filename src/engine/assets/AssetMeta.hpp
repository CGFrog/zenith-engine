#pragma once

#include "engine/assets/AssetGuid.hpp"

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

// The `<asset file>.meta` JSON sidecar that gives an asset its stable identity
// and holds per-asset import settings, for example:
//
//   {
//       "version": 1,
//       "guid": "3f2a...e9",
//       "importSettings": {}
//   }
//
// Import settings are an importer-defined JSON object; the meta layer only
// guarantees that they round-trip.
struct AssetMeta {
    static constexpr int currentVersion = 1;

    int version{currentVersion};
    AssetGuid guid;
    nlohmann::json importSettings = nlohmann::json::object();
};

struct AssetMetaResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

inline constexpr std::string_view assetMetaExtension = ".meta";

// `textures/stone.png` -> `textures/stone.png.meta`.
std::filesystem::path assetMetaPath(const std::filesystem::path& assetPath);

// Rejects a non-object document, a missing or null GUID, an unsupported
// version, and non-object import settings. Missing settings read as `{}`.
AssetMetaResult parseAssetMeta(std::string_view text, AssetMeta& meta);

// Always writes the current version; reading then writing upgrades a sidecar.
std::string serializeAssetMeta(const AssetMeta& meta);

AssetMetaResult readAssetMeta(const std::filesystem::path& metaPath, AssetMeta& meta);

// Writes `<metaPath>.tmp` and replaces `metaPath` with it, so readers see either
// the previous sidecar or the complete new one.
AssetMetaResult writeAssetMeta(const std::filesystem::path& metaPath, const AssetMeta& meta);
