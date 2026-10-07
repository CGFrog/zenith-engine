// The Project Hub and what it is built from: the recent projects list, the
// C++ game template generator, and the hub's headless UI. Building and playing
// a generated project's module is zenith_generated_module_runs
// (GeneratedModuleRuns.cpp).

#include "HeadlessImGui.hpp"
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

#include "support/Doctest.hpp"

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

TEST_SUITE_BEGIN("ProjectHub");

namespace fs = std::filesystem;
using testsupport::readText;
using testsupport::writeText;

namespace {

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

std::chrono::system_clock::time_point at(int64_t seconds) {
    return std::chrono::system_clock::time_point(std::chrono::seconds(seconds));
}

TEST_CASE("ProjectHub: project identifier") {
    CHECK_MESSAGE(projectIdentifier("My Game") == "my_game",
                  "identifier: spaces become underscores, letters lower case");
    CHECK_MESSAGE(projectIdentifier("  Space--Race 2  ") == "space_race_2", "identifier: separators collapse and trim");
    CHECK_MESSAGE(projectIdentifier("2048") == "game_2048", "identifier: a leading digit gets a prefix");
    CHECK_MESSAGE(projectIdentifier("- _ -").empty(), "identifier: a name without letters or digits has none");
    CHECK_MESSAGE((projectIdentifier("All") == "all_game" && projectIdentifier("Test") == "test_game"),
                  "identifier: names CMake reserves for targets are extended");
}

TEST_CASE("ProjectHub: recent projects") {
    const testsupport::TempDirectory rootDirectory("project-hub-recent");
    const fs::path root = rootDirectory.path();
    const fs::path existing = root / "existing" / "existing.zenith-project";
    writeText(existing, "{}");
    const fs::path listFile = root / "settings" / "recent-projects.json";
    {
        RecentProjects recent(listFile);
        CHECK_MESSAGE((static_cast<bool>(recent.load()) && recent.entries().empty()),
                      "recent: a missing list loads empty");
        recent.recordOpened(existing, "Existing", at(100));
        recent.recordOpened(root / "missing" / "missing.zenith-project", "Missing", at(200));
        // The same file named another way moves to the front instead of
        // being listed twice.
        recent.recordOpened(root / "existing" / ".." / "existing" / "existing.zenith-project", "Existing again",
                            at(300));
        CHECK_MESSAGE(recent.entries().size() == 2, "recent: one project is listed once");
        CHECK_MESSAGE((recent.entries().front().displayName == "Existing again" &&
                       recent.entries().front().lastOpened == at(300)),
                      "recent: the most recently opened project is first");
        CHECK_MESSAGE((static_cast<bool>(recent.save())), "recent: the list saves");
    }
    RecentProjects reloaded(listFile);
    CHECK_MESSAGE((static_cast<bool>(reloaded.load()) && reloaded.entries().size() == 2),
                  "recent: the list round trips");
    if (reloaded.entries().size() == 2) {
        const RecentProject& first = reloaded.entries()[0];
        const RecentProject& second = reloaded.entries()[1];
        CHECK_MESSAGE((first.projectFile == fs::absolute(existing).lexically_normal() && first.lastOpened == at(300)),
                      "recent: paths and times round trip in order");
        CHECK_MESSAGE((!recentProjectMissing(first) && recentProjectMissing(second)),
                      "recent: a project whose file is gone is flagged missing");
        CHECK_MESSAGE((reloaded.remove(second.projectFile) && reloaded.entries().size() == 1),
                      "recent: a missing project can be removed");
    }

    for (int index = 0; index < 25; ++index) {
        reloaded.recordOpened(root / ("p" + std::to_string(index)) / "p.zenith-project", "P", at(1000 + index));
    }
    CHECK_MESSAGE(reloaded.entries().size() == RecentProjects::maximumEntries,
                  "recent: the list keeps at most maximumEntries projects");
    CHECK_MESSAGE(
        (reloaded.entries().front().lastOpened == at(1024) && reloaded.entries().back().lastOpened == at(1005)),
        "recent: the oldest projects are dropped first");

    writeText(root / "bad" / "recent-projects.json", "{ not json");
    RecentProjects malformed(root / "bad" / "recent-projects.json");
    CHECK_MESSAGE((!malformed.load() && malformed.entries().empty()), "recent: a malformed list is an error");
    writeText(root / "typed" / "recent-projects.json", R"({"version": "1", "projects": []})");
    RecentProjects mistyped(root / "typed" / "recent-projects.json");
    CHECK_MESSAGE(!mistyped.load(), "recent: a version that is not a number is an error, not an exception");
}

TEST_CASE("ProjectHub: generate project") {
    const testsupport::TempDirectory rootDirectory("project-hub-generate");
    const fs::path root = rootDirectory.path();
    AssetManager assets;
    const ProjectTemplateResult created = generateProjectFromTemplate(templateRequest("Space Race", root), assets);
    CHECK_MESSAGE((static_cast<bool>(created)), "template: the C++ game project generates");
    if (!created) {
        std::cerr << created.error << '\n';
        return;
    }
    const fs::path directory = root / "space_race";
    CHECK_MESSAGE(created.projectFile == directory / "space_race.zenith-project",
                  "template: the project file is named");

    ProjectConfig project;
    CHECK_MESSAGE((static_cast<bool>(loadProjectConfig(created.projectFile, project))),
                  "template: the project file loads");
    CHECK_MESSAGE(project.displayName == "Space Race", "template: the display name is kept");
    CHECK_MESSAGE((project.assetRoot == directory / "assets" && project.sceneRoot == directory / "scenes"),
                  "template: assets/ and scenes/ are the roots");
    CHECK_MESSAGE(project.startupScene == directory / "scenes" / "main.zenith-scene",
                  "template: the startup scene is set");
    CHECK_MESSAGE(project.gameModule == directory / "modules" / "space_race.zmodule",
                  "template: the game module is modules/<identifier>.zmodule");

    AssetManager sceneAssets;
    Scene scene;
    const ProjectConfigResult loaded = loadProjectStartupScene(project, sceneAssets, scene);
    CHECK_MESSAGE((static_cast<bool>(loaded)), "template: the starter scene loads");
    for (const char* name : {"Ground", "Cube", "Sun", "Environment", "Camera"}) {
        CHECK_MESSAGE(hasEntityNamed(scene, name), (std::string("template: the starter scene has ") + name));
    }
    CHECK_MESSAGE(scene.registry().view<const CameraComponent>().size() == 1,
                  "template: the starter scene has a camera");

    const std::string rootCmake = readText(directory / "CMakeLists.txt");
    const std::string moduleCmake = readText(directory / "src" / "CMakeLists.txt");
    const std::string moduleSource = readText(directory / "src" / "GameModule.cpp");
    const std::string gameplay = readText(directory / "src" / "SpinnerGameplay.cpp");
    const std::string readme = readText(directory / "README.md");
    CHECK_MESSAGE((contains(rootCmake, "project(space_race") && contains(rootCmake, "ZENITH_ROOT")),
                  "template: the root CMakeLists names the project and ZENITH_ROOT");
    CHECK_MESSAGE(
        (contains(moduleCmake, "add_library(space_race MODULE") && contains(moduleCmake, "modules/space_race.zmodule")),
        "template: the module target builds and copies modules/<identifier>.zmodule");
    CHECK_MESSAGE(contains(moduleSource, "ZENITH_GAME_MODULE(\"space_race\""), "template: the module is named");
    CHECK_MESSAGE((contains(moduleCmake, "# zenith:sources") && contains(moduleSource, "// zenith:includes") &&
                   contains(moduleSource, "// zenith:register")),
                  "template: the code generators' markers are in place");
    CHECK_MESSAGE(!fs::exists(directory / templateSnippetDirectoryName), "template: the snippets are not copied");
    CHECK_MESSAGE((contains(gameplay, "Fields<SpinnerComponent>") && contains(gameplay, "addSystem")),
                  "template: the example component is reflected and its system registered");
    CHECK_MESSAGE(contains(readme, "# Space Race"), "template: the README names the project");
    for (const auto& entry : fs::recursive_directory_iterator(directory)) {
        if (entry.is_regular_file() && contains(readText(entry.path()), "{{")) {
            CHECK_MESSAGE(false, ("template: no token is left in " + entry.path().string()));
        }
    }
    CHECK_MESSAGE((fs::is_regular_file(directory / "assets" / "primitives" / "cube.gltf") &&
                   fs::is_regular_file(directory / "assets" / "README.md") &&
                   fs::is_regular_file(directory / ".gitignore")),
                  "template: the starter model, the assets README, and .gitignore are written");

    const nlohmann::json presets = nlohmann::json::parse(readText(directory / "CMakePresets.json"), nullptr, false);
    const bool presetValid = presets.is_object() && presets["configurePresets"].is_array() &&
                             presets["configurePresets"][0]["name"] == "zenith" &&
                             presets["configurePresets"][0]["cacheVariables"]["ZENITH_ROOT"]["value"] ==
                                 fs::path(ZENITH_ENGINE_BUILD_DIR).generic_string() &&
                             presets["buildPresets"][0]["configuration"] == ZENITH_EDITOR_CONFIGURATION;
    CHECK_MESSAGE(presetValid, "template: the zenith preset names ZENITH_ROOT and the editor's configuration");
    if (fs::is_regular_file(fs::path(ZENITH_ENGINE_BUILD_DIR) / engineDescriptionFileName)) {
        CHECK_MESSAGE(presets["configurePresets"][0].contains("generator"),
                      "template: the preset repeats the engine's generator");
    }

    const std::optional<BuildRequest> build = projectBuildRequest(created.projectFile);
    CHECK_MESSAGE((build && build->workingDirectory == directory &&
                   build->command == "cmake --preset zenith && cmake --build --preset zenith"),
                  "template: the editor builds the project's preset from its folder");
    CHECK_MESSAGE(!projectBuildRequest(root / "elsewhere" / "none.zenith-project"),
                  "template: a project without presets has no project build");

    const DeveloperEnvironment environment{.variables = {{"ZENITH_TEST_VARIABLE", "1"}},
                                           .ninja = fs::path("C:/tools/ninja.exe")};
    const std::optional<BuildRequest> fast = fastProjectBuildRequest(created.projectFile, environment);
    const fs::path fastTree = directory / ".zenith" / "build";
    const auto hasArgument = [&fast](std::string_view argument) {
        return fast && std::ranges::find(fast->prepareArguments, argument) != fast->prepareArguments.end();
    };
    CHECK_MESSAGE((fast && fast->workingDirectory == directory && fast->environment == environment.variables &&
                   fast->arguments == std::vector<std::string>{"cmake", "--build", fastTree.generic_string()} &&
                   fast->prepareUnlessExists == fastTree / "build.ninja"),
                  "template: the fast build builds .zenith/build with the captured environment, configuring it once");
    CHECK_MESSAGE((hasArgument("Ninja") && hasArgument("-DCMAKE_MAKE_PROGRAM=C:/tools/ninja.exe") &&
                   hasArgument("-DZENITH_ROOT=" + fs::path(ZENITH_ENGINE_BUILD_DIR).generic_string()) &&
                   hasArgument(std::string("-DCMAKE_BUILD_TYPE=") + ZENITH_EDITOR_CONFIGURATION)),
                  "template: the fast build configures Ninja for the preset's engine and configuration");
    CHECK_MESSAGE(!fastProjectBuildRequest(root / "elsewhere" / "none.zenith-project", environment),
                  "template: a project without presets has no fast build");
    CHECK_MESSAGE(projectIdeLocation(created.projectFile).file == directory.lexically_normal().make_preferred(),
                  "hub: Open in IDE opens the project folder");
    const ExternalEditorCommandResult ide =
        expandExternalEditorCommand(R"(code -g "{file}:{line}:{column}")", projectIdeLocation(created.projectFile));
    CHECK_MESSAGE((ide && ide.arguments.size() == 3 && contains(ide.arguments[2], "space_race:1:1")),
                  "hub: the IDE command receives the project folder");
}

TEST_CASE("ProjectHub: generate failures") {
    const testsupport::TempDirectory rootDirectory("project-hub-generate-errors");
    const fs::path root = rootDirectory.path();
    AssetManager assets;
    const fs::path occupied = root / "occupied";
    writeText(occupied / "keep.txt", "mine");
    const ProjectTemplateResult existing = generateProjectFromTemplate(templateRequest("Occupied", root), assets);
    CHECK_MESSAGE((!existing && contains(existing.error, "not empty")), "template: a non-empty folder is refused");
    CHECK_MESSAGE(readText(occupied / "keep.txt") == "mine", "template: a refused folder is left alone");

    CHECK_MESSAGE(!generateProjectFromTemplate(templateRequest("Bad \"name\"", root), assets),
                  "template: names with quotes are refused");
    CHECK_MESSAGE(!generateProjectFromTemplate(templateRequest("", root), assets),
                  "template: an empty name is refused");

    const fs::path brokenTemplate = root / "broken_template";
    writeText(brokenTemplate / "README.md", "{{PROJECT_NAME}} uses {{NOT_A_TOKEN}}\n");
    ProjectTemplateRequest broken = templateRequest("Broken", root);
    broken.templateDirectory = brokenTemplate;
    const ProjectTemplateResult failed = generateProjectFromTemplate(broken, assets);
    CHECK_MESSAGE((!failed && contains(failed.error, "NOT_A_TOKEN")), "template: an unknown token is an error");
    CHECK_MESSAGE(!fs::exists(root / "broken"), "template: a failed generation removes the folder it created");
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

TEST_CASE("ProjectHub: hub draw") {
    const testsupport::TempDirectory rootDirectory("project-hub-draw");
    const fs::path root = rootDirectory.path();
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
    CHECK_MESSAGE(!drawHub(hub, assets), "hub: drawing alone chooses nothing");

    // Rows push their project file's path as their id.
    const std::string existingKey = fs::absolute(existing).lexically_normal().string();
    HeadlessImGuiContext::activateNextFrame(
        hubItem({"##hub_columns", "##recent_projects", existingKey.c_str(), "Open"}));
    const std::optional<fs::path> opened = drawHub(hub, assets);
    CHECK_MESSAGE((opened && *opened == fs::absolute(existing).lexically_normal()),
                  "hub: Open chooses a recent project");

    HeadlessImGuiContext::activateNextFrame(
        hubItem({"##hub_columns", "##recent_projects", missing.string().c_str(), "Remove"}));
    drawHub(hub, assets);
    CHECK_MESSAGE((recent.entries().size() == 1 && recent.entries().front().displayName == "Listed"),
                  "hub: Remove drops a missing project from the list");
    RecentProjects saved(recent.file());
    CHECK_MESSAGE((saved.load() && saved.entries().size() == 1), "hub: removing saves the list");

    HeadlessImGuiContext::activateNextFrame(hubItem({"##hub_columns", "Create Project"}));
    const std::optional<fs::path> created = drawHub(hub, assets);
    CHECK_MESSAGE((created && *created == root / "created" / "my_game" / "my_game.zenith-project" &&
                   fs::is_regular_file(*created)),
                  "hub: Create Project generates the template project and chooses it");
    CHECK_MESSAGE(!hub.messageIsError(), "hub: creating reports success");
}

// Whether the HUD drew text exactly matching text this frame.
} // namespace

TEST_SUITE_END();
