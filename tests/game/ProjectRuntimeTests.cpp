// The zenith executable's project-agnostic runtime: its command line, finding
// a packaged project, choosing and loading the gameplay (a project's game
// module or the linked demo gameplay), ProjectApplication playing a module's
// systems, and package verification of every project scene.

#include "engine/application/Application.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/module/GameModule.hpp"
#include "engine/scene/Scene.hpp"
#include "engine/scene/SceneDocument.hpp"
#include "game/application/GameLaunch.hpp"
#include "game/application/PackageVerification.hpp"
#include "game/application/ProjectApplication.hpp"
#include "game/application/RuntimeGameplay.hpp"
#include "game/gameplay/DemoGameplay.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

void writeText(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::trunc);
    output << text;
}

GameLaunchResult parse(std::initializer_list<std::string_view> arguments, GameLaunchOptions& options) {
    const std::vector<std::string_view> list(arguments);
    return parseGameLaunchArguments(list, options);
}

void testArguments() {
    GameLaunchOptions options;
    expect(parse({}, options).success && !options.verifyPackage && options.projectFile.empty() &&
               options.sceneFile.empty(),
           "no arguments play the built-in demo or the packaged project");

    const fs::path scene = fs::absolute("level one.zenith-scene");
    expect(parse({"--scene", scene.string(), "--verify-package", "--project", "game.zenith-project"}, options).success,
           "options parse in any order");
    expect(options.verifyPackage && options.projectFile == "game.zenith-project" && options.sceneFile == scene,
           "every option keeps its value");

    expect(parse({"--scene", "relative.zenith-scene"}, options).success && options.sceneFile.is_absolute() &&
               options.sceneFile.filename() == "relative.zenith-scene",
           "a relative --scene is resolved against the working directory");

    GameLaunchOptions unchanged;
    const GameLaunchResult unknown = parse({"--level", "x"}, unchanged);
    expect(!unknown && unknown.error.find("--level") != std::string::npos, "unknown arguments are named");
    const GameLaunchResult missing = parse({"--project"}, unchanged);
    expect(!missing && missing.error.find("--project needs a file") != std::string::npos,
           "an option without its file is refused");
    const GameLaunchResult repeated = parse({"--scene", "a", "--scene", "b"}, unchanged);
    expect(!repeated && repeated.error.find("more than once") != std::string::npos, "an option given twice is refused");
    expect(unchanged.projectFile.empty() && unchanged.sceneFile.empty(), "a failed parse leaves the options alone");
}

std::string projectJson() {
    return "{\n  \"version\": 4,\n  \"displayName\": \"Runtime Test\",\n  \"assetRoot\": \"assets\",\n"
           "  \"sceneRoot\": \"scenes\",\n  \"startupScene\": \"start.zenith-scene\",\n"
           "  \"renderSettings\": {\"version\": 1, \"antialiasing\": 1, \"shadows\": 1, \"lighting\": 1, "
           "\"bloom\": 0, \"environment\": 0, \"localLights\": 1}\n}\n";
}

