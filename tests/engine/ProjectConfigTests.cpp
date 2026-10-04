#include "engine/input/InputActionsJson.hpp"
#include "engine/project/ProjectConfig.hpp"
#include "engine/resources/ResourceLocator.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/Scene.hpp"
#include "engine/scene/SceneDocument.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>

namespace {
int failures = 0;

struct ProjectMarkerComponent {
    int32_t value{0};
};

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

void writeFile(const std::filesystem::path& path, std::string_view contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
} // namespace

int main() {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "zenith_project_config_tests";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory / "content", error);
    std::filesystem::create_directories(directory / "scenes" / "chapter", error);

    const std::filesystem::path projectPath = directory / "sample.zenith-project";
    ProjectConfig source{.projectFile = projectPath,
                         .displayName = "Sample project",
                         .assetRoot = directory / "content",
                         .sceneRoot = directory / "scenes",
                         .startupScene = directory / "scenes" / "room.zenith-scene"};
    source.renderSettings.environment = EnvironmentQuality::ImageBasedHigh;
    source.renderSettings.localLights = LocalLightingQuality::Disabled;
    source.renderSettings.maximumLocalLights = 2048;
    source.renderSettings.budgets.shadow = {1.0, 0.5};
    expect(saveProjectConfig(projectPath, source).success, "project configuration should save");

    ProjectConfig loaded;
    expect(loadProjectConfig(projectPath, loaded).success, "project configuration should load");
    expect(loaded.displayName == source.displayName && loaded.assetRoot == source.assetRoot &&
               loaded.sceneRoot == source.sceneRoot && loaded.startupScene == source.startupScene &&
               loaded.renderSettings.environment == source.renderSettings.environment &&
               loaded.renderSettings.localLights == source.renderSettings.localLights &&
               loaded.renderSettings.maximumLocalLights == 2048 &&
               loaded.renderSettings.budgets.shadow.gpuMilliseconds == 0.5,
           "project fields should round-trip as paths relative to the project file");

    std::filesystem::path resolvedScene;
    expect(resolveProjectScene(loaded, "chapter/room.zenith-scene", resolvedScene).success &&
               resolvedScene == directory / "scenes" / "chapter" / "room.zenith-scene",
           "scene references resolve beneath the configured scene root");
    expect(!resolveProjectScene(loaded, "../outside.zenith-scene", resolvedScene),
           "scene references cannot escape the configured scene root");

    AssetManager assets;
    Scene authoredScene;
    authoredScene.createEntity("Authored startup entity");
    expect(SceneDocument::save(source.startupScene, authoredScene, assets, {.assetRoot = source.assetRoot}).success,
           "the authored startup fixture should save");
    Scene runtimeScene;
    expect(loadProjectStartupScene(loaded, assets, runtimeScene).success,
           "the configured startup scene should load through the shared project contract");
    expect(runtimeScene.registry().view<NameComponent>().size() == 1,
           "startup scene loading should replace the runtime scene with authored content");

    authoredScene.createEntity("Second authored entity");
    const std::filesystem::path chapterScene = directory / "scenes" / "chapter" / "room.zenith-scene";
    expect(SceneDocument::save(chapterScene, authoredScene, assets, {.assetRoot = source.assetRoot}).success,
           "the transition scene fixture should save");
    expect(loadProjectScene(loaded, "chapter/room.zenith-scene", assets, runtimeScene).success &&
               runtimeScene.registry().view<NameComponent>().size() == 2,
           "project-relative scene references load authored transition destinations");

