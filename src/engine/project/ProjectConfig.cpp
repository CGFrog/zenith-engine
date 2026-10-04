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

// An absent key leaves path empty; a present one must be a relative path.
ProjectConfigResult readOptionalPath(const Json& document, std::string_view key,
                                     const std::filesystem::path& projectDirectory, std::filesystem::path& path) {
    if (!document.contains(key)) {
        path.clear();
        return {.success = true};
    }
    return readRoot(document, key, projectDirectory, path);
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

ProjectConfigResult readPhysicsLayers(const Json& document, std::optional<PhysicsLayerTable>& layers) {
    const auto found = document.find("physicsLayers");
    if (found == document.end()) {
        layers.reset();
        return {.success = true};
    }
    if (!found->is_array()) {
        return {.error = "Project field 'physicsLayers' must be an array of layers"};
    }
    PhysicsLayerTable table;
    for (const Json& layer : *found) {
        const auto name = layer.is_object() ? layer.find("name") : layer.end();
        if (!layer.is_object() || name == layer.end() || !name->is_string() || !table.add(name->get<std::string>())) {
            return {.error = "Each project physics layer needs a unique, non-empty 'name', at most " +
                             std::to_string(maxPhysicsLayers) + " layers"};
        }
    }
    for (size_t index = 0; index < found->size(); ++index) {
        const Json& layer = (*found)[index];
        const auto collidesWith = layer.find("collidesWith");
        if (collidesWith == layer.end()) {
            continue;
        }
        if (!collidesWith->is_array()) {
            return {.error = "Physics layer '" + table.names[index] + "' needs 'collidesWith' to list layer names"};
        }
        for (const Json& other : *collidesWith) {
            const std::optional<PhysicsLayer> otherLayer =
                other.is_string() ? table.find(other.get<std::string>()) : std::nullopt;
            if (!otherLayer) {
                return {.error = "Physics layer '" + table.names[index] + "' collides with an unknown layer " +
                                 other.dump()};
            }
            table.setCollides({static_cast<uint32_t>(index)}, *otherLayer, true);
        }
    }
    if (const std::string error = table.validate(); !error.empty()) {
        return {.error = "Invalid project physics layers: " + error};
    }
    layers = std::move(table);
    return {.success = true};
}

Json writePhysicsLayers(const PhysicsLayerTable& table) {
    Json layers = Json::array();
    for (uint32_t index = 0; index < table.size(); ++index) {
        Json collidesWith = Json::array();
        for (uint32_t other = 0; other < table.size(); ++other) {
            if (table.collides({index}, {other})) {
                collidesWith.push_back(table.names[other]);
            }
        }
        layers.push_back({{"name", table.names[index]}, {"collidesWith", std::move(collidesWith)}});
    }
    return layers;
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

    if (version >= 4) {
        if (ProjectConfigResult result = readOptionalPath(document, "gameModule", projectDirectory, loaded.gameModule);
            !result) {
            return result;
        }
    }
    if (version >= 5) {
        if (ProjectConfigResult result = readPhysicsLayers(document, loaded.physicsLayers); !result) {
            return result;
        }
    }

    config = std::move(loaded);
    return {.success = true};
}

PhysicsLayerTable ProjectConfig::physicsLayerTable() const {
    return physicsLayers.value_or(PhysicsLayerTable::defaults());
}

ProjectConfigResult saveProjectConfig(const std::filesystem::path& path, const ProjectConfig& config) {
    if (config.displayName.empty()) {
        return {.error = "Project display name must not be empty"};
    }
    if (config.physicsLayers) {
        if (const std::string error = config.physicsLayers->validate(); !error.empty()) {
            return {.error = "Invalid project physics layers: " + error};
        }
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

    std::filesystem::path gameModule;
    if (!config.gameModule.empty()) {
        if (ProjectConfigResult result =
                rootRelativeForSave(projectDirectory, config.gameModule, "gameModule", gameModule);
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
    Json document = {
        {"version", ProjectConfig::currentVersion},      {"displayName", config.displayName},
        {"assetRoot", assetRoot.generic_string()},       {"sceneRoot", sceneRoot.generic_string()},
        {"startupScene", startupScene.generic_string()}, {"renderSettings", Json::parse(serializedRenderSettings)}};
    if (!gameModule.empty()) {
        document["gameModule"] = gameModule.generic_string();
    }
    if (config.physicsLayers) {
        document["physicsLayers"] = writePhysicsLayers(*config.physicsLayers);
    }
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
                                     AssetManager& assets, Scene& scene,
                                     std::shared_ptr<const ComponentTypeRegistry> componentTypes) {
    std::filesystem::path scenePath;
    if (ProjectConfigResult result = resolveProjectScene(config, reference, scenePath); !result) {
        return result;
    }
    return loadProjectSceneFile(config, scenePath, assets, scene, std::move(componentTypes));
}

ProjectConfigResult loadProjectSceneFile(const ProjectConfig& config, const std::filesystem::path& sceneFile,
                                         AssetManager& assets, Scene& scene,
                                         std::shared_ptr<const ComponentTypeRegistry> componentTypes) {
    const SceneDocumentOptions options{.assetRoot = config.assetRoot, .componentTypes = std::move(componentTypes)};
    const SceneDocumentResult result = SceneDocument::load(sceneFile, assets, options, scene);
    return {.success = result.success, .error = result.error};
}

ProjectConfigResult loadProjectStartupScene(const ProjectConfig& config, AssetManager& assets, Scene& scene,
                                            std::shared_ptr<const ComponentTypeRegistry> componentTypes) {
    if (config.startupScene.empty()) {
        return {.error = "Project '" + config.displayName + "' does not configure a startup scene"};
    }
    if (!isContainedBy(std::filesystem::absolute(config.sceneRoot).lexically_normal(),
                       std::filesystem::absolute(config.startupScene).lexically_normal())) {
        return {.error = "Project startup scene escapes the configured scene root"};
    }

    const std::filesystem::path reference = config.startupScene.lexically_relative(config.sceneRoot);
    return loadProjectScene(config, reference, assets, scene, std::move(componentTypes));
}