void testPackagedProjectDiscovery(const fs::path& root) {
    const fs::path tree = root / "build tree";
    writeText(tree / "editor-smoke.zenith-project", projectJson());
    fs::path found = "stale";
    expect(findPackagedProject(tree, found).success && found.empty(),
           "a directory that is not a package never supplies a project");

    const fs::path package = root / "package";
    writeText(package / "content-manifest.json", "{}");
    writeText(package / "game.zenith-project", projectJson());
    expect(findPackagedProject(package, found).success && found == package / "game.zenith-project",
           "a package's root project is found");

    GameLaunchOptions launch;
    std::optional<ProjectConfig> project;
    expect(loadLaunchProject(launch, package, project).success && project &&
               project->startupScene == package / "scenes" / "start.zenith-scene",
           "without --project the packaged project plays its startup scene");

    const fs::path other = root / "elsewhere" / "test run.zenith-scene";
    launch.sceneFile = other;
    const GameLaunchResult missingScene = loadLaunchProject(launch, package, project);
    expect(!missingScene && missingScene.error.find("does not exist") != std::string::npos,
           "a --scene file that does not exist is refused");
    writeText(other, "{}");
    expect(loadLaunchProject(launch, package, project).success && project && project->startupScene == other &&
               project->assetRoot == package / "assets" && project->sceneRoot == other.parent_path(),
           "--scene outside the scene root replaces the startup scene, keeps the asset root, and brings its folder "
           "as the scene root");
    const fs::path inside = package / "scenes" / "levels" / "two.zenith-scene";
    writeText(inside, "{}");
    launch.sceneFile = inside;
    expect(loadLaunchProject(launch, package, project).success && project && project->startupScene == inside &&
               project->sceneRoot == package / "scenes",
           "--scene inside the scene root keeps the project's scene root");

    launch.sceneFile = other;
    std::optional<ProjectConfig> sceneOnly;
    expect(loadLaunchProject(launch, tree, sceneOnly).success && sceneOnly && sceneOnly->startupScene == other &&
               sceneOnly->assetRoot == tree / "assets" && sceneOnly->gameModule.empty(),
           "--scene without a project plays with the runtime's own assets");
    std::optional<ProjectConfig> none;
    launch.sceneFile.clear();
    expect(loadLaunchProject(launch, tree, none).success && !none,
           "without a project or a package the built-in demo plays");

    writeText(package / "second.zenith-project", projectJson());
    const GameLaunchResult ambiguous = findPackagedProject(package, found);
    expect(!ambiguous && ambiguous.error.find("--project") != std::string::npos,
           "a package with two projects asks for --project");
}

// The editor passes UTF-8 arguments; a path outside the ANSI code page must
// survive parsing and project loading.
void testNonAsciiPaths(const fs::path& root) {
    const std::string folder = reinterpret_cast<const char*>(u8"projét 日本");
    const fs::path directory = root / pathFromUtf8(folder);
    writeText(directory / "game.zenith-project", projectJson());
    const fs::path scene =
        directory / "scenes" / pathFromUtf8(reinterpret_cast<const char*>(u8"niveau é.zenith-scene"));
    writeText(scene, "{}");
    const std::string projectArgument =
        reinterpret_cast<const char*>((directory / "game.zenith-project").u8string().c_str());
    const std::string sceneArgument = reinterpret_cast<const char*>(scene.u8string().c_str());

    GameLaunchOptions launch;
    expect(parse({"--project", projectArgument, "--scene", sceneArgument}, launch).success &&
               launch.projectFile == directory / "game.zenith-project" && launch.sceneFile == scene,
           "UTF-8 arguments become the paths they spell");
    std::optional<ProjectConfig> project;
    const GameLaunchResult loaded = loadLaunchProject(launch, root / "build tree", project);
    expect(loaded.success && project && project->startupScene == scene && project->assetRoot == directory / "assets",
           "a project and scene under a non-ASCII folder load");
    if (!loaded) {
        std::cerr << loaded.error << '\n';
    }
}

// Runs after testPackagedProjectDiscovery, which makes root/package a package.
void testModuleLoadingPolicy(const fs::path& root) {
    expect(runtimeModuleLoading(root / "package" / "modules" / "game.zmodule", root / "package") ==
               GameModuleLoading::InPlace,
           "a module shipped in the package loads in place");
    expect(runtimeModuleLoading(root / "project" / "modules" / "game.zmodule", root / "package") ==
               GameModuleLoading::ShadowCopy,
           "a development project's module loads from a shadow copy so it can be rebuilt");
    expect(runtimeModuleLoading(root / "build tree" / "Debug" / "game.zmodule", root / "build tree") ==
               GameModuleLoading::ShadowCopy,
           "a module built inside a build tree (not a package) loads from a shadow copy");
}

