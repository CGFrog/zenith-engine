// Play targets without a window, GPU, or game process: target keys, the game
// camera, a viewport's game view (lens, focus, banner, the operators it
// invokes), the mouse capture policy, the mode tint, and the standalone game
// with a fake process (temporary scene, arguments, streamed output, Stop,
// and exit).

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/application/PlayTargets.hpp"
#include "editor/document/CommandHistory.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/interaction/EditorCamera.hpp"
#include "editor/menus/EditorMenus.hpp"
#include "editor/operators/OperatorRegistry.hpp"
#include "editor/operators/OperatorRunner.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/panels/ViewportPanel.hpp"
#include "editor/theme/ThemeChrome.hpp"
#include "engine/application/Application.hpp"
#include "engine/diagnostics/Logger.hpp"

#include <imgui.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
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

void testTargetsAndPolicies() {
    expect(playTargets().size() == 3 && playTargets().front().target == PlayTarget::ActiveViewport,
           "the active viewport is the first (default) target");
    for (const PlayTargetInfo& info : playTargets()) {
        expect(parsePlayTarget(info.key) == info.target && playTargetInfo(info.target).key == info.key &&
                   !info.label.empty(),
               "every target parses from its key");
    }
    expect(parsePlayTarget("viewport") == PlayTarget::ActiveViewport &&
               parsePlayTarget("floating") == PlayTarget::FloatingViewport &&
               parsePlayTarget("standalone") == PlayTarget::Standalone && !parsePlayTarget("window"),
           "target keys are viewport, floating, and standalone");

    expect(gameViewBanner(false, true, true) == GameViewBanner::None &&
               gameViewBanner(true, false, false) == GameViewBanner::ClickToFocus &&
               gameViewBanner(true, true, false) == GameViewBanner::ClickToCapture &&
               gameViewBanner(true, true, true) == GameViewBanner::EscReleasesCursor,
           "the banner follows focus and capture");
    expect(gameViewBannerText(GameViewBanner::EscReleasesCursor).find("Esc") != std::string_view::npos &&
               gameViewBannerText(GameViewBanner::None).empty(),
           "the capture banner names Esc");
    expect(playMouseCapturePolicy(true) == MouseCapturePolicy::Persistent &&
               playMouseCapturePolicy(false) == MouseCapturePolicy::WhileRightMouseDown,
           "only a focused game view captures the mouse persistently");

    expect(playModeTint(EditorMode::Edit, false) == ThemeModeTint::None &&
               playModeTint(EditorMode::Play, false) == ThemeModeTint::Play &&
               playModeTint(EditorMode::Simulate, true) == ThemeModeTint::Simulate &&
               playModeTint(EditorMode::Paused, false) == ThemeModeTint::Play &&
               playModeTint(EditorMode::Paused, true) == ThemeModeTint::Simulate,
           "the tint follows Play and Simulate, paused or not");
    const std::optional<ThemeColor> play = themeModeTintColor(ThemeModeTint::Play);
    const std::optional<ThemeColor> simulate = themeModeTintColor(ThemeModeTint::Simulate);
    expect(!themeModeTintColor(ThemeModeTint::None) && play && simulate && *play != *simulate && play->a > 0.0f &&
               simulate->a > 0.0f,
           "Play and Simulate have distinct visible tints");
    ThemeChrome chrome;
    expect(chrome.modeTint() == ThemeModeTint::None, "the chrome starts untinted");
    chrome.setModeTint(ThemeModeTint::Simulate);
    expect(chrome.modeTint() == ThemeModeTint::Simulate, "the chrome keeps the tint it is given");
}

Entity addCamera(Scene& scene, const char* name, glm::vec3 position, bool primary) {
    const Entity camera = scene.createEntity(name);
    scene.registry().emplace<CameraComponent>(camera).primary = primary;
    scene.registry().get<TransformComponent>(camera).local = glm::translate(glm::mat4(1.0f), position);
    scene.markTransformDirty(camera);
    scene.updateWorldTransforms();
    return camera;
}

void testGameCamera() {
    Scene scene;
    expect(gameCamera(scene) == entt::null, "a scene without cameras has no game camera");
    addCamera(scene, "Spare", {1.0f, 0.0f, 0.0f}, false);
    expect(gameCamera(scene) == entt::null, "a camera that is not primary is not the game camera");
    const Entity primary = addCamera(scene, "Player", {0.0f, 2.0f, 0.0f}, true);
    expect(gameCamera(scene) == primary, "the primary camera is the game camera");
}

