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

#include "support/Doctest.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using testsupport::TempDirectory;
using testsupport::writeText;

namespace {

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

} // namespace

TEST_SUITE_BEGIN("EditorSession");

TEST_CASE("EditorSession: startup and persistence") {
    const TempDirectory directory("editor-session-persistence");
    const fs::path& root = directory.path();
    {
        EditorHarness editor(root);
        CHECK_MESSAGE(static_cast<bool>(editor.start()), "startup: the editor session starts without a GPU");
        CHECK_MESSAGE(entityCount(editor.scene) > 0, "startup: the default scene is populated");
        bool running = true;
        for (int frame = 0; frame < 5; ++frame) {
            const ApplicationFrameResult result = editor.frame();
            running = running && result.success && result.keepRunning;
        }
        CHECK_MESSAGE(running, "startup: idle frames keep the editor running");
        CHECK_MESSAGE(!editor.session().fixedUpdatesEnabled(), "startup: Edit mode runs no fixed updates");
        editor.shutdown();
    }
    const fs::path settings = root / "settings";
    CHECK_MESSAGE(fs::is_regular_file(settings / "workspaces.json"), "persistence: unloading saves the workspaces");
    CHECK_MESSAGE(fs::is_regular_file(settings / "settings.ini"), "persistence: unloading saves the editor settings");

    EditorHarness reopened(root);
    CHECK_MESSAGE((static_cast<bool>(reopened.start()) && reopened.frame().keepRunning),
                  "persistence: a second session starts from the saved workspaces and settings");
}

TEST_CASE("EditorSession: play mode restores the scene") {
    const TempDirectory directory("editor-session-play");
    const fs::path& root = directory.path();
    EditorHarness editor(root);
    if (!editor.start()) {
        CHECK_MESSAGE(false, "play: the editor session starts");
        return;
    }
    editor.frame();
    const size_t authored = entityCount(editor.scene);

    editor.press(ImGuiKey_F5);
    ApplicationSession& session = editor.session();
    CHECK_MESSAGE((session.fixedUpdatesEnabled() && session.beginFixedUpdate()), "play: F5 enters Play mode");
    editor.scene.createEntity("Spawned during play");

    editor.press(ImGuiKey_F6);
    CHECK_MESSAGE(!session.beginFixedUpdate(), "play: F6 pauses fixed updates");
    // The press's release frame runs the step and ends it (EditorPlayModeTests
    // counts what a step advances).
    editor.press(ImGuiKey_F7);
    CHECK_MESSAGE((!session.fixedUpdatesEnabled() && session.simulationClock().paused &&
                   session.simulationClock().frameStepSeconds == 0.0f),
                  "play: F7 steps one frame and stays paused");
    editor.press(ImGuiKey_F6);
    CHECK_MESSAGE(session.beginFixedUpdate(), "play: F6 again resumes");

    editor.press(ImGuiMod_Shift | ImGuiKey_F5);
    CHECK_MESSAGE(!session.fixedUpdatesEnabled(), "play: Shift F5 returns to Edit mode");
    CHECK_MESSAGE((entityCount(editor.scene) == authored && !hasEntityNamed(editor.scene, "Spawned during play")),
                  "play: stopping restores the authored scene");
}

