#pragma once

#include "Scene.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

class AssetManager;

struct SceneDocumentOptions {
    std::filesystem::path assetRoot;
};

struct SceneDocumentResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

class SceneDocument {
  public:
    static constexpr uint32_t currentVersion = 1;

    static SceneDocumentResult serialize(const Scene& scene, const AssetManager& assets,
                                         const SceneDocumentOptions& options, std::string& output);
    static SceneDocumentResult deserialize(std::string_view input, AssetManager& assets,
                                           const SceneDocumentOptions& options, Scene& destination);
    static SceneDocumentResult save(const std::filesystem::path& path, const Scene& scene, const AssetManager& assets,
                                    const SceneDocumentOptions& options);
    static SceneDocumentResult load(const std::filesystem::path& path, AssetManager& assets,
                                    const SceneDocumentOptions& options, Scene& destination);
};