// Records what the game view asks the session for.
class GameViewActions final : public EditorSessionActions {
  public:
    explicit GameViewActions(ViewportPanel*& gameView)
        : m_gameView(gameView) {}

    std::vector<std::string> invoked;

    OperatorResult invokeOperator(ApplicationContext&, std::string_view operatorId, std::string_view,
                                  KeymapRegion) override {
        invoked.emplace_back(operatorId);
        // As the session's play.focus_game_view and play.release_game_input.
        if (m_gameView != nullptr && operatorId == "play.focus_game_view") {
            m_gameView->setGameInputFocus(true);
        } else if (m_gameView != nullptr && operatorId == "play.release_game_input") {
            m_gameView->setGameInputFocus(false);
        }
        return OperatorResult::finished();
    }

    bool operatorAvailable(ApplicationContext&, std::string_view) override {
        return true;
    }

    std::string operatorShortcut(std::string_view, std::string_view) const override {
        return {};
    }

    void requestOpenScene(ApplicationContext&, std::filesystem::path) override {}

    void projectFilesMoved(ApplicationContext&, std::span<const ProjectFileMove>) override {}

    void projectFilesRemoved(ApplicationContext&, std::span<const std::filesystem::path>) override {}

    void handleObjectKeys(ApplicationContext&, KeymapRegion) override {}

    bool modalTransformActive() const override {
        return false;
    }

    void updateModalTransform(ApplicationContext&, ViewportId) override {}

  private:
    ViewportPanel*& m_gameView;
};

struct GameViewFixture {
    Scene scene;
    AssetManager assets;
    PhysicsWorld physics;
    AudioSystem audio;
    ApplicationControl control;
    RenderRuntime rendering;
    GameUiContext gameUi;
    Logger logger{LoggerConfig{.echoToStderr = false}};
    ApplicationContext app{scene, assets, physics, audio, control, rendering, gameUi, logger};
    EditorDocument document{fs::temp_directory_path()};
    CommandHistory history;
    EditorDiagnostics diagnostics;
    Selection selection;
    ViewportPanel* gameView{nullptr};
    GameViewActions actions{gameView};
    EditorMenus menus;
    ViewportRegistry viewports;
    std::vector<std::unique_ptr<ViewportPanel>> panels;

    GameViewFixture() {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = {1280.0f, 720.0f};
        io.IniFilename = nullptr;
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        panels.push_back(std::make_unique<ViewportPanel>(viewports));
        panels.push_back(std::make_unique<ViewportPanel>(viewports));
    }

    ~GameViewFixture() {
        panels.clear();
        ImGui::DestroyContext();
    }

    GameViewFixture(const GameViewFixture&) = delete;
    GameViewFixture& operator=(const GameViewFixture&) = delete;

    ViewportPanel& left() {
        return *panels[0];
    }

    ViewportPanel& right() {
        return *panels[1];
    }

    // One session frame with both viewports side by side, in Play mode.
    void frame(ImVec2 mouse, const InputState& input = {}) {
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        io.AddMousePosEvent(mouse.x, mouse.y);
        viewports.beginFrame();
        ImGui::NewFrame();
        for (size_t index = 0; index < panels.size(); ++index) {
            ViewportPanel& panel = *panels[index];
            ImGui::SetNextWindowPos({640.0f * static_cast<float>(index), 0.0f});
            ImGui::SetNextWindowSize({640.0f, 720.0f});
            const std::string name = "Viewport##viewport." + std::to_string(panel.id().value);
            ImGui::Begin(name.c_str(), nullptr, panel.windowFlags());
            EditorContext context{.app = app,
                                  .editing = SceneEditing(scene, assets, document, history, diagnostics),
                                  .selection = selection,
                                  .diagnostics = diagnostics,
                                  .mode = EditorMode::Play,
                                  .actions = actions,
                                  .menus = menus};
            panel.update(context, input, io.DeltaTime);
            panel.draw(context);
            ImGui::End();
        }
        ImGui::Render();
    }

    void click(ImVec2 mouse) {
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame(mouse);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame(mouse);
    }

    void pressEscape(ImVec2 mouse) {
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
        frame(mouse);
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
        frame(mouse);
    }
};

ImVec2 centerOf(const ViewportPanel& panel) {
    return {panel.position().x + panel.size().x * 0.5f, panel.position().y + panel.size().y * 0.5f};
}

