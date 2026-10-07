// Runs a generated project's game module end to end, as standalone executable
// zenith_generated_module_runs (docs/TESTING.md, "Standalone tests"):
// --build-generated-module [--template <name>] [folder] generates a project
// (from the C++ game template unless named) and configures and builds its game
// module against this Zenith build with CMake, in a temporary folder or in the
// folder given, which is kept; for the FPS template it then plays the scene
// with the built module. --measure-iteration [--preset] [folder] times the
// editor's build of a generated FPS project while gameplay code changes.

#include "ProjectGeneration.hpp"
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
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "support/Files.hpp"
#include "support/TempDirectory.hpp"

namespace fs = std::filesystem;
using testsupport::readText;

namespace {

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
        // A directory of its own: CTest runs this beside the other tests.
        const testsupport::TempDirectory buildRoot(std::string(templateName) + "-template-module-build");
        return buildGeneratedModule(buildRoot.path(), templateName);
    }
    std::cerr << "Usage: zenith_generated_module_runs --build-generated-module [--template <name>] [folder]\n"
                 "       zenith_generated_module_runs --measure-iteration [--preset] [folder]\n";
    return 2;
}
