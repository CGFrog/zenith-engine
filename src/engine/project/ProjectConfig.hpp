#pragma once

// ProjectConfig: the .zenith-project manifest (folders, startup scene, module, render, layers, tags, navigation).

#include "engine/graphics/Rendering.hpp"
#include "engine/navigation/NavMeshBuildSettings.hpp"
#include "engine/physics/PhysicsLayers.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class AssetManager;
class ComponentTypeRegistry;
class InputActionMap;
class Scene;

struct ProjectConfig {
    static constexpr int currentVersion = 7;

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
    // The project's collision layers and matrix (version 5); empty uses
    // PhysicsLayerTable::defaults(). Written as "physicsLayers": an array of
    // {"name", "collidesWith": [names]} objects whose order gives each
    // layer's index; a pair collides when either lists the other.
    std::optional<PhysicsLayerTable> physicsLayers;
    // The project's default input actions (version 6): a JSON file
    // (InputActionsJson.hpp) stored relative to the project file and resolved
    // to an absolute path on load. Its actions replace the game module's
    // actions of the same name (loadProjectInputActions). Empty when the
    // project has none.
    std::filesystem::path inputActions;
    // The tag names the project declares for TagsComponent (version 6), in
    // the order the editor lists them; unique and non-empty.
    std::vector<std::string> tags;
    // How the project's navigation meshes are built (version 7): the editor's
    // navigation.bake and the game's sessions (GameServices::
    // setDefaultNavigationSettings). Written as "navigation": an object of
    // the NavMeshBuildSettings fields by their C++ names ("cellSize",
    // "agentRadius", ..., "layerMask" as a number); a missing field keeps its
    // default. Empty uses the defaults.
    std::optional<NavMeshBuildSettings> navigation;

    // physicsLayers, or the defaults.
    PhysicsLayerTable physicsLayerTable() const;
    // navigation, or the defaults.
    NavMeshBuildSettings navigationSettings() const;
};

struct ProjectConfigResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

ProjectConfigResult loadProjectConfig(const std::filesystem::path& path, ProjectConfig& config);
// Empty when tags can be saved: unique, non-empty, and on one line each.
std::string validateProjectTags(const std::vector<std::string>& tags);
// Reads the project's input action file, when it names one, and lets its
// actions replace those of actions with the same name (a game's defaults,
// from its module). A project without the file leaves actions alone.
ProjectConfigResult loadProjectInputActions(const ProjectConfig& config, InputActionMap& actions);
ProjectConfigResult saveProjectConfig(const std::filesystem::path& path, const ProjectConfig& config);
ProjectConfigResult resolveProjectScene(const ProjectConfig& config, const std::filesystem::path& reference,
                                        std::filesystem::path& scenePath);
// Loads a project scene with componentTypes, or with the engine's built-in
// component types when it is null. A game passes the registry its game module
// extended so scenes holding the game's components load.
ProjectConfigResult loadProjectScene(const ProjectConfig& config, const std::filesystem::path& reference,
                                     AssetManager& assets, Scene& scene,
                                     std::shared_ptr<const ComponentTypeRegistry> componentTypes = nullptr);
// Loads the scene document at sceneFile (any path, inside the scene root or
// not, such as a copy an editor saved for a test run) with the project's asset
// root and componentTypes, as loadProjectScene does.
ProjectConfigResult loadProjectSceneFile(const ProjectConfig& config, const std::filesystem::path& sceneFile,
                                         AssetManager& assets, Scene& scene,
                                         std::shared_ptr<const ComponentTypeRegistry> componentTypes = nullptr);
ProjectConfigResult loadProjectStartupScene(const ProjectConfig& config, AssetManager& assets, Scene& scene,
                                            std::shared_ptr<const ComponentTypeRegistry> componentTypes = nullptr);
