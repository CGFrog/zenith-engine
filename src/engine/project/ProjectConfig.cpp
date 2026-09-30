#include "ProjectConfig.hpp"

#include "engine/graphics/RenderConfiguration.hpp"
#include "engine/scene/SceneDocument.hpp"

#include <fstream>
#include <nlohmann/json.hpp>
#include <system_error>

namespace {
using Json = nlohmann::json;

bool isContainedBy(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    const std::filesystem::path relative = candidate.lexically_relative(root);
    if (relative.empty() || relative.is_absolute()) {
        return false;
    }
    const auto first = relative.begin();
    return first == relative.end() || *first != "..";
}

ProjectConfigResult resolveContainedPath(const std::filesystem::path& root, const std::filesystem::path& relative,
                                         std::string_view field, std::filesystem::path& resolved) {
    if (relative.empty() || relative.is_absolute()) {
        return {.error = "Project field '" + std::string(field) + "' must be a non-empty relative path"};
    }

    const std::filesystem::path normalizedRoot = std::filesystem::absolute(root).lexically_normal();
    const std::filesystem::path candidate = (normalizedRoot / relative).lexically_normal();
    if (!isContainedBy(normalizedRoot, candidate)) {
        return {.error = "Project field '" + std::string(field) + "' escapes its root"};
    }
    resolved = candidate;
    return {.success = true};
}

ProjectConfigResult readRoot(const Json& document, std::string_view key, const std::filesystem::path& projectDirectory,
                             std::filesystem::path& root) {
    const auto found = document.find(key);
    if (found == document.end() || !found->is_string()) {
        return {.error = "Project field '" + std::string(key) + "' must be a path string"};
    }
    const std::filesystem::path relative = found->get<std::string>();
    if (relative.empty() || relative.is_absolute()) {
        return {.error = "Project field '" + std::string(key) + "' must be a non-empty relative path"};
    }
    root = (std::filesystem::absolute(projectDirectory) / relative).lexically_normal();
    return {.success = true};
}

ProjectConfigResult containedRelativeForSave(const std::filesystem::path& root, const std::filesystem::path& path,
                                             std::string_view field, std::filesystem::path& relative) {
    const std::filesystem::path normalizedRoot = std::filesystem::absolute(root).lexically_normal();
    const std::filesystem::path normalizedPath = std::filesystem::absolute(path).lexically_normal();
    if (!isContainedBy(normalizedRoot, normalizedPath)) {
        return {.error = "Project field '" + std::string(field) + "' must remain inside its root"};
    }
    relative = normalizedPath.lexically_relative(normalizedRoot);
    return {.success = !relative.empty()};
}

ProjectConfigResult rootRelativeForSave(const std::filesystem::path& projectDirectory,
                                        const std::filesystem::path& root, std::string_view field,
                                        std::filesystem::path& relative) {
    relative = std::filesystem::absolute(root).lexically_normal().lexically_relative(
        std::filesystem::absolute(projectDirectory).lexically_normal());
    if (relative.empty() || relative.is_absolute()) {
        return {.error = "Project field '" + std::string(field) + "' must be representable as a relative path"};
    }
    return {.success = true};
}
} // namespace

ProjectConfigResult loadProjectConfig(const std::filesystem::path& path, ProjectConfig& config) {
    std::ifstream input(path);
    if (!input) {
        return {.error = "Could not open project file: " + path.string()};
    }

    Json document;
    try {
        input >> document;
    } catch (const Json::exception& error) {
        return {.error = "Could not parse project file '" + path.string() + "': " + error.what()};
    }
    if (!document.is_object()) {
        return {.error = "Project file must contain an object: " + path.string()};
    }

    const int version = document.value("version", 0);
    if (version < 1 || version > ProjectConfig::currentVersion) {
        return {.error = "Unsupported project version in: " + path.string()};
    }

    ProjectConfig loaded;
    loaded.projectFile = std::filesystem::absolute(path).lexically_normal();
    const std::filesystem::path projectDirectory = loaded.projectFile.parent_path();
    if (ProjectConfigResult result = readRoot(document, "assetRoot", projectDirectory, loaded.assetRoot); !result) {
        return result;
    }
    if (ProjectConfigResult result = readRoot(document, "sceneRoot", projectDirectory, loaded.sceneRoot); !result) {
        return result;
    }

    if (version == 1) {
        loaded.displayName = loaded.projectFile.stem().string();
    } else {
        const auto displayName = document.find("displayName");
        if (displayName == document.end() || !displayName->is_string() || displayName->get<std::string>().empty()) {
            return {.error = "Project field 'displayName' must be a non-empty string"};
        }
        loaded.displayName = displayName->get<std::string>();

        const auto startupScene = document.find("startupScene");
        if (startupScene == document.end() || !startupScene->is_string()) {
            return {.error = "Project field 'startupScene' must be a path string"};
        }
        const std::filesystem::path reference = startupScene->get<std::string>();
        if (!reference.empty()) {
            if (ProjectConfigResult result =
                    resolveContainedPath(loaded.sceneRoot, reference, "startupScene", loaded.startupScene);
                !result) {
                return result;
            }
        }
    }
    if (version >= 3) {
        const auto renderSettings = document.find("renderSettings");
        if (renderSettings == document.end()) {
            return {.error = "Project field 'renderSettings' is required"};
        }
        if (RenderConfigurationResult result = deserializeRenderSettings(renderSettings->dump(), loaded.renderSettings);
            !result) {
            return {.error = "Invalid project render settings: " + result.error};
        }
    }

    config = std::move(loaded);
    return {.success = true};
}