void testGameView() {
    GameViewFixture fixture;
    const Entity camera = addCamera(fixture.scene, "Player camera", {3.0f, 4.0f, 5.0f}, true);
    const glm::vec3 cameraPosition = sceneCameraLens(fixture.scene, camera)->position;
    const ImVec2 outside{1.0f, 719.0f};
    for (int index = 0; index < 3; ++index) {
        fixture.frame(outside);
    }
    const glm::vec3 editorPosition = fixture.left().lens(fixture.scene).position;
    expect(glm::length(editorPosition - cameraPosition) > 1e-3f, "an editing viewport looks through its editor camera");

    fixture.left().setGameInputFocus(true);
    expect(!fixture.left().gameInputFocus(), "only a game view takes game input focus");
    fixture.left().beginGameView();
    fixture.gameView = &fixture.left();
    expect(fixture.left().gameView() && fixture.left().gameInputFocus(), "a game view starts with input focus");
    const ImVec2 inside = centerOf(fixture.left());
    fixture.frame(inside);
    expect(glm::length(fixture.left().lens(fixture.scene).position - cameraPosition) < 1e-4f,
           "the game view looks through the game camera");
    expect(!fixture.right().gameView() &&
               glm::length(fixture.right().lens(fixture.scene).position - cameraPosition) > 1e-3f,
           "other viewports keep their editor cameras");
    expect(fixture.left().gameViewBanner() == GameViewBanner::ClickToCapture &&
               fixture.right().gameViewBanner() == GameViewBanner::None,
           "a focused game view with a free cursor offers to capture it");
    expect(fixture.left().gameViewHoldsCursor(), "the game may capture the cursor while the mouse is over its view");
    fixture.frame(centerOf(fixture.right()));
    expect(fixture.left().gameInputFocus() && !fixture.left().gameViewHoldsCursor(),
           "over another viewport the editor keeps the cursor, so a click there is not captured");

    InputState captured;
    captured.mouseCaptured = true;
    fixture.frame(inside, captured);
    expect(fixture.left().gameViewBanner() == GameViewBanner::EscReleasesCursor && fixture.left().gameViewHoldsCursor(),
           "while the game holds the mouse the banner says Esc releases it");

    fixture.actions.invoked.clear();
    fixture.pressEscape(inside);
    expect(std::ranges::count(fixture.actions.invoked, "play.release_game_input") == 1 &&
               !fixture.left().gameInputFocus() && fixture.left().gameViewBanner() == GameViewBanner::ClickToFocus,
           "Esc releases game input through play.release_game_input");

    fixture.actions.invoked.clear();
    fixture.click(inside);
    expect(std::ranges::count(fixture.actions.invoked, "play.focus_game_view") == 1 && fixture.left().gameInputFocus(),
           "a click on the game view takes focus through play.focus_game_view");
    expect(std::ranges::none_of(fixture.actions.invoked,
                                [](const std::string& id) { return id.starts_with("view3d.select"); }),
           "a click on the game view selects nothing");

    fixture.actions.invoked.clear();
    fixture.click(centerOf(fixture.right()));
    expect(!fixture.left().gameInputFocus(), "a click elsewhere releases game input");

    fixture.left().endGameView();
    fixture.gameView = nullptr;
    fixture.frame(outside);
    expect(!fixture.left().gameView() && fixture.left().gameViewBanner() == GameViewBanner::None &&
               glm::length(fixture.left().lens(fixture.scene).position - editorPosition) < 1e-4f,
           "ending the game view returns to the editor camera");

    // Without a game camera the game view falls back to the editor camera.
    fixture.scene.destroyEntity(camera);
    fixture.left().beginGameView();
    fixture.frame(inside);
    expect(glm::length(fixture.left().lens(fixture.scene).position - editorPosition) < 1e-4f,
           "a scene without a game camera plays through the editor camera");
    fixture.left().endGameView();
}

// What the fake game process saw and does; shared with the test.
struct FakeGame {
    std::vector<ChildProcessOptions> starts;
    bool running{false};
    bool cancelled{false};
    std::optional<int> exitCode;
    std::vector<std::string> pendingLines;
    bool failStart{false};
};

class FakeGameProcess final : public GameProcess {
  public:
    explicit FakeGameProcess(std::shared_ptr<FakeGame> game)
        : m_game(std::move(game)) {}

    ChildProcessResult start(const ChildProcessOptions& options) override {
        if (m_game->failStart) {
            return {.error = "no such program"};
        }
        m_game->starts.push_back(options);
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
        m_game->exitCode = 1;
    }

  private:
    std::shared_ptr<FakeGame> m_game;
};

size_t gameEntries(const Logger& logger, std::string_view text) {
    size_t count = 0;
    for (const LogEntry& entry : logger.entries()) {
        if (entry.category == StandalonePlayer::logCategory && entry.message.find(text) != std::string::npos) {
            ++count;
        }
    }
    return count;
}