const ComponentType* counterType(const Gameplay& gameplay) {
    return gameplay.componentTypes ? gameplay.componentTypes->find("test_counter") : nullptr;
}

int32_t counterField(const ComponentType& type, const Scene& scene, Entity entity, std::string_view key) {
    const void* component = type.find(scene, entity);
    for (const FieldDescriptor& field : type.fields()) {
        if (field.key == key && component != nullptr) {
            return field.value<int32_t>(component);
        }
    }
    return -1;
}

// A project whose scenes hold the test module's counter component.
ProjectConfig writeModuleProject(const fs::path& directory, const Gameplay& gameplay) {
    writeText(directory / "runtime.zenith-project", projectJson());
    ProjectConfig project;
    const ProjectConfigResult loaded = loadProjectConfig(directory / "runtime.zenith-project", project);
    expect(loaded.success, "the module project loads");
    fs::create_directories(project.assetRoot);
    const ComponentType* counter = counterType(gameplay);
    if (counter == nullptr) {
        return project;
    }
    AssetManager assets;
    for (const fs::path& scenePath :
         {project.sceneRoot / "start.zenith-scene", project.sceneRoot / "levels" / "second.zenith-scene"}) {
        Scene scene;
        const Entity entity = scene.createEntity("Counter");
        counter->addDefault(scene, entity);
        fs::create_directories(scenePath.parent_path());
        expect(SceneDocument::save(scenePath, scene, assets,
                                   {.assetRoot = project.assetRoot, .componentTypes = gameplay.componentTypes})
                   .success,
               "a scene holding a module component saves");
    }
    return project;
}

Entity onlyCounter(const Scene& scene, const ComponentType& type) {
    for (const Entity entity : scene.registry().view<NameComponent>()) {
        if (type.find(scene, entity) != nullptr) {
            return entity;
        }
    }
    return entt::null;
}

void testModuleProjectPlays(const fs::path& directory) {
    // Before every scene: the module's code must outlive them.
    RuntimeGameplay runtime;
    {
        ProjectConfig probe;
        probe.displayName = "Probe";
        probe.gameModule = ZENITH_TEST_GAME_MODULE;
        const GameplayResult loaded = loadRuntimeGameplay(&probe, GameModuleLoading::InPlace, runtime);
        expect(loaded.success && runtime.module != nullptr, "a project's game module loads");
        if (!loaded) {
            std::cerr << loaded.error << '\n';
            return;
        }
    }
    expect(runtime.presentation == GamePresentation::Project,
           "a module other than the demo's plays in the project-agnostic application");
    expect(runtime.module->loadedPath() == runtime.module->sourcePath(), "an in-place module loads the library itself");
    const ComponentType* counter = counterType(runtime.gameplay);
    expect(counter != nullptr, "the module's component types reach the gameplay");
    if (counter == nullptr) {
        return;
    }

    ProjectConfig project = writeModuleProject(directory, runtime.gameplay);
    PackageVerificationResult verified = verifyProjectScenes(project, runtime.gameplay);
    expect(verified.success && verified.scenes.size() == 2 &&
               verified.scenes[0] == project.sceneRoot / "levels" / "second.zenith-scene" &&
               verified.scenes[1] == project.sceneRoot / "start.zenith-scene",
           "verification loads every scene under the scene root, nested ones included");

    {
        ProjectApplication application(project, runtime.gameplay);
        Scene scene;
        AssetManager assets;
        PhysicsWorld physics;
        AudioSystem audio;
        audio.init(AudioConfig{.outputEnabled = false});
        ApplicationSession session(application, scene, assets, physics, audio);
        const SceneResult started = session.start();
        expect(started.success, "the project application plays the module's startup scene");
        if (!started) {
            std::cerr << started.error << '\n';
            return;
        }
        const Entity entity = onlyCounter(scene, *counter);
        expect(entity != entt::null, "the scene's module component loaded");
        if (entity == entt::null) {
            session.shutdown();
            return;
        }
        session.update(InputState{}, 1.0f / 60.0f);
        expect(counterField(*counter, scene, entity, "value") == 2,
               "Play Update systems run in their order: add one, then double");
        expect(counterField(*counter, scene, entity, "edit_ticks") == 0, "Edit-only systems do not run in the game");
        session.fixedUpdate(InputState{}, 1.0f / 60.0f);
        expect(counterField(*counter, scene, entity, "fixed_ticks") == 1, "FixedUpdate systems run each fixed step");
        session.shutdown();
    }

    {
        // --scene: a copy outside the scene root plays instead.
        const fs::path copy = directory / "standalone" / "copy.zenith-scene";
        fs::create_directories(copy.parent_path());
        fs::copy_file(project.sceneRoot / "levels" / "second.zenith-scene", copy, fs::copy_options::overwrite_existing);
        ProjectConfig standalone = project;
        standalone.startupScene = copy;
        ProjectApplication application(standalone, runtime.gameplay);
        Scene scene;
        AssetManager assets;
        PhysicsWorld physics;
        AudioSystem audio;
        audio.init(AudioConfig{.outputEnabled = false});
        ApplicationSession session(application, scene, assets, physics, audio);
        expect(session.start().success && onlyCounter(scene, *counter) != entt::null,
               "a scene outside the scene root plays with the project's module");
        session.shutdown();
    }

    writeText(project.sceneRoot / "broken.zenith-scene", "not a scene");
    verified = verifyProjectScenes(project, runtime.gameplay);
    expect(!verified && verified.error.find("broken.zenith-scene") != std::string::npos,
           "verification names a scene that does not load");
    fs::remove(project.sceneRoot / "broken.zenith-scene");
}