ProjectConfigResult saveProjectConfig(const std::filesystem::path& path, const ProjectConfig& config) {
    if (config.displayName.empty()) {
        return {.error = "Project display name must not be empty"};
    }

    const std::filesystem::path projectPath = std::filesystem::absolute(path).lexically_normal();
    const std::filesystem::path projectDirectory = projectPath.parent_path();
    std::filesystem::path assetRoot;
    std::filesystem::path sceneRoot;
    if (ProjectConfigResult result = rootRelativeForSave(projectDirectory, config.assetRoot, "assetRoot", assetRoot);
        !result) {
        return result;
    }
    if (ProjectConfigResult result = rootRelativeForSave(projectDirectory, config.sceneRoot, "sceneRoot", sceneRoot);
        !result) {
        return result;
    }

    std::filesystem::path startupScene;
    if (!config.startupScene.empty()) {
        if (ProjectConfigResult result =
                containedRelativeForSave(config.sceneRoot, config.startupScene, "startupScene", startupScene);
            !result) {
            return result;
        }
    }

    std::error_code error;
    std::filesystem::create_directories(projectDirectory, error);
    if (error) {
        return {.error = "Could not create project directory: " + error.message()};
    }

    std::ofstream output(projectPath, std::ios::trunc);
    if (!output) {
        return {.error = "Could not write project file: " + projectPath.string()};
    }
    std::string serializedRenderSettings;
    if (RenderConfigurationResult result = serializeRenderSettings(config.renderSettings, serializedRenderSettings);
        !result) {
        return {.error = result.error};
    }
    const Json document = {
        {"version", ProjectConfig::currentVersion},      {"displayName", config.displayName},
        {"assetRoot", assetRoot.generic_string()},       {"sceneRoot", sceneRoot.generic_string()},
        {"startupScene", startupScene.generic_string()}, {"renderSettings", Json::parse(serializedRenderSettings)}};
    output << document.dump(2) << '\n';
    if (!output) {
        return {.error = "Could not finish writing project file: " + projectPath.string()};
    }
    return {.success = true};
}

ProjectConfigResult resolveProjectScene(const ProjectConfig& config, const std::filesystem::path& reference,
                                        std::filesystem::path& scenePath) {
    return resolveContainedPath(config.sceneRoot, reference, "scene", scenePath);
}

ProjectConfigResult loadProjectScene(const ProjectConfig& config, const std::filesystem::path& reference,
                                     AssetManager& assets, Scene& scene) {
    std::filesystem::path scenePath;
    if (ProjectConfigResult result = resolveProjectScene(config, reference, scenePath); !result) {
        return result;
    }
    const SceneDocumentResult result = SceneDocument::load(scenePath, assets, {.assetRoot = config.assetRoot}, scene);
    return {.success = result.success, .error = result.error};
}

ProjectConfigResult loadProjectStartupScene(const ProjectConfig& config, AssetManager& assets, Scene& scene) {
    if (config.startupScene.empty()) {
        return {.error = "Project '" + config.displayName + "' does not configure a startup scene"};
    }
    if (!isContainedBy(std::filesystem::absolute(config.sceneRoot).lexically_normal(),
                       std::filesystem::absolute(config.startupScene).lexically_normal())) {
        return {.error = "Project startup scene escapes the configured scene root"};
    }

    const std::filesystem::path reference = config.startupScene.lexically_relative(config.sceneRoot);
    return loadProjectScene(config, reference, assets, scene);
}