// A module system that throws during Play stops Play at the end of the frame,
// restores the edit scene, and leaves the editor running; Play starts again
// once the cause is gone.
TEST_CASE("EditorSession: gameplay fault stops play") {
    const TempDirectory directory("editor-session-fault");
    const fs::path& root = directory.path();
    ProjectConfig project = testProject(root);
    project.gameModule = ZENITH_EDITOR_TEST_MODULE_V1;
    EditorHarness editor(root, std::move(project));
    if (!editor.start()) {
        CHECK_MESSAGE(false, "fault: the editor session starts with the test module");
        return;
    }
    editor.frame();
    // The test module's count_play throws while this entity exists.
    const SceneEntityId trigger = editor.scene.id(editor.scene.createEntity("editor_test_throw"));
    const size_t authored = entityCount(editor.scene);
    ApplicationSession& session = editor.session();

    editor.press(ImGuiKey_F5);
    const ApplicationFrameResult faulted = editor.frame();
    CHECK_MESSAGE((faulted.success && faulted.keepRunning), "fault: a throwing system does not end the editor");
    CHECK_MESSAGE(!session.fixedUpdatesEnabled(), "fault: a throwing system stops Play");
    CHECK_MESSAGE((entityCount(editor.scene) == authored && hasEntityNamed(editor.scene, "editor_test_throw")),
                  "fault: stopping restores the edit scene");
    const std::vector<LogEntry> entries = editor.logger.entries();
    CHECK_MESSAGE(std::ranges::any_of(entries,
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
    CHECK_MESSAGE((running && !session.fixedUpdatesEnabled()), "fault: the editor keeps running in Edit mode");

    editor.scene.destroyEntity(editor.scene.findEntity(trigger));
    editor.press(ImGuiKey_F5);
    editor.frame();
    CHECK_MESSAGE(session.fixedUpdatesEnabled(), "fault: Play runs again once the system stops throwing");
    editor.press(ImGuiMod_Shift | ImGuiKey_F5);
}

TEST_CASE("EditorSession: new document and quit") {
    const TempDirectory directory("editor-session-file");
    const fs::path& root = directory.path();
    EditorHarness editor(root);
    if (!editor.start()) {
        CHECK_MESSAGE(false, "file: the editor session starts");
        return;
    }
    editor.frame();
    const size_t defaultCount = entityCount(editor.scene);
    // Not recorded in history, so the document stays clean and New does not
    // stop for the unsaved-changes dialog.
    editor.scene.createEntity("Scratch");
    const ApplicationFrameResult created = editor.press(ImGuiMod_Ctrl | ImGuiKey_N);
    CHECK_MESSAGE(
        (created.keepRunning && !hasEntityNamed(editor.scene, "Scratch") && entityCount(editor.scene) == defaultCount),
        "file: Ctrl N replaces a clean document with the default scene");

    const ApplicationFrameResult quit = editor.press(ImGuiMod_Ctrl | ImGuiKey_Q);
    CHECK_MESSAGE((quit.success && !quit.keepRunning), "file: Ctrl Q on a clean document quits");
}

TEST_CASE("EditorSession: host quit request") {
    const TempDirectory directory("editor-session-quit");
    const fs::path& root = directory.path();
    EditorHarness editor(root);
    if (!editor.start()) {
        CHECK_MESSAGE(false, "quit: the editor session starts");
        return;
    }
    editor.frame();
    editor.press(ImGuiKey_F5);
    // Closing the window while playing stops Play mode and then quits.
    editor.session().requestQuit();
    CHECK_MESSAGE(!editor.session().fixedUpdatesEnabled(), "quit: a host quit request leaves Play mode");
    const ApplicationFrameResult result = editor.frame();
    CHECK_MESSAGE((result.success && !result.keepRunning), "quit: a host quit request on a clean document quits");
}

namespace {

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

} // namespace

// The play targets through the real keymap: F5 plays in the active viewport
// (whose game view holds game input, so the editor captures the mouse
// persistently), F9 and F10 (bound in keymap.json) play in a floating
// viewport and standalone with a fake game process.
TEST_CASE("EditorSession: play targets") {
    const TempDirectory directory("editor-session-targets");
    const fs::path& root = directory.path();
    const fs::path executable = root / "zenith-fake.exe";
    writeText(executable, "fake");
    {
        const auto binding = [](ImGuiKeyChord chord, const char* target) {
            return nlohmann::json{{"operator", "play.start"},
                                  {"region", keymapRegionName(KeymapRegion::Global)},
                                  {"chord", formatKeyChord(chord)},
                                  {"properties", {{"target", target}}}};
        };
        writeText(root / "settings" / "keymap.json",
                  nlohmann::json{{"version", 1},
                                 {"removed", nlohmann::json::array()},
                                 {"added", {binding(ImGuiKey_F9, "floating"), binding(ImGuiKey_F10, "standalone")}}}
                      .dump());
    }
    const auto game = std::make_shared<FakeGame>();
    EditorHarness editor(root, std::nullopt,
                         EditorPlayServices{.gameExecutable = executable,
                                            .makeGameProcess = [game]() -> std::unique_ptr<GameProcess> {
                                                return std::make_unique<FakeGameProcess>(game);
                                            },
                                            .temporaryDirectory = root / "play"});
    if (!editor.start()) {
        CHECK_MESSAGE(false, "targets: the editor session starts");
        return;
    }
    ApplicationSession& session = editor.session();
    editor.frame();
    editor.frame();
    const size_t authored = entityCount(editor.scene);
    CHECK_MESSAGE(editor.application().mouseCapturePolicy() == MouseCapturePolicy::WhileRightMouseDown,
                  "targets: editing flies the camera with the right mouse button only");

    editor.press(ImGuiKey_F5);
    CHECK_MESSAGE(session.fixedUpdatesEnabled(), "targets: F5 plays in the active viewport");
    CHECK_MESSAGE(editor.application().mouseCapturePolicy() == MouseCapturePolicy::WhileRightMouseDown,
                  "targets: away from the game view a click stays the editor's");
    if (const ImGuiWindow* viewport = HeadlessImGuiContext::findWindow("##viewport.1")) {
        ImGui::GetIO().AddMousePosEvent(viewport->Pos.x + viewport->Size.x * 0.5f,
                                        viewport->Pos.y + viewport->Size.y * 0.5f);
    }
    editor.frame();
    CHECK_MESSAGE(editor.application().mouseCapturePolicy() == MouseCapturePolicy::Persistent,
                  "targets: over the focused game view a click captures the mouse until Esc");
    editor.press(ImGuiKey_Escape);
    CHECK_MESSAGE(editor.application().mouseCapturePolicy() == MouseCapturePolicy::WhileRightMouseDown,
                  "targets: Esc gives game input back to the editor");
    editor.press(ImGuiMod_Shift | ImGuiKey_F5);
    CHECK_MESSAGE((!session.fixedUpdatesEnabled() && entityCount(editor.scene) == authored),
                  "targets: Stop leaves the active viewport target");

    const auto floatingWindow = [] {
        const ImGuiWindow* window = HeadlessImGuiContext::findWindow("##viewport.2");
        return window != nullptr && window->WasActive ? window : nullptr;
    };
    editor.press(ImGuiKey_F9);
    editor.frame();
    const ImGuiWindow* floating = floatingWindow();
    CHECK_MESSAGE((session.fixedUpdatesEnabled() && floating != nullptr && floating->DockNode == nullptr),
                  "targets: F9 plays in a new floating viewport");
    if (floating != nullptr) {
        ImGui::GetIO().AddMousePosEvent(floating->Pos.x + floating->Size.x * 0.5f,
                                        floating->Pos.y + floating->Size.y * 0.5f);
    }
    editor.frame();
    CHECK_MESSAGE(editor.application().mouseCapturePolicy() == MouseCapturePolicy::Persistent,
                  "targets: the floating viewport holds game input");
    editor.press(ImGuiMod_Shift | ImGuiKey_F5);
    editor.frame();
    CHECK_MESSAGE((!session.fixedUpdatesEnabled() && floatingWindow() == nullptr),
                  "targets: Stop closes the floating viewport");

    editor.press(ImGuiKey_F10);
    CHECK_MESSAGE((game->running && !session.fixedUpdatesEnabled()),
                  "targets: F10 launches the standalone game and leaves the editor's scene alone");
    const auto sceneArgument = std::ranges::find(game->arguments, "--scene");
    const bool hasScene = sceneArgument != game->arguments.end() && sceneArgument + 1 != game->arguments.end();
    const fs::path sceneFile = hasScene ? fs::path(*(sceneArgument + 1)) : fs::path();
    CHECK_MESSAGE((!game->arguments.empty() && game->arguments.front() == executable.string() && hasScene &&
                   fs::is_regular_file(sceneFile)),
                  "targets: the game gets its executable and a temporary copy of the scene");
    game->pendingLines = {"hello from the game"};
    editor.frame();
    bool streamed = false;
    for (const LogEntry& entry : editor.logger.entries()) {
        streamed = streamed || (entry.category == "game" && entry.message == "hello from the game");
    }
    CHECK_MESSAGE(streamed, "targets: the game's output reaches the log");
    editor.press(ImGuiKey_F5);
    CHECK_MESSAGE(!session.fixedUpdatesEnabled(), "targets: Play waits while the standalone game runs");
    editor.press(ImGuiMod_Shift | ImGuiKey_F5);
    editor.frame();
    CHECK_MESSAGE((game->cancelled && !fs::exists(sceneFile)), "targets: Stop kills the game and removes its scene");

    editor.press(ImGuiKey_F10);
    CHECK_MESSAGE(game->running, "targets: the standalone game starts again");
    game->running = false;
    game->exitCode = 0;
    editor.frame();
    game->arguments.clear();
    // F5 reuses the last target, which only starts once the old game left Play.
    editor.press(ImGuiKey_F5);
    CHECK_MESSAGE((game->running && !game->arguments.empty()),
                  "targets: the game exiting leaves Play, so F5 starts the standalone game again");
    editor.press(ImGuiMod_Shift | ImGuiKey_F5);
    editor.frame();
}

namespace {

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

} // namespace

// Starts in the hub, opens a template project from the recent list, and goes
// back to the hub through File > Project Hub (run from operator search).
TEST_CASE("EditorSession: project hub switches projects") {
    const TempDirectory directory("editor-session-hub");
    const fs::path& root = directory.path();
    AssetManager generatorAssets;
    const ProjectTemplateResult generated =
        generateProjectFromTemplate({.displayName = "Hub Game",
                                     .parentDirectory = root / "projects",
                                     .templateDirectory = fs::path(ZENITH_TEMPLATE_DIR) / cppGameTemplateName,
                                     .engineRoot = root / "engine",
                                     .engineAssetRoot = ZENITH_ASSET_DIR},
                                    generatorAssets);
    if (!generated) {
        CHECK_MESSAGE(false, ("hub: the template project generates: " + generated.error));
        return;
    }
    {
        RecentProjects recent(root / "settings" / "recent-projects.json");
        recent.recordOpened(generated.projectFile, "Hub Game", std::chrono::system_clock::time_point{});
        CHECK_MESSAGE(static_cast<bool>(recent.save()), "hub: the recent list is prepared");
    }

    EditorHarness editor(
        root, EditorProjectHubOptions{.hub = {.defaultProjectLocation = root / "projects"}, .startInHub = true});
    if (!editor.start()) {
        CHECK_MESSAGE(false, "hub: the editor starts in the Project Hub");
        return;
    }
    const ApplicationFrameResult hubFrame = editor.frame();
    CHECK_MESSAGE((hubFrame.keepRunning && entityCount(editor.scene) == 0), "hub: the hub shows no scene");

    const std::string projectKey = fs::absolute(generated.projectFile).lexically_normal().string();
    HeadlessImGuiContext::activateNextFrame(
        projectHubItem({"##hub_columns", "##recent_projects", projectKey.c_str(), "Open"}));
    const ApplicationFrameResult opened = editor.frame();
    CHECK_MESSAGE((opened.success && opened.keepRunning), "hub: opening a recent project keeps the editor running");
    CHECK_MESSAGE((hasEntityNamed(editor.scene, "Ground") && hasEntityNamed(editor.scene, "Camera")),
                  "hub: the chosen project's session opens its startup scene");
    CHECK_MESSAGE(editor.application().project().displayName == "Hub Game", "hub: the chosen project is current");
    const std::span<const RecentProject> recent = editor.application().recentProjects().entries();
    CHECK_MESSAGE((!recent.empty() && recent.front().lastOpened > std::chrono::system_clock::time_point{}),
                  "hub: opening a project records when it was opened");
    editor.frame();

    // F3, then the operator's name and Enter.
    editor.press(ImGuiKey_F3);
    HeadlessImGuiContext::typeNextFrame("Project Hub");
    editor.frame();
    const ApplicationFrameResult left = editor.press(ImGuiKey_Enter);
    CHECK_MESSAGE((left.success && left.keepRunning && entityCount(editor.scene) == 0),
                  "hub: File > Project Hub closes the clean project and shows the hub");
    CHECK_MESSAGE(fs::is_regular_file(root / "settings" / "workspaces.json"),
                  "hub: leaving the project unloads its session, which saves the workspaces");
    CHECK_MESSAGE(editor.frame().keepRunning, "hub: the hub keeps running after a project closed");
}

TEST_CASE("EditorSession: project hub uses the preferred look") {
    const TempDirectory directory("editor-session-hub-look");
    const fs::path& root = directory.path();
    writeText(root / "settings" / "settings.ini", "theme.accent=violet\nui.scale=2\n");
    EditorHarness editor(
        root, EditorProjectHubOptions{.hub = {.defaultProjectLocation = root / "projects"}, .startInHub = true});
    if (!editor.start()) {
        CHECK_MESSAGE(false, "hub look: the editor starts in the Project Hub");
        return;
    }
    const ApplicationFrameResult frame = editor.frame();
    CHECK_MESSAGE((frame.success && frame.keepRunning), "hub look: the hub draws");
    CHECK_MESSAGE(editorArtPixel() == 2.0f, "hub look: the hub uses the UI scale from settings.ini");
    CHECK_MESSAGE(editorAccent() == themeAccentInfo(ThemeAccent::Violet).color.withAlpha(1.0f),
                  "hub look: the hub uses the accent from settings.ini");
}

TEST_SUITE_END();