    // A game's component types load only through the registry the game passes.
    auto gameTypes = std::make_shared<ComponentTypeRegistry>(*builtInComponentTypes());
    expect(gameTypes
               ->add(ComponentTypeBuilder<ProjectMarkerComponent>("project_marker", "Project Marker")
                         .fields(Fields<ProjectMarkerComponent>().add("value", &ProjectMarkerComponent::value))
                         .build())
               .success,
           "the game component type registers");
    Scene markedScene;
    const Entity marked = markedScene.createEntity("Marked");
    markedScene.registry().emplace<ProjectMarkerComponent>(marked, ProjectMarkerComponent{.value = 42});
    const std::filesystem::path markedPath = directory / "scenes" / "marked.zenith-scene";
    expect(SceneDocument::save(markedPath, markedScene, assets,
                               {.assetRoot = source.assetRoot, .componentTypes = gameTypes})
               .success,
           "the game component fixture should save");
    Scene builtInOnly;
    expect(loadProjectScene(loaded, "marked.zenith-scene", assets, builtInOnly).success &&
               builtInOnly.registry().view<ProjectMarkerComponent>().empty(),
           "without a registry project scenes load with the built-in component types only");
    Scene withGameTypes;
    const bool loadedWithGameTypes =
        loadProjectScene(loaded, "marked.zenith-scene", assets, withGameTypes, gameTypes).success;
    const auto markers = withGameTypes.registry().view<ProjectMarkerComponent>();
    expect(loadedWithGameTypes && markers.size() == 1 &&
               markers.get<ProjectMarkerComponent>(*markers.begin()).value == 42,
           "project scenes load game components through the registry the game passes");
    ProjectConfig markedStartup = loaded;
    markedStartup.startupScene = directory / "scenes" / "marked.zenith-scene";
    Scene startupWithGameTypes;
    expect(loadProjectStartupScene(markedStartup, assets, startupWithGameTypes, gameTypes).success &&
               startupWithGameTypes.registry().view<ProjectMarkerComponent>().size() == 1,
           "startup scenes load game components through the registry the game passes");

    const std::filesystem::path legacyPath = directory / "legacy.zenith-project";
    writeFile(legacyPath, R"({"version":1,"assetRoot":"content","sceneRoot":"scenes"})");
    ProjectConfig legacy;
    expect(loadProjectConfig(legacyPath, legacy).success && legacy.displayName == "legacy" &&
               legacy.startupScene.empty(),
           "version 1 projects load with a derived display name and no startup scene");
    Scene unchangedScene;
    const Entity unchangedEntity = unchangedScene.createEntity("Keep me");
    expect(!loadProjectStartupScene(legacy, assets, unchangedScene),
           "a legacy project without a startup scene fails with a recoverable error");
    expect(unchangedScene.registry().valid(unchangedEntity),
           "a missing startup scene does not mutate the destination scene");

    const std::filesystem::path versionTwoPath = directory / "version-two.zenith-project";
    writeFile(
        versionTwoPath,
        R"({"version":2,"displayName":"Version two","assetRoot":"content","sceneRoot":"scenes","startupScene":"room.zenith-scene"})");
    ProjectConfig versionTwo;
    expect(loadProjectConfig(versionTwoPath, versionTwo).success &&
               versionTwo.renderSettings.environment == EnvironmentQuality::Hemisphere,
           "version 2 projects remain readable with explicit default render settings");

    const std::filesystem::path escapingPath = directory / "escaping.zenith-project";
    writeFile(
        escapingPath,
        R"({"version":2,"displayName":"Escape","assetRoot":"content","sceneRoot":"scenes","startupScene":"../outside.zenith-scene"})");
    ProjectConfig escaping = loaded;
    expect(!loadProjectConfig(escapingPath, escaping), "project startup scenes cannot escape the scene root");
    expect(escaping.displayName == loaded.displayName,
           "failed containment validation preserves the active project configuration");

    expect(loaded.gameModule.empty(), "a project saved without a game module loads without one");
    expect(readFile(projectPath).find("gameModule") == std::string::npos,
           "a project without a game module does not write the optional key");

    const std::filesystem::path moduleProjectPath = directory / "module.zenith-project";
    ProjectConfig withModule = source;
    withModule.gameModule = directory / "bin" / "game_module.dll";
    expect(saveProjectConfig(moduleProjectPath, withModule).success, "a project with a game module should save");
    const std::string moduleProjectText = readFile(moduleProjectPath);
    expect(moduleProjectText.find(R"("gameModule": "bin/game_module.dll")") != std::string::npos &&
               moduleProjectText.find(R"("version": 6)") != std::string::npos,
           "the game module is written relative to the project file");
    ProjectConfig loadedWithModule;
    expect(loadProjectConfig(moduleProjectPath, loadedWithModule).success &&
               loadedWithModule.gameModule == directory / "bin" / "game_module.dll",
           "the game module resolves against the project directory");
    ProjectConfig resavedWithModule;
    expect(saveProjectConfig(moduleProjectPath, loadedWithModule).success &&
               loadProjectConfig(moduleProjectPath, resavedWithModule).success &&
               resavedWithModule.gameModule == loadedWithModule.gameModule,
           "the game module is preserved when a loaded project is saved again");

