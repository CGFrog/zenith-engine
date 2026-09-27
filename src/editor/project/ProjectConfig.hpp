#pragma once

#include <filesystem>
#include <string>

struct ProjectConfig {
    static constexpr int currentVersion = 1;

    std::filesystem::path projectFile;
    std::filesystem::path assetRoot;
    std::filesystem::path sceneRoot;
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
