// Runs the whole editor session headlessly: EditorApplication composed by an
// ApplicationSession, as zenith_editor composes it, with ImGui frames driven
// by HeadlessImGui instead of a window and renderer. Shortcuts are pressed
// through ImGui's input queue, so they travel the real keymap and operator
// path; checks observe the scene, the fixed-update gates, control requests,
// and the files the session persists.

#include "HeadlessImGui.hpp"

#include "editor/application/EditorApplication.hpp"
#include "editor/operators/Keymap.hpp"
#include "editor/project/ProjectTemplate.hpp"
#include "editor/project/RecentProjects.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "engine/application/Application.hpp"
#include "engine/scene/Scene.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {
int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

size_t entityCount(const Scene& scene) {
    return scene.registry().view<const SceneEntityIdComponent>().size();
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

ProjectConfig testProject(const fs::path& root) {
    ProjectConfig project;
    project.displayName = "Editor session test";
    project.assetRoot = ZENITH_ASSET_DIR;
    project.sceneRoot = root / "scenes";
    fs::create_directories(project.sceneRoot);
    return project;
}

// Declaration order is teardown order in reverse: the session (and the
// editor's unload, which still uses ImGui) ends before the ImGui context.
class EditorHarness {
  public:
    explicit EditorHarness(const fs::path& root, std::optional<EditorProjectHubOptions> projectHub = std::nullopt,
                           EditorPlayServices play = {})
        : EditorHarness(root, testProject(root), std::move(projectHub), std::move(play)) {}

    EditorHarness(const fs::path& root, ProjectConfig project,
                  std::optional<EditorProjectHubOptions> projectHub = std::nullopt, EditorPlayServices play = {})
        : m_application(root / "settings", ZENITH_EDITOR_FONT, std::move(project), std::move(projectHub),
                        std::move(play))
        , m_session(m_application, scene, assets, m_physics, m_audio, m_rendering, m_gameUi, logger) {}

    SceneResult start() {
        return m_session.start();
    }

    // One engine frame: update between ImGui frames, then the UI.
    ApplicationFrameResult frame() {
        m_session.update(InputState{}, 1.0f / 60.0f);
        buildFontAtlas();
        m_imgui.frame([this] { m_session.drawUi(); });
        return m_session.finishFrame();
    }

    // Presses and releases chord over two frames, then lets the session act on
    // any request the shortcut made.
    ApplicationFrameResult press(ImGuiKeyChord chord) {
        m_session.update(InputState{}, 1.0f / 60.0f);
        buildFontAtlas();
        m_imgui.press(chord, [this] { m_session.drawUi(); });
        return m_session.finishFrame();
    }

    ApplicationSession& session() {
        return m_session;
    }

    const EditorApplication& application() const {
        return m_application;
    }

    void shutdown() {
        m_session.shutdown();
    }

  private:
    // The editor loads its font into the atlas; a renderer backend rebuilds
    // the atlas before the next frame, and the harness stands in for it.
    static void buildFontAtlas() {
        ImFontAtlas& fonts = *ImGui::GetIO().Fonts;
        if (!fonts.IsBuilt()) {
            unsigned char* pixels = nullptr;
            int width = 0;
            int height = 0;
            fonts.GetTexDataAsRGBA32(&pixels, &width, &height);
        }
    }

    HeadlessImGui m_imgui;

  public:
    Scene scene;
    AssetManager assets;
    Logger logger{LoggerConfig{.echoToStderr = false}};

  private:
    PhysicsWorld m_physics;
    AudioSystem m_audio;
    RenderRuntime m_rendering;
    GameUiContext m_gameUi;
    EditorApplication m_application;
    ApplicationSession m_session;
};

void testStartupAndPersistence(const fs::path& root) {
    {
        EditorHarness editor(root);
        expect(static_cast<bool>(editor.start()), "startup: the editor session starts without a GPU");
        expect(entityCount(editor.scene) > 0, "startup: the default scene is populated");
        bool running = true;
        for (int frame = 0; frame < 5; ++frame) {
            const ApplicationFrameResult result = editor.frame();
            running = running && result.success && result.keepRunning;
        }
        expect(running, "startup: idle frames keep the editor running");
        expect(!editor.session().fixedUpdatesEnabled(), "startup: Edit mode runs no fixed updates");
        editor.shutdown();
    }
    const fs::path settings = root / "settings";
    expect(fs::is_regular_file(settings / "workspaces.json"), "persistence: unloading saves the workspaces");
    expect(fs::is_regular_file(settings / "settings.ini"), "persistence: unloading saves the editor settings");

    EditorHarness reopened(root);
    expect(static_cast<bool>(reopened.start()) && reopened.frame().keepRunning,
           "persistence: a second session starts from the saved workspaces and settings");
}

void testPlayModeRestoresTheScene(const fs::path& root) {
    EditorHarness editor(root);
    if (!editor.start()) {
        expect(false, "play: the editor session starts");
        return;
    }
    editor.frame();
    const size_t authored = entityCount(editor.scene);

    editor.press(ImGuiKey_F5);
    ApplicationSession& session = editor.session();
    expect(session.fixedUpdatesEnabled() && session.beginFixedUpdate(), "play: F5 enters Play mode");
    editor.scene.createEntity("Spawned during play");

    editor.press(ImGuiKey_F6);
    expect(!session.beginFixedUpdate(), "play: F6 pauses fixed updates");
    // The press's release frame runs the step and ends it (EditorPlayModeTests
    // counts what a step advances).
    editor.press(ImGuiKey_F7);
    expect(!session.fixedUpdatesEnabled() && session.simulationClock().paused &&
               session.simulationClock().frameStepSeconds == 0.0f,
           "play: F7 steps one frame and stays paused");
    editor.press(ImGuiKey_F6);
    expect(session.beginFixedUpdate(), "play: F6 again resumes");

    editor.press(ImGuiMod_Shift | ImGuiKey_F5);
    expect(!session.fixedUpdatesEnabled(), "play: Shift F5 returns to Edit mode");
    expect(entityCount(editor.scene) == authored && !hasEntityNamed(editor.scene, "Spawned during play"),
           "play: stopping restores the authored scene");
}

// A module system that throws during Play stops Play at the end of the frame,
// restores the edit scene, and leaves the editor running; Play starts again
// once the cause is gone.
void testGameplayFaultStopsPlay(const fs::path& root) {
    ProjectConfig project = testProject(root);
    project.gameModule = ZENITH_EDITOR_TEST_MODULE_V1;
    EditorHarness editor(root, std::move(project));
    if (!editor.start()) {
        expect(false, "fault: the editor session starts with the test module");
        return;
    }
    editor.frame();
    // The test module's count_play throws while this entity exists.
    const SceneEntityId trigger = editor.scene.id(editor.scene.createEntity("editor_test_throw"));
    const size_t authored = entityCount(editor.scene);
    ApplicationSession& session = editor.session();

    editor.press(ImGuiKey_F5);
    const ApplicationFrameResult faulted = editor.frame();
    expect(faulted.success && faulted.keepRunning, "fault: a throwing system does not end the editor");
    expect(!session.fixedUpdatesEnabled(), "fault: a throwing system stops Play");
    expect(entityCount(editor.scene) == authored && hasEntityNamed(editor.scene, "editor_test_throw"),
           "fault: stopping restores the edit scene");
    const std::vector<LogEntry> entries = editor.logger.entries();
    expect(std::ranges::any_of(entries,
                               [](const LogEntry& entry) {
                                   return entry.level == LogLevel::Error &&
                                          entry.message.find("System 'count_play' threw in Update") !=
                                              std::string::npos;
                               }),
           "fault: the throwing system is logged with its name and phase");
    bool running = true;
    for (int frame = 0; frame < 3; ++frame) {
        const ApplicationFrameResult result = editor.frame();
        running = running && result.success && result.keepRunning;
    }
    expect(running && !session.fixedUpdatesEnabled(), "fault: the editor keeps running in Edit mode");

    editor.scene.destroyEntity(editor.scene.findEntity(trigger));
    editor.press(ImGuiKey_F5);
    editor.frame();
    expect(session.fixedUpdatesEnabled(), "fault: Play runs again once the system stops throwing");
    editor.press(ImGuiMod_Shift | ImGuiKey_F5);
}

void testNewDocumentAndQuit(const fs::path& root) {
    EditorHarness editor(root);
    if (!editor.start()) {
        expect(false, "file: the editor session starts");
        return;
    }
    editor.frame();
    const size_t defaultCount = entityCount(editor.scene);
    // Not recorded in history, so the document stays clean and New does not
    // stop for the unsaved-changes dialog.
    editor.scene.createEntity("Scratch");
    const ApplicationFrameResult created = editor.press(ImGuiMod_Ctrl | ImGuiKey_N);
    expect(created.keepRunning && !hasEntityNamed(editor.scene, "Scratch") && entityCount(editor.scene) == defaultCount,
           "file: Ctrl N replaces a clean document with the default scene");

    const ApplicationFrameResult quit = editor.press(ImGuiMod_Ctrl | ImGuiKey_Q);
    expect(quit.success && !quit.keepRunning, "file: Ctrl Q on a clean document quits");
}

void testHostQuitRequest(const fs::path& root) {
    EditorHarness editor(root);
    if (!editor.start()) {
        expect(false, "quit: the editor session starts");
        return;
    }
    editor.frame();
    editor.press(ImGuiKey_F5);
    // Closing the window while playing stops Play mode and then quits.
    editor.session().requestQuit();
    expect(!editor.session().fixedUpdatesEnabled(), "quit: a host quit request leaves Play mode");
    const ApplicationFrameResult result = editor.frame();
    expect(result.success && !result.keepRunning, "quit: a host quit request on a clean document quits");
}

// What the fake game process saw and does; shared with the test.
struct FakeGame {
    std::vector<std::string> arguments;
    bool running{false};
    bool cancelled{false};
    std::optional<int> exitCode;
    std::vector<std::string> pendingLines;
};

class FakeGameProcess final : public GameProcess {
  public:
    explicit FakeGameProcess(std::shared_ptr<FakeGame> game)
        : m_game(std::move(game)) {}

    ChildProcessResult start(const ChildProcessOptions& options) override {
        m_game->arguments = options.command.arguments;
        m_game->running = true;
        m_game->cancelled = false;
        m_game->exitCode.reset();
        return {.success = true};
    }

    bool running() const override {
        return m_game->running;
    }

    std::optional<int> exitCode() const override {
        return m_game->exitCode;
    }

    std::vector<std::string> takeLines() override {
        return std::exchange(m_game->pendingLines, {});
    }

    void cancel() override {
        m_game->cancelled = true;
        m_game->running = false;
    }

  private:
    std::shared_ptr<FakeGame> m_game;
};

// The play targets through the real keymap: F5 plays in the active viewport
// (whose game view holds game input, so the editor captures the mouse
// persistently), F9 and F10 (bound in keymap.json) play in a floating
// viewport and standalone with a fake game process.
void testPlayTargets(const fs::path& root) {
    fs::create_directories(root / "settings");
    const fs::path executable = root / "zenith-fake.exe";
    std::ofstream(executable) << "fake";
    {
        const auto binding = [](ImGuiKeyChord chord, const char* target) {
            return nlohmann::json{{"operator", "play.start"},
                                  {"region", keymapRegionName(KeymapRegion::Global)},
                                  {"chord", formatKeyChord(chord)},
                                  {"properties", {{"target", target}}}};
        };
        std::ofstream(root / "settings" / "keymap.json")
            << nlohmann::json{{"version", 1},
                              {"removed", nlohmann::json::array()},
                              {"added", {binding(ImGuiKey_F9, "floating"), binding(ImGuiKey_F10, "standalone")}}}
                   .dump();
    }
    const auto game = std::make_shared<FakeGame>();
    EditorHarness editor(root, std::nullopt,
                         EditorPlayServices{.gameExecutable = executable,
                                            .makeGameProcess = [game]() -> std::unique_ptr<GameProcess> {
                                                return std::make_unique<FakeGameProcess>(game);
                                            },
                                            .temporaryDirectory = root / "play"});
    if (!editor.start()) {
        expect(false, "targets: the editor session starts");
        return;
    }
    ApplicationSession& session = editor.session();
    editor.frame();
    editor.frame();
    const size_t authored = entityCount(editor.scene);
    expect(editor.application().mouseCapturePolicy() == MouseCapturePolicy::WhileRightMouseDown,
           "targets: editing flies the camera with the right mouse button only");

    editor.press(ImGuiKey_F5);
    expect(session.fixedUpdatesEnabled(), "targets: F5 plays in the active viewport");
    expect(editor.application().mouseCapturePolicy() == MouseCapturePolicy::WhileRightMouseDown,
           "targets: away from the game view a click stays the editor's");
    if (const ImGuiWindow* viewport = HeadlessImGuiContext::findWindow("##viewport.1")) {
        ImGui::GetIO().AddMousePosEvent(viewport->Pos.x + viewport->Size.x * 0.5f,
                                        viewport->Pos.y + viewport->Size.y * 0.5f);
    }
    editor.frame();
    expect(editor.application().mouseCapturePolicy() == MouseCapturePolicy::Persistent,
           "targets: over the focused game view a click captures the mouse until Esc");
    editor.press(ImGuiKey_Escape);
    expect(editor.application().mouseCapturePolicy() == MouseCapturePolicy::WhileRightMouseDown,
           "targets: Esc gives game input back to the editor");
    editor.press(ImGuiMod_Shift | ImGuiKey_F5);
    expect(!session.fixedUpdatesEnabled() && entityCount(editor.scene) == authored,
           "targets: Stop leaves the active viewport target");

    const auto floatingWindow = [] {
        const ImGuiWindow* window = HeadlessImGuiContext::findWindow("##viewport.2");
        return window != nullptr && window->WasActive ? window : nullptr;
    };
    editor.press(ImGuiKey_F9);
    editor.frame();
    const ImGuiWindow* floating = floatingWindow();
    expect(session.fixedUpdatesEnabled() && floating != nullptr && floating->DockNode == nullptr,
           "targets: F9 plays in a new floating viewport");
    if (floating != nullptr) {
        ImGui::GetIO().AddMousePosEvent(floating->Pos.x + floating->Size.x * 0.5f,
                                        floating->Pos.y + floating->Size.y * 0.5f);
    }
    editor.frame();
    expect(editor.application().mouseCapturePolicy() == MouseCapturePolicy::Persistent,
           "targets: the floating viewport holds game input");
    editor.press(ImGuiMod_Shift | ImGuiKey_F5);
    editor.frame();
    expect(!session.fixedUpdatesEnabled() && floatingWindow() == nullptr, "targets: Stop closes the floating viewport");

    editor.press(ImGuiKey_F10);
    expect(game->running && !session.fixedUpdatesEnabled(),
           "targets: F10 launches the standalone game and leaves the editor's scene alone");
    const auto sceneArgument = std::ranges::find(game->arguments, "--scene");
    const bool hasScene = sceneArgument != game->arguments.end() && sceneArgument + 1 != game->arguments.end();
    const fs::path sceneFile = hasScene ? fs::path(*(sceneArgument + 1)) : fs::path();
    expect(!game->arguments.empty() && game->arguments.front() == executable.string() && hasScene &&
               fs::is_regular_file(sceneFile),
           "targets: the game gets its executable and a temporary copy of the scene");
    game->pendingLines = {"hello from the game"};
    editor.frame();
    bool streamed = false;
    for (const LogEntry& entry : editor.logger.entries()) {
        streamed = streamed || (entry.category == "game" && entry.message == "hello from the game");
    }
    expect(streamed, "targets: the game's output reaches the log");
    editor.press(ImGuiKey_F5);
    expect(!session.fixedUpdatesEnabled(), "targets: Play waits while the standalone game runs");
    editor.press(ImGuiMod_Shift | ImGuiKey_F5);
    editor.frame();
    expect(game->cancelled && !fs::exists(sceneFile), "targets: Stop kills the game and removes its scene");

    editor.press(ImGuiKey_F10);
    expect(game->running, "targets: the standalone game starts again");
    game->running = false;
    game->exitCode = 0;
    editor.frame();
    game->arguments.clear();
    // F5 reuses the last target, which only starts once the old game left Play.
    editor.press(ImGuiKey_F5);
    expect(game->running && !game->arguments.empty(),
           "targets: the game exiting leaves Play, so F5 starts the standalone game again");
    editor.press(ImGuiMod_Shift | ImGuiKey_F5);
    editor.frame();
}

// The id of an item in the Project Hub's window, reached through path's ids.
ImGuiID projectHubItem(std::initializer_list<const char*> path) {
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

// Starts in the hub, opens a template project from the recent list, and goes
// back to the hub through File > Project Hub (run from operator search).
void testProjectHubSwitchesProjects(const fs::path& root) {
    AssetManager generatorAssets;
    const ProjectTemplateResult generated =
        generateProjectFromTemplate({.displayName = "Hub Game",
                                     .parentDirectory = root / "projects",
                                     .templateDirectory = fs::path(ZENITH_TEMPLATE_DIR) / cppGameTemplateName,
                                     .engineRoot = root / "engine",
                                     .engineAssetRoot = ZENITH_ASSET_DIR},
                                    generatorAssets);
    if (!generated) {
        expect(false, "hub: the template project generates: " + generated.error);
        return;
    }
    {
        RecentProjects recent(root / "settings" / "recent-projects.json");
        recent.recordOpened(generated.projectFile, "Hub Game", std::chrono::system_clock::time_point{});
        expect(static_cast<bool>(recent.save()), "hub: the recent list is prepared");
    }

    EditorHarness editor(
        root, EditorProjectHubOptions{.hub = {.defaultProjectLocation = root / "projects"}, .startInHub = true});
    if (!editor.start()) {
        expect(false, "hub: the editor starts in the Project Hub");
        return;
    }
    const ApplicationFrameResult hubFrame = editor.frame();
    expect(hubFrame.keepRunning && entityCount(editor.scene) == 0, "hub: the hub shows no scene");

    const std::string projectKey = fs::absolute(generated.projectFile).lexically_normal().string();
    HeadlessImGuiContext::activateNextFrame(
        projectHubItem({"##hub_columns", "##recent_projects", projectKey.c_str(), "Open"}));
    const ApplicationFrameResult opened = editor.frame();
    expect(opened.success && opened.keepRunning, "hub: opening a recent project keeps the editor running");
    expect(hasEntityNamed(editor.scene, "Ground") && hasEntityNamed(editor.scene, "Camera"),
           "hub: the chosen project's session opens its startup scene");
    expect(editor.application().project().displayName == "Hub Game", "hub: the chosen project is current");
    const std::span<const RecentProject> recent = editor.application().recentProjects().entries();
    expect(!recent.empty() && recent.front().lastOpened > std::chrono::system_clock::time_point{},
           "hub: opening a project records when it was opened");
    editor.frame();

    // F3, then the operator's name and Enter.
    editor.press(ImGuiKey_F3);
    HeadlessImGuiContext::typeNextFrame("Project Hub");
    editor.frame();
    const ApplicationFrameResult left = editor.press(ImGuiKey_Enter);
    expect(left.success && left.keepRunning && entityCount(editor.scene) == 0,
           "hub: File > Project Hub closes the clean project and shows the hub");
    expect(fs::is_regular_file(root / "settings" / "workspaces.json"),
           "hub: leaving the project unloads its session, which saves the workspaces");
    expect(editor.frame().keepRunning, "hub: the hub keeps running after a project closed");
}

void testProjectHubUsesThePreferredLook(const fs::path& root) {
    fs::create_directories(root / "settings");
    {
        std::ofstream settings(root / "settings" / "settings.ini");
        settings << "theme.accent=violet\nui.scale=2\n";
    }
    EditorHarness editor(
        root, EditorProjectHubOptions{.hub = {.defaultProjectLocation = root / "projects"}, .startInHub = true});
    if (!editor.start()) {
        expect(false, "hub look: the editor starts in the Project Hub");
        return;
    }
    const ApplicationFrameResult frame = editor.frame();
    expect(frame.success && frame.keepRunning, "hub look: the hub draws");
    expect(editorArtPixel() == 2.0f, "hub look: the hub uses the UI scale from settings.ini");
    expect(editorAccent() == themeAccentInfo(ThemeAccent::Violet).color.withAlpha(1.0f),
           "hub look: the hub uses the accent from settings.ini");
}
} // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "zenith_editor_session_tests";
    std::error_code error;
    fs::remove_all(root, error);
    fs::create_directories(root);

    testStartupAndPersistence(root / "persistence");
    testPlayModeRestoresTheScene(root / "play");
    testGameplayFaultStopsPlay(root / "fault");
    testNewDocumentAndQuit(root / "file");
    testHostQuitRequest(root / "quit");
    testPlayTargets(root / "targets");
    testProjectHubSwitchesProjects(root / "hub");
    testProjectHubUsesThePreferredLook(root / "hub_look");

    fs::remove_all(root, error);
    if (failures != 0) {
        std::cerr << failures << " editor session test(s) failed\n";
        return 1;
    }
    std::cout << "Editor session tests passed\n";
    return 0;
}