void testDemoAndLinkedGameplay() {
    RuntimeGameplay runtime;
    expect(loadRuntimeGameplay(nullptr, GameModuleLoading::InPlace, runtime).success && runtime.module == nullptr &&
               runtime.presentation == GamePresentation::Demo &&
               runtime.gameplay.componentTypes->find(DemoGameplay::CharacterControllerKey) != nullptr,
           "without a project the demo gameplay linked into the game plays");

    ProjectConfig moduleless;
    moduleless.displayName = "Showcase";
    expect(loadRuntimeGameplay(&moduleless, GameModuleLoading::InPlace, runtime).success && runtime.module == nullptr &&
               runtime.presentation == GamePresentation::Demo,
           "a project without a game module is walked with the linked demo gameplay");

    ProjectConfig demo;
    demo.displayName = "Demo";
    demo.gameModule = ZENITH_DEMO_GAME_MODULE;
    const GameplayResult loaded = loadRuntimeGameplay(&demo, GameModuleLoading::InPlace, runtime);
    expect(loaded.success && runtime.module != nullptr && runtime.module->name() == demoGameModuleName &&
               runtime.presentation == GamePresentation::Demo,
           "the demo's module plays with the demo's presentation");
    if (!loaded) {
        std::cerr << loaded.error << '\n';
    }

    ProjectConfig missing;
    missing.displayName = "Missing";
    missing.gameModule = fs::temp_directory_path() / "zenith_no_such_module.zmodule";
    const GameplayResult failed = loadRuntimeGameplay(&missing, GameModuleLoading::InPlace, runtime);
    expect(!failed && failed.error.find("Missing") != std::string::npos && runtime.module != nullptr,
           "a module that does not load fails naming the project and keeps the loaded gameplay");
}
} // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "zenith_project_runtime_tests";
    std::error_code error;
    fs::remove_all(root, error);
    fs::create_directories(root, error);

    testArguments();
    testPackagedProjectDiscovery(root);
    testNonAsciiPaths(root);
    testModuleLoadingPolicy(root);
    testModuleProjectPlays(root / "module project");
    testDemoAndLinkedGameplay();

    fs::remove_all(root, error);
    return failures == 0 ? 0 : 1;
}