void testStandalone(const fs::path& root) {
    fs::create_directories(root);
    const fs::path executable = root / "zenith-fake.exe";
    std::ofstream(executable) << "fake";
    const fs::path project = root / "game.zenith-project";
    std::ofstream(project) << "{}";
    const fs::path temporary = root / "play";

    AssetManager assets;
    Scene scene;
    scene.createEntity("Unsaved entity");
    const SceneDocumentOptions options{.assetRoot = ZENITH_ASSET_DIR};
    Logger logger(LoggerConfig{.echoToStderr = false});

    const auto game = std::make_shared<FakeGame>();
    StandalonePlayer player(std::make_unique<FakeGameProcess>(game));
    const StandaloneResult missing =
        player.start({.gameExecutable = root / "missing.exe", .temporaryDirectory = temporary}, scene, assets, options);
    expect(!missing && missing.error.find("missing.exe") != std::string::npos && game->starts.empty(),
           "a missing game executable fails before launching anything");

    const StandaloneLaunch launch{
        .gameExecutable = executable, .projectFile = project, .temporaryDirectory = temporary};
    expect(static_cast<bool>(player.start(launch, scene, assets, options)) && player.running(),
           "the standalone game starts");
    const fs::path sceneFile = player.sceneFile();
    std::error_code sameFolderError;
    expect(fs::is_regular_file(sceneFile) && fs::equivalent(sceneFile.parent_path(), temporary, sameFolderError) &&
               sceneFile.extension() == ".zenith-scene",
           "the edited scene is written to a temporary .zenith-scene");
    Scene written;
    expect(SceneDocument::load(sceneFile, assets, options, written).success &&
               written.registry().view<const NameComponent>().size() == 1,
           "the temporary scene holds the unsaved document");
    const std::vector<std::string> expected{executable.string(), "--project", fs::absolute(project).string(), "--scene",
                                            sceneFile.string()};
    expect(game->starts.size() == 1 && game->starts.front().command.arguments == expected &&
               game->starts.front().command.kind == ChildProcessCommand::Kind::Program &&
               game->starts.front().workingDirectory == executable.parent_path(),
           "the game runs as <game> --project <file> --scene <temporary scene> beside its executable");
    expect(!player.start(launch, scene, assets, options), "a second game cannot start while one runs");

    game->pendingLines = {"Vulkan ready", "Loaded scene"};
    expect(!player.poll(logger) && gameEntries(logger, "Vulkan ready") == 1 &&
               gameEntries(logger, "Loaded scene") == 1 && player.outputLines() == 2,
           "the game's output streams into the log");

    player.stop();
    expect(game->cancelled && player.stopRequested(), "Stop terminates the game process");
    game->pendingLines = {"last words"};
    expect(player.poll(logger) && !player.running() && gameEntries(logger, "last words") == 1 &&
               gameEntries(logger, "stopped") == 1,
           "the poll after Stop reports the end with the remaining output");
    expect(!fs::exists(sceneFile), "the temporary scene is removed when the game ends");
    expect(!player.poll(logger), "the end is reported once");

    // Exiting by itself, without a project file.
    expect(static_cast<bool>(
               player.start({.gameExecutable = executable, .temporaryDirectory = temporary}, scene, assets, options)),
           "the game starts again after it ended");
    expect(game->starts.back().command.arguments.size() == 3 && game->starts.back().command.arguments[1] == "--scene",
           "without a project file the game gets only the scene");
    const fs::path secondScene = player.sceneFile();
    game->running = false;
    game->exitCode = 3;
    expect(player.poll(logger) && !player.running() && gameEntries(logger, "code 3") == 1 && !fs::exists(secondScene),
           "a game that exits by itself ends the run and reports its exit code");

    game->failStart = true;
    const StandaloneResult failed = player.start(launch, scene, assets, options);
    expect(!failed && !player.running() && failed.error.find("no such program") != std::string::npos,
           "a process that cannot start reports why");
    std::error_code error;
    expect(fs::is_empty(temporary, error), "a failed start leaves no temporary scene behind");
}
} // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "zenith_play_targets_tests";
    std::error_code error;
    fs::remove_all(root, error);

    testTargetsAndPolicies();
    testGameCamera();
    testGameView();
    testStandalone(root);

    fs::remove_all(root, error);
    if (failures != 0) {
        std::cerr << failures << " play target test(s) failed\n";
        return 1;
    }
    std::cout << "Play target tests passed\n";
    return 0;
}
