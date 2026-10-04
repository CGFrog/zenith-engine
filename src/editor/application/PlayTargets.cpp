#include "PlayTargets.hpp"

#include "engine/assets/Assets.hpp"
#include "engine/diagnostics/Logger.hpp"

#include <array>
#include <cstdio>
#include <random>
#include <system_error>

namespace {
constexpr std::array<PlayTargetInfo, 3> targetInfos{{
    {PlayTarget::ActiveViewport, "viewport", "Active Viewport"},
    {PlayTarget::FloatingViewport, "floating", "Floating Viewport"},
    {PlayTarget::Standalone, "standalone", "Standalone Game"},
}};

// UTF-8 text of a path, for messages and ChildProcess arguments.
std::string utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

// A file name no other editor writing to the same folder picks.
std::string uniqueSceneName() {
    std::random_device device;
    std::array<char, 64> name{};
    std::snprintf(name.data(), name.size(), "play-%08x%08x.zenith-scene", device(), device());
    return name.data();
}

class ChildGameProcess final : public GameProcess {
  public:
    ChildProcessResult start(const ChildProcessOptions& options) override {
        return m_process.start(options);
    }

    bool running() const override {
        return m_process.running();
    }

    std::optional<int> exitCode() const override {
        return m_process.exitCode();
    }

    std::vector<std::string> takeLines() override {
        return m_process.takeLines();
    }

    void cancel() override {
        m_process.cancel();
    }

  private:
    ChildProcess m_process;
};
} // namespace

std::span<const PlayTargetInfo> playTargets() {
    return targetInfos;
}

const PlayTargetInfo& playTargetInfo(PlayTarget target) {
    for (const PlayTargetInfo& info : targetInfos) {
        if (info.target == target) {
            return info;
        }
    }
    return targetInfos.front();
}

std::optional<PlayTarget> parsePlayTarget(std::string_view key) {
    for (const PlayTargetInfo& info : targetInfos) {
        if (info.key == key) {
            return info.target;
        }
    }
    return std::nullopt;
}

Entity gameCamera(const Scene& scene) {
    auto cameras = scene.registry().view<const CameraComponent, const TransformComponent>();
    for (const Entity entity : cameras) {
        if (cameras.get<const CameraComponent>(entity).primary) {
            return entity;
        }
    }
    return entt::null;
}

GameViewBanner gameViewBanner(bool gameView, bool inputFocus, bool cursorCaptured) {
    if (!gameView) {
        return GameViewBanner::None;
    }
    if (!inputFocus) {
        return GameViewBanner::ClickToFocus;
    }
    return cursorCaptured ? GameViewBanner::EscReleasesCursor : GameViewBanner::ClickToCapture;
}

std::string_view gameViewBannerText(GameViewBanner banner) {
    switch (banner) {
    case GameViewBanner::None:
        return {};
    case GameViewBanner::ClickToFocus:
        return "Game input paused - click the view to play";
    case GameViewBanner::ClickToCapture:
        return "Playing - click to capture the cursor";
    case GameViewBanner::EscReleasesCursor:
        return "Esc releases the cursor";
    }
    return {};
}

MouseCapturePolicy playMouseCapturePolicy(bool gameViewHoldsCursor) {
    return gameViewHoldsCursor ? MouseCapturePolicy::Persistent : MouseCapturePolicy::WhileRightMouseDown;
}

ThemeModeTint playModeTint(EditorMode mode, bool simulating) {
    switch (mode) {
    case EditorMode::Edit:
        return ThemeModeTint::None;
    case EditorMode::Play:
        return ThemeModeTint::Play;
    case EditorMode::Simulate:
        return ThemeModeTint::Simulate;
    case EditorMode::Paused:
        return simulating ? ThemeModeTint::Simulate : ThemeModeTint::Play;
    }
    return ThemeModeTint::None;
}

std::vector<std::string> standaloneArguments(const std::filesystem::path& gameExecutable,
                                             const std::filesystem::path& projectFile,
                                             const std::filesystem::path& sceneFile) {
    // ChildProcess takes UTF-8 arguments; path::string() would use the
    // Windows code page and mangle (or throw on) other characters.

    std::vector<std::string> arguments{utf8(gameExecutable)};
    if (!projectFile.empty()) {
        arguments.push_back("--project");
        arguments.push_back(utf8(std::filesystem::absolute(projectFile)));
    }
    arguments.push_back("--scene");
    arguments.push_back(utf8(std::filesystem::absolute(sceneFile)));
    return arguments;
}

