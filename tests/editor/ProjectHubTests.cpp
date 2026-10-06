// The Project Hub and what it is built from: the recent projects list, the
// C++ game template generator, and the hub's headless UI. With
// --build-generated-module [--template <name>] the test instead generates a
// project (from the C++ game template unless named) and
// configures and builds its game module against this Zenith build with CMake
// (in a temporary folder, or in the folder given after the flag, which is
// kept). With --measure-iteration [--preset] [folder] it times the editor's
// build of a generated FPS project while gameplay code changes
// (measureIteration).

#include "HeadlessImGui.hpp"

#include "editor/project/BuildRunner.hpp"
#include "editor/project/CodeGenerator.hpp"
#include "editor/project/ExternalEditor.hpp"
#include "editor/project/FastProjectBuild.hpp"
#include "editor/project/ProjectHub.hpp"
#include "editor/project/ProjectTemplate.hpp"
#include "editor/project/RecentProjects.hpp"
#include "engine/application/Application.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/audio/AudioSystem.hpp"
#include "engine/behaviour/BehaviourRegistry.hpp"
#include "engine/gameplay/FirstPerson.hpp"
#include "engine/graphics/Rendering.hpp"
#include "engine/input/InputActions.hpp"
#include "engine/module/GameModule.hpp"
#include "engine/module/Gameplay.hpp"
#include "engine/physics/CharacterController.hpp"
#include "engine/physics/PhysicsWorld.hpp"
#include "engine/project/ProjectConfig.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/Scene.hpp"
#include "engine/ui/GameUi.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace fs = std::filesystem;

