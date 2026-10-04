#pragma once

#include "editor/application/EditorPlayMode.hpp"
#include "editor/theme/ThemeParameters.hpp"
#include "engine/input/InputManager.hpp"
#include "engine/platform/ChildProcess.hpp"
#include "engine/scene/Scene.hpp"
#include "engine/scene/SceneDocument.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class AssetManager;
class Logger;

// Where play.start runs the game (EditorSession's play workflows,
// EditorPlayback.cpp).
enum class PlayTarget : uint8_t {
    // The active viewport shows the game camera instead of its editor camera
    // and holds game input while it has focus; other viewports keep editing.
    ActiveViewport,
    // A new Viewport area, floating over the editor and locked to the game
    // camera like ActiveViewport; Stop closes it again.
    FloatingViewport,
    // A separate game process (see StandalonePlayer); the editor stays in
    // Edit mode and its scene does not run.
    Standalone,
};

struct PlayTargetInfo {
    PlayTarget target;
    // The play.start "target" property, for example "viewport".
    std::string_view key;
    std::string_view label;
};

// Every target, the default (ActiveViewport) first.
std::span<const PlayTargetInfo> playTargets();
const PlayTargetInfo& playTargetInfo(PlayTarget target);
std::optional<PlayTarget> parsePlayTarget(std::string_view key);

// The camera a game view shows: the scene's first CameraComponent marked
// primary that has a transform, in the order the renderer chooses its
// fallback camera (MeshPass::prepareViewFrame), so it is the camera the game
// itself renders through and its camera systems move. entt::null without one.
Entity gameCamera(const Scene& scene);

// What a game view's banner tells the user about game input.
enum class GameViewBanner : uint8_t {
    // Not a game view.
    None,
    // Game input is paused: the view lost focus.
    ClickToFocus,
    // The game has input but the cursor is free; a click captures it.
    ClickToCapture,
    // The game holds the mouse; Esc releases it.
    EscReleasesCursor,
};

GameViewBanner gameViewBanner(bool gameView, bool inputFocus, bool cursorCaptured);
std::string_view gameViewBannerText(GameViewBanner banner);

// The editor's mouse capture: a game view that may hold the cursor
// (ViewportPanel::gameViewHoldsCursor) captures it on a click and keeps it
// until Esc (Persistent); otherwise only while the right mouse button flies
// an editor camera.
MouseCapturePolicy playMouseCapturePolicy(bool gameViewHoldsCursor);

// The theme wash for the editor's mode: Play (running or paused) is warm,
// Simulate violet, Edit untinted.
ThemeModeTint playModeTint(EditorMode mode, bool simulating);

// The game process contract: `<game> [--project <project file>] --scene
// <absolute path to a .zenith-scene>`. The scene replaces the project's
// startup scene for that run (src/main.cpp); without a project file the game
// runs the scene with its default project.
std::vector<std::string> standaloneArguments(const std::filesystem::path& gameExecutable,
                                             const std::filesystem::path& projectFile,
                                             const std::filesystem::path& sceneFile);

// The child process the standalone target runs; tests replace it with a
// fake.
class GameProcess {
  public:
    virtual ~GameProcess() = default;

    virtual ChildProcessResult start(const ChildProcessOptions& options) = 0;
    // True from a successful start until the process has exited and its
    // output has been read.
    virtual bool running() const = 0;
    virtual std::optional<int> exitCode() const = 0;
    virtual std::vector<std::string> takeLines() = 0;
    // Terminates the process tree; running() turns false once it is reaped.
    virtual void cancel() = 0;
};

// A GameProcess backed by ChildProcess.
std::unique_ptr<GameProcess> makeGameProcess();

// What the Standalone target runs; the editor application supplies it.
struct EditorPlayServices {
    // The game executable; empty means Standalone is unavailable.
    std::filesystem::path gameExecutable;
    // Creates the game's process; empty uses makeGameProcess. Tests pass a
    // fake.
    std::function<std::unique_ptr<GameProcess>()> makeGameProcess;
    // Where the temporary scenes go; empty uses a "zenith-play" folder in the
    // system's temporary directory.
    std::filesystem::path temporaryDirectory;
};

// `zenith` (with the platform's executable suffix) in editorDirectory, where
// the build and packages put the game beside the editor.
std::filesystem::path gameExecutableBeside(const std::filesystem::path& editorDirectory);

struct StandaloneLaunch {
    // The game executable (`zenith`); must exist.
    std::filesystem::path gameExecutable;
    // The open project's .zenith-project file; empty for the default project.
    std::filesystem::path projectFile;
    // Where the temporary scene file is written; created when missing.
    std::filesystem::path temporaryDirectory;
};

struct StandaloneResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

// Plays the edited scene in a separate game process: writes the scene (with
// unsaved changes) to a temporary .zenith-scene, launches the game on it,
// streams the process's stdout and stderr into a Logger (category "game"),
// and removes the file once the process ends. One process at a time; all
// members run on the owning (UI) thread.
class StandalonePlayer {
  public:
    static constexpr std::string_view logCategory = "game";

    explicit StandalonePlayer(std::unique_ptr<GameProcess> process);
    ~StandalonePlayer();
    StandalonePlayer(const StandalonePlayer&) = delete;
    StandalonePlayer& operator=(const StandalonePlayer&) = delete;

    // Fails, launching nothing, while a process runs, when the executable is
    // missing, or when the scene cannot be written or the process started.
    StandaloneResult start(const StandaloneLaunch& launch, const Scene& scene, const AssetManager& assets,
                           const SceneDocumentOptions& options);
    // Moves new output lines into logger; returns true on the one poll that
    // sees the process end (by itself or after stop), which logs its exit.
    bool poll(Logger& logger);
    // Terminates the process; the next polls report its end.
    void stop();
    // From a successful start until the poll that reports the end.
    bool running() const;
    bool stopRequested() const;
    // The temporary scene of the current (or last) run.
    const std::filesystem::path& sceneFile() const;
    // The command line of the current (or last) run.
    const std::vector<std::string>& arguments() const;
    // Output lines of the current (or last) run moved into the log so far.
    size_t outputLines() const;

  private:
    void removeSceneFile();
    void logOutput(Logger& logger);

    std::unique_ptr<GameProcess> m_process;
    std::filesystem::path m_sceneFile;
    std::vector<std::string> m_arguments;
    size_t m_outputLines{0};
    bool m_running{false};
    bool m_stopRequested{false};
};