std::unique_ptr<GameProcess> makeGameProcess() {
    return std::make_unique<ChildGameProcess>();
}

std::filesystem::path gameExecutableBeside(const std::filesystem::path& editorDirectory) {
#ifdef _WIN32
    return editorDirectory / "zenith.exe";
#else
    return editorDirectory / "zenith";
#endif
}

StandalonePlayer::StandalonePlayer(std::unique_ptr<GameProcess> process)
    : m_process(std::move(process)) {}

StandalonePlayer::~StandalonePlayer() {
    if (m_running) {
        m_process->cancel();
    }
    removeSceneFile();
}

StandaloneResult StandalonePlayer::start(const StandaloneLaunch& launch, const Scene& scene, const AssetManager& assets,
                                         const SceneDocumentOptions& options) {
    if (m_running) {
        return {.error = "A standalone game is already running"};
    }
    std::error_code error;
    if (launch.gameExecutable.empty() || !std::filesystem::is_regular_file(launch.gameExecutable, error)) {
        return {.error = "The game executable '" + utf8(launch.gameExecutable) +
                         "' does not exist; build the zenith target first"};
    }
    if (!launch.projectFile.empty() && !std::filesystem::is_regular_file(launch.projectFile, error)) {
        return {.error = "The project file '" + utf8(launch.projectFile) + "' does not exist"};
    }
    std::filesystem::create_directories(launch.temporaryDirectory, error);
    if (error) {
        return {.error = "Could not create '" + utf8(launch.temporaryDirectory) + "': " + error.message()};
    }
    removeSceneFile();
    m_sceneFile = std::filesystem::absolute(launch.temporaryDirectory / uniqueSceneName(), error);
    if (const SceneDocumentResult saved = SceneDocument::save(m_sceneFile, scene, assets, options); !saved) {
        m_sceneFile.clear();
        return {.error = "Could not write the scene for the standalone game: " + saved.error};
    }
    m_arguments = standaloneArguments(launch.gameExecutable, launch.projectFile, m_sceneFile);
    // The game resolves its resources beside its executable.
    const ChildProcessOptions processOptions{.command = ChildProcessCommand::program(m_arguments),
                                             .workingDirectory = launch.gameExecutable.parent_path()};
    if (const ChildProcessResult started = m_process->start(processOptions); !started) {
        removeSceneFile();
        return {.error = "Could not start the standalone game: " + started.error};
    }
    m_running = true;
    m_stopRequested = false;
    m_outputLines = 0;
    return {.success = true};
}

void StandalonePlayer::logOutput(Logger& logger) {
    for (const std::string& line : m_process->takeLines()) {
        logger.log(LogLevel::Info, logCategory, line);
        ++m_outputLines;
    }
}

bool StandalonePlayer::poll(Logger& logger) {
    if (!m_running) {
        return false;
    }
    logOutput(logger);
    if (m_process->running()) {
        return false;
    }
    // Lines read between the last take and the exit.
    logOutput(logger);
    m_running = false;
    const std::optional<int> exitCode = m_process->exitCode();
    if (m_stopRequested) {
        logger.log(LogLevel::Info, logCategory, "Standalone game stopped");
    } else if (exitCode && *exitCode == 0) {
        logger.log(LogLevel::Info, logCategory, "Standalone game exited");
    } else {
        logger.log(LogLevel::Warning, logCategory,
                   "Standalone game exited with code " + (exitCode ? std::to_string(*exitCode) : std::string("?")));
    }
    removeSceneFile();
    return true;
}

void StandalonePlayer::stop() {
    if (m_running && !m_stopRequested) {
        m_stopRequested = true;
        m_process->cancel();
    }
}

bool StandalonePlayer::running() const {
    return m_running;
}

bool StandalonePlayer::stopRequested() const {
    return m_running && m_stopRequested;
}

const std::filesystem::path& StandalonePlayer::sceneFile() const {
    return m_sceneFile;
}

const std::vector<std::string>& StandalonePlayer::arguments() const {
    return m_arguments;
}

size_t StandalonePlayer::outputLines() const {
    return m_outputLines;
}

void StandalonePlayer::removeSceneFile() {
    if (m_sceneFile.empty()) {
        return;
    }
    // A file the game still holds open stays behind in the temporary folder.
    std::error_code ignored;
    std::filesystem::remove(m_sceneFile, ignored);
}