    // Variants of the saved version 4 document with one field changed.
    const auto writeVariant = [&](const std::filesystem::path& path, int version, nlohmann::json gameModule) {
        nlohmann::json document = nlohmann::json::parse(moduleProjectText);
        document["version"] = version;
        document["gameModule"] = std::move(gameModule);
        writeFile(path, document.dump());
    };
    const std::filesystem::path absoluteModulePath = directory / "absolute-module.zenith-project";
    writeVariant(absoluteModulePath, 4, (directory / "game.dll").generic_string());
    ProjectConfig absoluteModule;
    expect(!loadProjectConfig(absoluteModulePath, absoluteModule), "an absolute game module path is rejected");
    const std::filesystem::path numericModulePath = directory / "numeric-module.zenith-project";
    writeVariant(numericModulePath, 4, 7);
    ProjectConfig numericModule;
    expect(!loadProjectConfig(numericModulePath, numericModule), "a game module that is not a path is rejected");
    const std::filesystem::path versionThreePath = directory / "version-three.zenith-project";
    writeVariant(versionThreePath, 3, "ignored.dll");
    ProjectConfig versionThree;
    expect(loadProjectConfig(versionThreePath, versionThree).success && versionThree.gameModule.empty(),
           "version 3 projects load without a game module");

    // Physics layers (version 5): absent means the defaults; a saved table
    // round-trips with its collision matrix.
    expect(!loadedWithModule.physicsLayers && loadedWithModule.physicsLayerTable() == PhysicsLayerTable::defaults() &&
               moduleProjectText.find("physicsLayers") == std::string::npos,
           "a project without physics layers uses and writes none but the defaults");
    ProjectConfig withLayers = source;
    PhysicsLayerTable layers = PhysicsLayerTable::defaults();
    const std::optional<PhysicsLayer> pickups = layers.add("Pickups");
    layers.setCollides(*pickups, PhysicsLayer::Player, true);
    withLayers.physicsLayers = layers;
    const std::filesystem::path layersPath = directory / "layers.zenith-project";
    ProjectConfig loadedLayers;
    expect(saveProjectConfig(layersPath, withLayers).success && loadProjectConfig(layersPath, loadedLayers).success &&
               loadedLayers.physicsLayers == layers,
           "project physics layers and their collision matrix round-trip");
    const auto writeLayers = [&](const std::filesystem::path& path, int version, nlohmann::json value) {
        nlohmann::json document = nlohmann::json::parse(readFile(layersPath));
        document["version"] = version;
        document["physicsLayers"] = std::move(value);
        writeFile(path, document.dump());
    };
    const std::filesystem::path oneSidedPath = directory / "one-sided.zenith-project";
    writeLayers(oneSidedPath, 5,
                nlohmann::json::array({{{"name", "Ground"}}, {{"name", "Bodies"}, {"collidesWith", {"Ground"}}}}));
    ProjectConfig oneSided;
    expect(loadProjectConfig(oneSidedPath, oneSided).success && oneSided.physicsLayers &&
               oneSided.physicsLayers->collides({0}, {1}) && !oneSided.physicsLayers->collides({0}, {0}),
           "a pair listed by one side collides both ways; unlisted pairs do not");
    const std::filesystem::path unknownLayerPath = directory / "unknown-layer.zenith-project";
    writeLayers(unknownLayerPath, 5, nlohmann::json::array({{{"name", "Ground"}, {"collidesWith", {"Water"}}}}));
    ProjectConfig unknownLayer;
    expect(!loadProjectConfig(unknownLayerPath, unknownLayer), "a collision with an unknown layer is rejected");
    const std::filesystem::path duplicatePath = directory / "duplicate-layer.zenith-project";
    writeLayers(duplicatePath, 5, nlohmann::json::array({{{"name", "Ground"}}, {{"name", "Ground"}}}));
    ProjectConfig duplicate;
    expect(!loadProjectConfig(duplicatePath, duplicate), "a layer name used twice is rejected");
    const std::filesystem::path versionFourPath = directory / "version-four.zenith-project";
    writeLayers(versionFourPath, 4, "ignored");
    ProjectConfig versionFour;
    expect(loadProjectConfig(versionFourPath, versionFour).success && !versionFour.physicsLayers,
           "version 4 projects load with the default physics layers");

