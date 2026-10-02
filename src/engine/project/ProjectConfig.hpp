#pragma once

#include "engine/graphics/Rendering.hpp"

#include <filesystem>
#include <string>

class AssetManager;
class Scene;

struct ProjectConfig {
    static constexpr int currentVersion = 4;

    std::filesystem::path projectFile;
    std::string displayName;
    std::filesystem::path assetRoot;
    std::filesystem::path sceneRoot;
    std::filesystem::path startupScene;
    RenderSettings renderSettings;
    // The game module library (version 4), stored relative to the project
    // file and resolved to an absolute path on load. Empty when the project
    // has no game module.
    std::filesystem::path gameModule;
};

struct ProjectConfigResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

ProjectConfigResult loadProjectConfig(const std::filesystem::path& path, ProjectConfig& config);
ProjectConfigResult saveProjectConfig(const std::filesystem::path& path, const ProjectConfig& config);
ProjectConfigResult resolveProjectScene(const ProjectConfig& config, const std::filesystem::path& reference,
                                        std::filesystem::path& scenePath);
ProjectConfigResult loadProjectScene(const ProjectConfig& config, const std::filesystem::path& reference,
                                     AssetManager& assets, Scene& scene);
ProjectConfigResult loadProjectStartupScene(const ProjectConfig& config, AssetManager& assets, Scene& scene);
