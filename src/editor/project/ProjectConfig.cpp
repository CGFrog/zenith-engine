#include "ProjectConfig.hpp"

#include <fstream>
#include <nlohmann/json.hpp>

namespace {
ProjectConfigResult readRelativeRoot(const nlohmann::json& document, std::string_view key,
                                     const std::filesystem::path& projectDirectory, std::filesystem::path& root) {
    const auto found = document.find(key);
    if (found == document.end() || !found->is_string()) {
        return {.error = "Project field '" + std::string(key) + "' must be a path string"};
    }
    const std::filesystem::path relative = found->get<std::string>();
    if (relative.empty() || relative.is_absolute()) {
        return {.error = "Project field '" + std::string(key) + "' must be a non-empty project-relative path"};
    }
    root = (projectDirectory / relative).lexically_normal();
    return {.success = true};
}
} // namespace

ProjectConfigResult loadProjectConfig(const std::filesystem::path& path, ProjectConfig& config) {
    std::ifstream input(path);
    if (!input) {
        return {.error = "Could not open project file: " + path.string()};
    }

    nlohmann::json document;
    try {
        input >> document;
    } catch (const nlohmann::json::exception& error) {
        return {.error = "Could not parse project file '" + path.string() + "': " + error.what()};
    }
    if (!document.is_object() || document.value("version", 0) != ProjectConfig::currentVersion) {
        return {.error = "Unsupported project version in: " + path.string()};
    }

    ProjectConfig loaded;
    loaded.projectFile = path.lexically_normal();
    const std::filesystem::path directory = loaded.projectFile.parent_path();
    if (ProjectConfigResult result = readRelativeRoot(document, "assetRoot", directory, loaded.assetRoot); !result) {
        return result;
    }
    if (ProjectConfigResult result = readRelativeRoot(document, "sceneRoot", directory, loaded.sceneRoot); !result) {
        return result;
    }
    config = std::move(loaded);
    return {.success = true};
}

ProjectConfigResult saveProjectConfig(const std::filesystem::path& path, const ProjectConfig& config) {
    const std::filesystem::path directory = path.parent_path();
    const std::filesystem::path assetRoot = config.assetRoot.lexically_relative(directory);
    const std::filesystem::path sceneRoot = config.sceneRoot.lexically_relative(directory);
    if (assetRoot.empty() || sceneRoot.empty() || assetRoot.is_absolute() || sceneRoot.is_absolute()) {
        return {.error = "Project roots must be representable relative to the project file"};
    }
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        return {.error = "Could not create project directory: " + error.message()};
    }
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        return {.error = "Could not write project file: " + path.string()};
    }
    const nlohmann::json document = {{"version", ProjectConfig::currentVersion},
                                     {"assetRoot", assetRoot.generic_string()},
                                     {"sceneRoot", sceneRoot.generic_string()}};
    output << document.dump(2) << '\n';
    if (!output) {
        return {.error = "Could not finish writing project file: " + path.string()};
    }
    return {.success = true};
}