    // Version 6: the input action file and the tag list.
    ProjectConfig withInput = source;
    withInput.inputActions = directory / "config" / "input_actions.json";
    withInput.tags = {"enemy", "pickup"};
    const std::filesystem::path inputPath = directory / "input.zenith-project";
    ProjectConfig loadedInput;
    expect(saveProjectConfig(inputPath, withInput).success && loadProjectConfig(inputPath, loadedInput).success &&
               loadedInput.inputActions == withInput.inputActions && loadedInput.tags == withInput.tags &&
               readFile(inputPath).find(R"("inputActions": "config/input_actions.json")") != std::string::npos,
           "the input action file (relative to the project) and tags round-trip");
    ProjectConfig badTags = source;
    badTags.tags = {"enemy", "enemy"};
    expect(!saveProjectConfig(directory / "bad-tags.zenith-project", badTags), "a tag listed twice is not saved");
    const std::filesystem::path versionFivePath = directory / "version-five.zenith-project";
    {
        nlohmann::json document = nlohmann::json::parse(readFile(inputPath));
        document["version"] = 5;
        writeFile(versionFivePath, document.dump());
    }
    ProjectConfig versionFive;
    expect(loadProjectConfig(versionFivePath, versionFive).success && versionFive.inputActions.empty() &&
               versionFive.tags.empty(),
           "version 5 projects load without input actions or tags");

    // The project's actions override the module's of the same name.
    InputActionMap moduleActions;
    moduleActions.addAction("Jump", InputActionValueType::Button);
    moduleActions.addBinding("Jump", {.source = Key::Space});
    moduleActions.addAction("Fire", InputActionValueType::Button);
    InputActionMap projectActions;
    projectActions.addAction("Jump", InputActionValueType::Button);
    projectActions.addBinding("Jump", {.source = Key::E});
    projectActions.addAction("Crouch", InputActionValueType::Button);
    expect(saveInputActionMap(withInput.inputActions, projectActions).success, "the project's actions save");
    InputActionMap merged = moduleActions;
    expect(loadProjectInputActions(loadedInput, merged).success && merged.actionNames().size() == 3 &&
               merged.bindings("Jump").size() == 1 && std::get<Key>(merged.bindings("Jump")[0].source) == Key::E &&
               merged.contains("Fire"),
           "the project's actions replace the module's of the same name and add their own");
    InputActionMap untouched = moduleActions;
    expect(loadProjectInputActions(source, untouched).success && untouched.bindings("Jump").size() == 1,
           "a project without an input action file leaves the module's actions alone");
    writeFile(withInput.inputActions, "{\"version\": 1, \"actions\": [{\"name\": \"Jump\", \"type\": \"leap\"}]}");
    InputActionMap afterBadFile = moduleActions;
    const ProjectConfigResult badFile = loadProjectInputActions(loadedInput, afterBadFile);
    expect(!badFile && badFile.error.find("Jump") != std::string::npos &&
               std::get<Key>(afterBadFile.bindings("Jump")[0].source) == Key::Space,
           "a bad input action file is reported and changes nothing");

    writeFile(projectPath, "{not json");
    ProjectConfig preserved = loaded;
    expect(!loadProjectConfig(projectPath, preserved), "malformed project configuration should fail");
    expect(preserved.assetRoot == loaded.assetRoot, "failed project load should preserve the active configuration");

    const std::filesystem::path packagedRoot = directory / "package with spaces";
    std::filesystem::create_directories(packagedRoot / "assets", error);
    std::filesystem::create_directories(packagedRoot / "shaders", error);
    initializeResourceLocator(packagedRoot / "bin" / "zenith");
    expect(resourceRoot() == std::filesystem::weakly_canonical(packagedRoot),
           "resource discovery finds the parent of a multi-config executable directory");
    initializeResourceLocator(packagedRoot / "zenith");
    expect(assetRoot() == std::filesystem::weakly_canonical(packagedRoot) / "assets" &&
               shaderRoot() == std::filesystem::weakly_canonical(packagedRoot) / "shaders",
           "packaged resources resolve beside the executable without using the working directory");

    std::filesystem::remove_all(directory, error);
    return failures == 0 ? 0 : 1;
}