namespace {
int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

std::string readText(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void writeText(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

bool contains(std::string_view text, std::string_view fragment) {
    return text.find(fragment) != std::string_view::npos;
}

bool hasEntityNamed(const Scene& scene, std::string_view name) {
    const auto view = scene.registry().view<const NameComponent>();
    for (const Entity entity : view) {
        if (view.get<const NameComponent>(entity).name == name) {
            return true;
        }
    }
    return false;
}

ProjectTemplateRequest templateRequest(std::string name, const fs::path& parent) {
    return {.displayName = std::move(name),
            .parentDirectory = parent,
            .templateDirectory = fs::path(ZENITH_TEMPLATE_DIR) / cppGameTemplateName,
            .engineRoot = ZENITH_ENGINE_BUILD_DIR,
            .engineAssetRoot = ZENITH_ASSET_DIR,
            .buildConfiguration = ZENITH_EDITOR_CONFIGURATION};
}

std::chrono::system_clock::time_point at(int64_t seconds) {
    return std::chrono::system_clock::time_point(std::chrono::seconds(seconds));
}

void testProjectIdentifier() {
    expect(projectIdentifier("My Game") == "my_game", "identifier: spaces become underscores, letters lower case");
    expect(projectIdentifier("  Space--Race 2  ") == "space_race_2", "identifier: separators collapse and trim");
    expect(projectIdentifier("2048") == "game_2048", "identifier: a leading digit gets a prefix");
    expect(projectIdentifier("- _ -").empty(), "identifier: a name without letters or digits has none");
    expect(projectIdentifier("All") == "all_game" && projectIdentifier("Test") == "test_game",
           "identifier: names CMake reserves for targets are extended");
}

void testRecentProjects(const fs::path& root) {
    const fs::path existing = root / "existing" / "existing.zenith-project";
    writeText(existing, "{}");
    const fs::path listFile = root / "settings" / "recent-projects.json";
    {
        RecentProjects recent(listFile);
        expect(static_cast<bool>(recent.load()) && recent.entries().empty(), "recent: a missing list loads empty");
        recent.recordOpened(existing, "Existing", at(100));
        recent.recordOpened(root / "missing" / "missing.zenith-project", "Missing", at(200));
        // The same file named another way moves to the front instead of
        // being listed twice.
        recent.recordOpened(root / "existing" / ".." / "existing" / "existing.zenith-project", "Existing again",
                            at(300));
        expect(recent.entries().size() == 2, "recent: one project is listed once");
        expect(recent.entries().front().displayName == "Existing again" &&
                   recent.entries().front().lastOpened == at(300),
               "recent: the most recently opened project is first");
        expect(static_cast<bool>(recent.save()), "recent: the list saves");
    }
    RecentProjects reloaded(listFile);
    expect(static_cast<bool>(reloaded.load()) && reloaded.entries().size() == 2, "recent: the list round trips");
    if (reloaded.entries().size() == 2) {
        const RecentProject& first = reloaded.entries()[0];
        const RecentProject& second = reloaded.entries()[1];
        expect(first.projectFile == fs::absolute(existing).lexically_normal() && first.lastOpened == at(300),
               "recent: paths and times round trip in order");
        expect(!recentProjectMissing(first) && recentProjectMissing(second),
               "recent: a project whose file is gone is flagged missing");
        expect(reloaded.remove(second.projectFile) && reloaded.entries().size() == 1,
               "recent: a missing project can be removed");
    }

    for (int index = 0; index < 25; ++index) {
        reloaded.recordOpened(root / ("p" + std::to_string(index)) / "p.zenith-project", "P", at(1000 + index));
    }
    expect(reloaded.entries().size() == RecentProjects::maximumEntries,
           "recent: the list keeps at most maximumEntries projects");
    expect(reloaded.entries().front().lastOpened == at(1024) && reloaded.entries().back().lastOpened == at(1005),
           "recent: the oldest projects are dropped first");

    writeText(root / "bad" / "recent-projects.json", "{ not json");
    RecentProjects malformed(root / "bad" / "recent-projects.json");
    expect(!malformed.load() && malformed.entries().empty(), "recent: a malformed list is an error");
    writeText(root / "typed" / "recent-projects.json", R"({"version": "1", "projects": []})");
    RecentProjects mistyped(root / "typed" / "recent-projects.json");
    expect(!mistyped.load(), "recent: a version that is not a number is an error, not an exception");
}

void testGenerateProject(const fs::path& root) {
    AssetManager assets;
    const ProjectTemplateResult created = generateProjectFromTemplate(templateRequest("Space Race", root), assets);
    expect(static_cast<bool>(created), "template: the C++ game project generates");
    if (!created) {
        std::cerr << created.error << '\n';
        return;
    }
    const fs::path directory = root / "space_race";
    expect(created.projectFile == directory / "space_race.zenith-project", "template: the project file is named");

    ProjectConfig project;
    expect(static_cast<bool>(loadProjectConfig(created.projectFile, project)), "template: the project file loads");
    expect(project.displayName == "Space Race", "template: the display name is kept");
    expect(project.assetRoot == directory / "assets" && project.sceneRoot == directory / "scenes",
           "template: assets/ and scenes/ are the roots");
    expect(project.startupScene == directory / "scenes" / "main.zenith-scene", "template: the startup scene is set");
    expect(project.gameModule == directory / "modules" / "space_race.zmodule",
           "template: the game module is modules/<identifier>.zmodule");

    AssetManager sceneAssets;
    Scene scene;
    const ProjectConfigResult loaded = loadProjectStartupScene(project, sceneAssets, scene);
    expect(static_cast<bool>(loaded), "template: the starter scene loads");
    for (const char* name : {"Ground", "Cube", "Sun", "Environment", "Camera"}) {
        expect(hasEntityNamed(scene, name), std::string("template: the starter scene has ") + name);
    }
    expect(scene.registry().view<const CameraComponent>().size() == 1, "template: the starter scene has a camera");

    const std::string rootCmake = readText(directory / "CMakeLists.txt");
    const std::string moduleCmake = readText(directory / "src" / "CMakeLists.txt");
    const std::string moduleSource = readText(directory / "src" / "GameModule.cpp");
    const std::string gameplay = readText(directory / "src" / "SpinnerGameplay.cpp");
    const std::string readme = readText(directory / "README.md");
    expect(contains(rootCmake, "project(space_race") && contains(rootCmake, "ZENITH_ROOT"),
           "template: the root CMakeLists names the project and ZENITH_ROOT");
    expect(contains(moduleCmake, "add_library(space_race MODULE") &&
               contains(moduleCmake, "modules/space_race.zmodule"),
           "template: the module target builds and copies modules/<identifier>.zmodule");
    expect(contains(moduleSource, "ZENITH_GAME_MODULE(\"space_race\""), "template: the module is named");
    expect(contains(moduleCmake, "# zenith:sources") && contains(moduleSource, "// zenith:includes") &&
               contains(moduleSource, "// zenith:register"),
           "template: the code generators' markers are in place");
    expect(!fs::exists(directory / templateSnippetDirectoryName), "template: the snippets are not copied");
    expect(contains(gameplay, "Fields<SpinnerComponent>") && contains(gameplay, "addSystem"),
           "template: the example component is reflected and its system registered");
    expect(contains(readme, "# Space Race"), "template: the README names the project");
    for (const auto& entry : fs::recursive_directory_iterator(directory)) {
        if (entry.is_regular_file() && contains(readText(entry.path()), "{{")) {
            expect(false, "template: no token is left in " + entry.path().string());
        }
    }
    expect(fs::is_regular_file(directory / "assets" / "primitives" / "cube.gltf") &&
               fs::is_regular_file(directory / "assets" / "README.md") && fs::is_regular_file(directory / ".gitignore"),
           "template: the starter model, the assets README, and .gitignore are written");

    const nlohmann::json presets = nlohmann::json::parse(readText(directory / "CMakePresets.json"), nullptr, false);
    const bool presetValid = presets.is_object() && presets["configurePresets"].is_array() &&
                             presets["configurePresets"][0]["name"] == "zenith" &&
                             presets["configurePresets"][0]["cacheVariables"]["ZENITH_ROOT"]["value"] ==
                                 fs::path(ZENITH_ENGINE_BUILD_DIR).generic_string() &&
                             presets["buildPresets"][0]["configuration"] == ZENITH_EDITOR_CONFIGURATION;
    expect(presetValid, "template: the zenith preset names ZENITH_ROOT and the editor's configuration");
    if (fs::is_regular_file(fs::path(ZENITH_ENGINE_BUILD_DIR) / engineDescriptionFileName)) {
        expect(presets["configurePresets"][0].contains("generator"),
               "template: the preset repeats the engine's generator");
    }

    const std::optional<BuildRequest> build = projectBuildRequest(created.projectFile);
    expect(build && build->workingDirectory == directory &&
               build->command == "cmake --preset zenith && cmake --build --preset zenith",
           "template: the editor builds the project's preset from its folder");
    expect(!projectBuildRequest(root / "elsewhere" / "none.zenith-project"),
           "template: a project without presets has no project build");

    const DeveloperEnvironment environment{.variables = {{"ZENITH_TEST_VARIABLE", "1"}},
                                           .ninja = fs::path("C:/tools/ninja.exe")};
    const std::optional<BuildRequest> fast = fastProjectBuildRequest(created.projectFile, environment);
    const fs::path fastTree = directory / ".zenith" / "build";
    const auto hasArgument = [&fast](std::string_view argument) {
        return fast && std::ranges::find(fast->prepareArguments, argument) != fast->prepareArguments.end();
    };
    expect(fast && fast->workingDirectory == directory && fast->environment == environment.variables &&
               fast->arguments == std::vector<std::string>{"cmake", "--build", fastTree.generic_string()} &&
               fast->prepareUnlessExists == fastTree / "build.ninja",
           "template: the fast build builds .zenith/build with the captured environment, configuring it once");
    expect(hasArgument("Ninja") && hasArgument("-DCMAKE_MAKE_PROGRAM=C:/tools/ninja.exe") &&
               hasArgument("-DZENITH_ROOT=" + fs::path(ZENITH_ENGINE_BUILD_DIR).generic_string()) &&
               hasArgument(std::string("-DCMAKE_BUILD_TYPE=") + ZENITH_EDITOR_CONFIGURATION),
           "template: the fast build configures Ninja for the preset's engine and configuration");
    expect(!fastProjectBuildRequest(root / "elsewhere" / "none.zenith-project", environment),
           "template: a project without presets has no fast build");
    expect(projectIdeLocation(created.projectFile).file == directory.lexically_normal().make_preferred(),
           "hub: Open in IDE opens the project folder");
    const ExternalEditorCommandResult ide =
        expandExternalEditorCommand(R"(code -g "{file}:{line}:{column}")", projectIdeLocation(created.projectFile));
    expect(ide && ide.arguments.size() == 3 && contains(ide.arguments[2], "space_race:1:1"),
           "hub: the IDE command receives the project folder");
}

void testGenerateFailures(const fs::path& root) {
    AssetManager assets;
    const fs::path occupied = root / "occupied";
    writeText(occupied / "keep.txt", "mine");
    const ProjectTemplateResult existing = generateProjectFromTemplate(templateRequest("Occupied", root), assets);
    expect(!existing && contains(existing.error, "not empty"), "template: a non-empty folder is refused");
    expect(readText(occupied / "keep.txt") == "mine", "template: a refused folder is left alone");

    expect(!generateProjectFromTemplate(templateRequest("Bad \"name\"", root), assets),
           "template: names with quotes are refused");
    expect(!generateProjectFromTemplate(templateRequest("", root), assets), "template: an empty name is refused");

    const fs::path brokenTemplate = root / "broken_template";
    writeText(brokenTemplate / "README.md", "{{PROJECT_NAME}} uses {{NOT_A_TOKEN}}\n");
    ProjectTemplateRequest broken = templateRequest("Broken", root);
    broken.templateDirectory = brokenTemplate;
    const ProjectTemplateResult failed = generateProjectFromTemplate(broken, assets);
    expect(!failed && contains(failed.error, "NOT_A_TOKEN"), "template: an unknown token is an error");
    expect(!fs::exists(root / "broken"), "template: a failed generation removes the folder it created");
}

// Draws one hub frame and returns what it chose.
std::optional<fs::path> drawHub(ProjectHub& hub, AssetManager& assets) {
    ImGui::NewFrame();
    std::optional<fs::path> chosen = hub.draw(assets);
    ImGui::Render();
    return chosen;
}

ImGuiID hubItem(std::initializer_list<const char*> path) {
    const ImGuiWindow* window = HeadlessImGuiContext::findWindow("Project Hub");
    if (window == nullptr) {
        return 0;
    }
    ImGuiID id = window->ID;
    for (const char* part : path) {
        id = ImHashStr(part, 0, id);
    }
    return id;
}

void testHubDraw(const fs::path& root) {
    HeadlessImGuiContext imgui;
    AssetManager assets;
    const fs::path existing = root / "listed" / "listed.zenith-project";
    writeText(existing, "{}");
    const fs::path missing = fs::absolute(root / "gone" / "gone.zenith-project").lexically_normal();
    RecentProjects recent(root / "settings" / "recent-projects.json");
    recent.recordOpened(missing, "Gone", at(10));
    recent.recordOpened(existing, "Listed", at(20));

    const std::array templateRoots{fs::path(ZENITH_TEMPLATE_DIR)};
    ProjectHub hub(recent, {.templates = listProjectTemplates(templateRoots),
                            .engineRoot = ZENITH_ENGINE_BUILD_DIR,
                            .engineAssetRoot = ZENITH_ASSET_DIR,
                            .buildConfiguration = ZENITH_EDITOR_CONFIGURATION,
                            .defaultProjectLocation = root / "created"});
    expect(!drawHub(hub, assets), "hub: drawing alone chooses nothing");

    // Rows push their project file's path as their id.
    const std::string existingKey = fs::absolute(existing).lexically_normal().string();
    HeadlessImGuiContext::activateNextFrame(
        hubItem({"##hub_columns", "##recent_projects", existingKey.c_str(), "Open"}));
    const std::optional<fs::path> opened = drawHub(hub, assets);
    expect(opened && *opened == fs::absolute(existing).lexically_normal(), "hub: Open chooses a recent project");

    HeadlessImGuiContext::activateNextFrame(
        hubItem({"##hub_columns", "##recent_projects", missing.string().c_str(), "Remove"}));
    drawHub(hub, assets);
    expect(recent.entries().size() == 1 && recent.entries().front().displayName == "Listed",
           "hub: Remove drops a missing project from the list");
    RecentProjects saved(recent.file());
    expect(saved.load() && saved.entries().size() == 1, "hub: removing saves the list");

    HeadlessImGuiContext::activateNextFrame(hubItem({"##hub_columns", "Create Project"}));
    const std::optional<fs::path> created = drawHub(hub, assets);
    expect(created && *created == root / "created" / "my_game" / "my_game.zenith-project" &&
               fs::is_regular_file(*created),
           "hub: Create Project generates the template project and chooses it");
    expect(!hub.messageIsError(), "hub: creating reports success");
}

// Whether the HUD drew text exactly matching text this frame.
bool hudShows(const GameUiContext& ui, std::string_view text) {
    return std::ranges::any_of(ui.commands(), [text](const UiCommand& command) {
        const auto* drawn = std::get_if<UiTextCommand>(&command.payload);
        return drawn != nullptr && drawn->text == text;
    });
}

// Plays the FPS scene as the game does (GameplayRunner over the module, with
// the project's input actions and physics stepping), shoots the first target
// twice, and checks the HUD counts it; then pauses (the roadmap's C6 exit
// criterion 3, without a window).
int playFpsScene(const GameModule& module, const ProjectConfig& project) {
    Logger logger{{.echoToStderr = false}};
    AssetManager assets{&logger};
    Scene scene;
    PhysicsWorld physics;
    if (!physics.init() || !physics.setLayers(project.physicsLayerTable())) {
        std::cerr << "FAILED: the physics world starts\n";
        return 1;
    }
    AudioSystem audio;
    audio.init(AudioConfig{.outputEnabled = false});
    ApplicationControl control;
    RenderRuntime rendering;
    GameUiContext ui;
    ApplicationContext context{scene, assets, physics, audio, control, rendering, ui, logger};
    Gameplay gameplay = gameplayFromModule(module);
    InputActionMap actions = *gameplay.inputActions;
    if (!loadProjectInputActions(project, actions) ||
        !loadProjectStartupScene(project, assets, scene, gameplay.componentTypes)) {
        std::cerr << "FAILED: the FPS scene and input actions load\n";
        return 1;
    }
    gameplay.inputActions = std::make_shared<const InputActionMap>(std::move(actions));

    // Put the first target straight ahead of the camera, which starts
    // looking down -Z, before physics builds its body.
    scene.updateWorldTransforms();
    const auto cameras = scene.registry().view<const FirstPersonCameraComponent>();
    const Entity camera = cameras.empty() ? entt::null : cameras.front();
    Entity target = entt::null;
    for (const auto [entity, name] : scene.registry().view<const NameComponent>().each()) {
        target = name.name == "Target 1" ? entity : target;
    }
    if (camera == entt::null || target == entt::null) {
        std::cerr << "FAILED: the FPS scene has its camera and first target\n";
        return 1;
    }
    const glm::vec3 eye(scene.registry().get<TransformComponent>(camera).world[3]);
    glm::mat4& targetLocal = scene.registry().get<TransformComponent>(target).local;
    targetLocal[3] = glm::vec4(eye + glm::vec3(0.0f, 0.0f, -6.0f), 1.0f);
    scene.markTransformDirty(target);
    const SceneEntityId targetId = scene.id(target);

    GameplayRunner runner(gameplay);
    runner.start(context, project.assetRoot);
    constexpr float step = 1.0f / 60.0f;
    // As the engine does: fixed steps (and physics) run only while the game's
    // clock advances.
    const auto frame = [&](const InputState& input) {
        runner.update(context, input, step);
        if (fixedStepAccumulation(runner.simulationClock(), step) > 0.0f) {
            runner.fixedUpdate(context, input, step);
            physics.step(scene, assets, step);
            runner.afterPhysicsStep(context);
        }
        runner.lateUpdate(context);
        scene.updateWorldTransforms();
        ui.beginFrame({1280, 720}, input);
        runner.drawUi(context);
    };
    const auto frames = [&](int count) {
        for (int index = 0; index < count; ++index) {
            frame(InputState{});
        }
    };
    InputState fire;
    fire.setMouseButtonDown(MouseButton::Left);
    fire.setMouseButtonPressed(MouseButton::Left);
    frames(5);
    const bool counted = hudShows(ui, "Targets 0 / 5");
    frame(fire);
    frames(30);
    frame(fire);
    frames(3);
    const bool destroyed = scene.findEntity(targetId) == entt::null;
    const bool scored = hudShows(ui, "Targets 1 / 5");

    // Walking and looking: W moves the player, the mouse turns the view.
    const auto characters = scene.registry().view<const CharacterControllerComponent>();
    const Entity player = characters.empty() ? entt::null : characters.front();
    const glm::vec3 startPosition(scene.registry().get<TransformComponent>(player).world[3]);
    const float startYaw = scene.registry().get<FirstPersonCameraComponent>(camera).yawRadians;
    InputState walk;
    walk.setKeyDown(Key::W);
    walk.mouseDelta = {40.0f, 0.0f};
    for (int index = 0; index < 30; ++index) {
        frame(walk);
    }
    const float walkedDistance =
        glm::distance(startPosition, glm::vec3(scene.registry().get<TransformComponent>(player).world[3]));
    const bool walked = walkedDistance > 0.5f;
    const bool looked = scene.registry().get<FirstPersonCameraComponent>(camera).yawRadians != startYaw;
    std::cout << "walked " << walkedDistance << " m, yaw "
              << scene.registry().get<FirstPersonCameraComponent>(camera).yawRadians << '\n';
    InputState pause;
    pause.setKeyDown(Key::Escape);
    pause.setKeyPressed(Key::Escape);
    frame(pause);
    frame(InputState{});
    const bool paused = hudShows(ui, "Paused") && runner.services()->time().paused();
    // Paused, the world holds still: walking and looking do nothing.
    const glm::vec3 pausedPosition(scene.registry().get<TransformComponent>(player).world[3]);
    const float pausedYaw = scene.registry().get<FirstPersonCameraComponent>(camera).yawRadians;
    for (int index = 0; index < 10; ++index) {
        frame(walk);
    }
    const bool held =
        glm::distance(pausedPosition, glm::vec3(scene.registry().get<TransformComponent>(player).world[3])) < 1e-4f &&
        scene.registry().get<FirstPersonCameraComponent>(camera).yawRadians == pausedYaw;
    frame(pause);
    const bool resumed = !runner.services()->time().paused();
    runner.stop(context);
    if (!counted || !walked || !looked || !destroyed || !scored || !paused || !held || !resumed) {
        std::cerr << "FAILED: playing the FPS scene: targets counted " << counted << ", walked " << walked
                  << ", looked " << looked << ", target destroyed " << destroyed << ", HUD updated " << scored
                  << ", paused " << paused << ", held while paused " << held << ", resumed " << resumed << '\n';
        return 1;
    }
    std::cout << "Played the FPS scene: shot a target, the HUD counted it, and the game paused\n";
    return 0;
}

// The FPS template's module registers its behaviours and systems, and its
// starter scene loads with them, keeping no module component as JSON.
int checkFpsModule(const GameModule& module, const ProjectConfig& project) {
    const bool registered =
        module.behaviours()->find("hitscan_weapon") != nullptr && module.behaviours()->find("target") != nullptr &&
        module.behaviours()->find("goal_zone") != nullptr && module.systems()->find("built_game.score") != nullptr &&
        module.systems()->find("built_game.hud") != nullptr;
    if (!registered) {
        std::cerr << "FAILED: the FPS module did not register its behaviours and systems\n";
        return 1;
    }
    // Destroyed before the module, which holds the components' code.
    AssetManager sceneAssets;
    Scene scene;
    if (const ProjectConfigResult loaded =
            loadProjectStartupScene(project, sceneAssets, scene, module.componentTypes());
        !loaded) {
        std::cerr << "FAILED: the FPS scene does not load with its module: " << loaded.error << '\n';
        return 1;
    }
    if (!scene.registry().view<PreservedComponentsComponent>().empty()) {
        std::cerr << "FAILED: the FPS scene keeps module components its module did not read\n";
        return 1;
    }
    std::cout << "FPS module built and its scene loaded\n";
    return playFpsScene(module, project);
}

int buildGeneratedModule(const fs::path& root, std::string_view templateName) {
    AssetManager assets;
    ProjectTemplateRequest request = templateRequest("Built Game", root);
    request.templateDirectory = fs::path(ZENITH_TEMPLATE_DIR) / templateName;
    const ProjectTemplateResult created = generateProjectFromTemplate(request, assets);
    if (!created) {
        std::cerr << "FAILED: generate: " << created.error << '\n';
        return 1;
    }
    // The code generators' output must compile with the template's warnings.
    const fs::path snippets = fs::path(ZENITH_TEMPLATE_DIR) / cppGameTemplateName / templateSnippetDirectoryName;
    const std::array<CodeGenerationRequest, 3> generatedTypes{
        CodeGenerationRequest{.kind = GeneratedCodeKind::Behaviour,
                              .name = "DoorOpener",
                              .fields = {{"openSpeed", GeneratedFieldType::Float},
                                         {"locked", GeneratedFieldType::Bool},
                                         {"uses", GeneratedFieldType::Int},
                                         {"openOffset", GeneratedFieldType::Vec3},
                                         {"sound", GeneratedFieldType::String},
                                         {"target", GeneratedFieldType::Entity}}},
        CodeGenerationRequest{.kind = GeneratedCodeKind::Component,
                              .name = "Health",
                              .fields = {{"maxHealth", GeneratedFieldType::Float}}},
        CodeGenerationRequest{.kind = GeneratedCodeKind::System, .name = "Regenerate"},
    };
    for (const CodeGenerationRequest& generation : generatedTypes) {
        if (const CodeGenerationResult generated = generateModuleCode(created.projectFile, snippets, generation);
            !generated) {
            std::cerr << "FAILED: generating " << generation.name << ": " << generated.error << '\n';
            return 1;
        }
    }
    const std::optional<BuildRequest> build = projectBuildRequest(created.projectFile);
    if (!build) {
        std::cerr << "FAILED: the generated project has no build\n";
        return 1;
    }
    const fs::path log = root / "build.log";
    const std::string cmake = std::string("\"") + ZENITH_TEST_CMAKE + "\"";
    // The editor's command, with this build's CMake.
#if defined(_WIN32)
    const std::string changeDirectory = "cd /d \"";
#else
    const std::string changeDirectory = "cd \"";
#endif
    const std::string command = changeDirectory + build->workingDirectory.string() + "\" && " + cmake +
                                " --preset zenith > \"" + log.string() + "\" 2>&1 && " + cmake +
                                " --build --preset zenith >> \"" + log.string() + "\" 2>&1";
#if defined(_WIN32)
    // cmd /c strips one pair of quotes around the whole command.
    const int exitCode = std::system(("\"" + command + "\"").c_str());
#else
    const int exitCode = std::system(command.c_str());
#endif
    if (exitCode != 0) {
        std::cerr << "FAILED: building the generated module exited with " << exitCode << ":\n" << readText(log);
        return 1;
    }
    ProjectConfig project;
    if (!loadProjectConfig(created.projectFile, project) || !fs::is_regular_file(project.gameModule)) {
        std::cerr << "FAILED: the build did not copy the module to " << project.gameModule << '\n';
        return 1;
    }
    std::unique_ptr<GameModule> module;
    if (const GameModuleResult loaded =
            GameModule::load(project.gameModule, {.shadowDirectory = root / "shadow"}, module);
        !loaded) {
        std::cerr << "FAILED: the generated module does not load: " << loaded.error << '\n';
        return 1;
    }
    if (templateName == fpsGameTemplateName) {
        return checkFpsModule(*module, project);
    }
    const bool registered = module->name() == "built_game" && module->componentTypes()->find("spinner") != nullptr &&
                            module->systems()->find("built_game.spin") != nullptr;
    if (!registered) {
        std::cerr << "FAILED: the generated module did not register its component and system\n";
        return 1;
    }
    const bool generatedRegistered = module->behaviours()->find("door_opener") != nullptr &&
                                     module->componentTypes()->find("door_opener") != nullptr &&
                                     module->componentTypes()->find("health") != nullptr &&
                                     module->systems()->find("built_game.regenerate") != nullptr;
    if (!generatedRegistered) {
        std::cerr << "FAILED: the generated behaviour, component, and system did not register\n";
        return 1;
    }
    std::cout << "Generated module built and loaded\n";
    return 0;
}

// Runs request as the editor's Build does (BuildRunner) and returns its
// seconds, or nullopt (with the output's tail printed) when it fails.
std::optional<double> timeBuild(const BuildRequest& request, std::string_view name) {
    BuildRunner runner;
    const auto begin = std::chrono::steady_clock::now();
    if (const ChildProcessResult started = runner.start(request); !started) {
        std::cerr << "FAILED: " << name << ": the build did not start: " << started.error << '\n';
        return std::nullopt;
    }
    const auto deadline = begin + std::chrono::minutes(10);
    while (!runner.poll().finished) {
        if (std::chrono::steady_clock::now() > deadline) {
            runner.cancel();
            std::cerr << "FAILED: " << name << ": the build took over ten minutes\n";
            return std::nullopt;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    if (runner.state() != BuildState::Succeeded) {
        std::cerr << "FAILED: " << name << ": the build failed:\n";
        const std::deque<BuildOutputLine>& output = runner.output();
        for (size_t index = output.size() > 40 ? output.size() - 40 : 0; index < output.size(); ++index) {
            std::cerr << "  " << output[index].text << '\n';
        }
        return std::nullopt;
    }
    return seconds;
}

// Changes file as a save in an editor does: new content, so every build tool
// sees it.
void appendEdit(const fs::path& file) {
    std::ofstream output(file, std::ios::app);
    output << "// edited\n";
}

// What iterating on gameplay code costs through the editor's own build
// request (projectBuildRequest): a generated FPS project's first build, a
// build with nothing changed, one with one source changed, one with a header
// four sources include changed, and loading the built module. Run by hand
// for numbers; CTest only checks that it completes.
int measureIteration(const fs::path& root, bool preset) {
    AssetManager assets;
    ProjectTemplateRequest request = templateRequest("Iteration", root);
    request.templateDirectory = fs::path(ZENITH_TEMPLATE_DIR) / fpsGameTemplateName;
    const ProjectTemplateResult created = generateProjectFromTemplate(request, assets);
    if (!created) {
        std::cerr << "FAILED: generate: " << created.error << '\n';
        return 1;
    }
    // What the editor runs: the fast build once its environment is captured,
    // else (or with --preset) the project's preset build.
    std::optional<BuildRequest> build;
    if (!preset) {
        FastProjectBuild fast(created.projectFile, root / "developer-environment.json");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
        while (fast.pending() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        build = fast.request();
        if (!build) {
            std::cout << "fast build unavailable: " << fast.unavailableReason() << '\n';
        }
    }
    if (!build) {
        build = projectBuildRequest(created.projectFile);
    }
    if (!build) {
        std::cerr << "FAILED: the generated project has no build\n";
        return 1;
    }
    const fs::path sources = created.projectFile.parent_path() / "src";

    struct Step {
        const char* name;
        std::function<void()> before;
    };

    const std::array<Step, 4> steps{
        Step{"iteration.first_build", [] {}},
        Step{"iteration.no_change", [] {}},
        Step{"iteration.one_source", [&] { appendEdit(sources / "HitscanWeapon.cpp"); }},
        Step{"iteration.shared_header", [&] { appendEdit(sources / "FpsGame.hpp"); }},
    };
    for (const Step& step : steps) {
        step.before();
        const std::optional<double> seconds = timeBuild(*build, step.name);
        if (!seconds) {
            return 1;
        }
        std::cout << step.name << ' ' << *seconds << " s\n";
    }
    ProjectConfig project;
    if (!loadProjectConfig(created.projectFile, project)) {
        std::cerr << "FAILED: the generated project does not load\n";
        return 1;
    }
    std::unique_ptr<GameModule> module;
    const auto loadBegin = std::chrono::steady_clock::now();
    if (const GameModuleResult loaded =
            GameModule::load(project.gameModule, {.shadowDirectory = root / "shadow"}, module);
        !loaded) {
        std::cerr << "FAILED: the built module does not load: " << loaded.error << '\n';
        return 1;
    }
    std::cout << "iteration.module_load "
              << std::chrono::duration<double>(std::chrono::steady_clock::now() - loadBegin).count() << " s\n";
    std::cout << "build: " << build->command << '\n';
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    std::error_code error;
    if (argc > 1 && std::string_view(argv[1]) == "--measure-iteration") {
        // --preset measures the project's preset build instead of the fast one.
        int next = 2;
        const bool preset = argc > 2 && std::string_view(argv[2]) == "--preset";
        if (preset) {
            next = 3;
        }
        // An explicit folder keeps the project.
        if (argc > next) {
            return measureIteration(fs::absolute(argv[next]), preset);
        }
        // Not under the temporary directory: MSBuild's file tracker ignores
        // reads from it, so a header edit there would rebuild nothing.
        const fs::path measureRoot = fs::current_path() / "module_iteration_benchmark";
        fs::remove_all(measureRoot, error);
        const int result = measureIteration(measureRoot, preset);
        fs::remove_all(measureRoot, error);
        return result;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--build-generated-module") {
        // --template <name> chooses another template than the C++ game.
        int next = 2;
        std::string_view templateName = cppGameTemplateName;
        if (argc > 3 && std::string_view(argv[2]) == "--template") {
            templateName = argv[3];
            next = 4;
        }
        // An explicit folder keeps the project, for running the editor on it.
        if (argc > next) {
            return buildGeneratedModule(fs::absolute(argv[next]), templateName);
        }
        // Its own folder: CTest runs this beside the other tests.
        const fs::path buildRoot =
            fs::temp_directory_path() / ("zenith_" + std::string(templateName) + "_template_module_build");
        fs::remove_all(buildRoot, error);
        const int result = buildGeneratedModule(buildRoot, templateName);
        fs::remove_all(buildRoot, error);
        return result;
    }

    const fs::path root = fs::temp_directory_path() / "zenith_project_hub_tests";
    fs::remove_all(root, error);
    fs::create_directories(root);

    testProjectIdentifier();
    testRecentProjects(root / "recent");
    testGenerateProject(root / "generate");
    testGenerateFailures(root / "failures");
    testHubDraw(root / "hub");

    fs::remove_all(root, error);
    if (failures != 0) {
        std::cerr << failures << " project hub test(s) failed\n";
        return 1;
    }
    std::cout << "Project hub tests passed\n";
    return 0;
}
