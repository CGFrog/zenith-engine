// Play targets: Play in the active viewport (it switches to the game camera,
// holds game input with Persistent mouse capture, shows its banner, pauses
// and steps one frame, and Stop restores the scene and the editor camera);
// then, when the game executable is built, play standalone: launch it on a
// temporary scene, stream its output for a while, and stop it.

#include "editor/application/EditorPlayMode.hpp"
#include "editor/application/PlayTargets.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/interaction/EditorCamera.hpp"
#include "editor/panels/ViewportPanel.hpp"
#include "editor/panels/ViewportRegistry.hpp"
#include "editor/smoke/EditorSmokeHooks.hpp"
#include "editor/smoke/EditorSmokeSteps.hpp"
#include "engine/scene/SceneDocument.hpp"

#include <limits>
#include <system_error>

namespace {

class PlayTargetSmokeStep final : public SmokeStep {
  public:
    std::string_view name() const override {
        return "play targets";
    }

    SmokeStepResult update(SmokeStepContext& context, uint32_t stepFrame) override {
        if (stepFrame < standaloneFrame) {
            return updateViewportPhase(context, stepFrame);
        }
        if (stepFrame == standaloneFrame) {
            return launchStandalone(context, stepFrame);
        }
        return updateStandalone(context, stepFrame);
    }

    uint32_t frameBudget() const override {
        return lastFrame + 30;
    }

  private:
    // Step frames: the viewport phase runs 0-4, the standalone game launches
    // at standaloneFrame, is stopped by standaloneStopFrame at the latest,
    // and must have ended by lastFrame.
    static constexpr uint32_t standaloneFrame = 6;
    static constexpr uint32_t standaloneStopFrame = 50;
    static constexpr uint32_t lastFrame = 70;

    static bool snapshot(SmokeStepContext& context, std::string& text) {
        if (const SceneDocumentResult result = SceneDocument::serialize(context.app.scene, context.app.assets,
                                                                        context.editor.document().options(), text);
            !result) {
            text = result.error;
            return false;
        }
        return true;
    }

    SmokeStepResult updateViewportPhase(SmokeStepContext& context, uint32_t stepFrame) {
        EditorSmokeHooks& editor = context.editor;
        ViewportPanel* viewport = editor.gameViewport();
        if (stepFrame == 0) {
            if (editor.viewports().active() == nullptr) {
                context.app.logger.log(LogLevel::Info, "editor",
                                       "Play target smoke: viewport phase skipped, no viewport");
                return SmokeStepResult::running();
            }
            if (!snapshot(context, m_snapshot)) {
                return SmokeStepResult::failed("the scene could not be serialized: " + m_snapshot);
            }
            if (!editor.invokeOperator("play.start", R"({"target":"viewport"})", KeymapRegion::Global) ||
                editor.playMode().mode() != EditorMode::Play || editor.gameViewport() == nullptr) {
                return SmokeStepResult::failed("play.start did not play in the active viewport");
            }
            if (!editor.gameViewport()->gameInputFocus() || !editor.gameReceivesInput()) {
                return SmokeStepResult::failed(
                    "the game view should start with input focus, routing input to the game");
            }
            return SmokeStepResult::running();
        }
        if (viewport == nullptr) {
            return SmokeStepResult::running();
        }
        switch (stepFrame) {
        case 2:
            // The view has been drawn in game view since.
            return checkGameView(context, *viewport);
        case 3:
            if (!editor.invokeOperator("play.step", {}, KeymapRegion::Global) || !editor.playMode().steppingFrame()) {
                return SmokeStepResult::failed("play.step did not request a frame step while paused");
            }
            return SmokeStepResult::running();
        case 4: {
            if (editor.playMode().mode() != EditorMode::Paused || editor.playMode().steppingFrame()) {
                return SmokeStepResult::failed("a frame step should last one frame and stay paused");
            }
            editor.invokeOperator("play.stop", {}, KeymapRegion::Global);
            std::string after;
            if (!snapshot(context, after)) {
                return SmokeStepResult::failed("the scene could not be serialized: " + after);
            }
            if (editor.playMode().mode() != EditorMode::Edit || viewport->gameView() || after != m_snapshot) {
                return SmokeStepResult::failed("Stop did not end the game view and restore the scene");
            }
            return SmokeStepResult::running();
        }
        default:
            return SmokeStepResult::running();
        }
    }

    SmokeStepResult checkGameView(SmokeStepContext& context, ViewportPanel& viewport) {
        EditorSmokeHooks& editor = context.editor;
        const Scene& scene = context.app.scene;
        const std::optional<RenderView> cameraLens = sceneCameraLens(scene, gameCamera(scene));
        if (cameraLens && glm::length(viewport.lens(scene).position - cameraLens->position) > 1e-3f) {
            return SmokeStepResult::failed("the game view does not look through the game camera");
        }
        for (const ViewportPanel* other : editor.viewports().openViewports()) {
            if (other != &viewport && other->gameView()) {
                return SmokeStepResult::failed("only the playing viewport should show the game");
            }
        }
        const GameViewBanner banner = viewport.gameViewBanner();
        if (banner == GameViewBanner::None) {
            return SmokeStepResult::failed("the game view shows no input banner");
        }
        context.app.logger.log(LogLevel::Info, "editor",
                               std::string("Play target smoke: game camera ") + (cameraLens ? "found" : "absent") +
                                   ", banner '" + std::string(gameViewBannerText(banner)) + "'");
        editor.invokeOperator("play.release_game_input", {}, KeymapRegion::Global);
        if (editor.mouseCapturePolicy() != MouseCapturePolicy::WhileRightMouseDown || editor.gameReceivesInput()) {
            return SmokeStepResult::failed(
                "releasing game input should restore editor mouse capture and stop routing input");
        }
        editor.invokeOperator("play.toggle_pause", {}, KeymapRegion::Global);
        return SmokeStepResult::running();
    }

    SmokeStepResult launchStandalone(SmokeStepContext& context, uint32_t stepFrame) {
        EditorSmokeHooks& editor = context.editor;
        std::error_code error;
        if (!std::filesystem::is_regular_file(editor.gameExecutable(), error)) {
            context.app.logger.log(LogLevel::Info, "editor",
                                   "Play target smoke: standalone phase skipped, the game executable '" +
                                       editor.gameExecutable().string() + "' is not built");
            return SmokeStepResult::done();
        }
        m_gameLogSequence = context.app.logger.snapshotAfter(std::numeric_limits<uint64_t>::max()).latestSequence;
        m_standaloneFrame = stepFrame;
        if (!editor.invokeOperator("play.start", R"({"target":"standalone"})", KeymapRegion::Global) ||
            !editor.standalone().running() ||
            !std::filesystem::is_regular_file(editor.standalone().sceneFile(), error)) {
            return SmokeStepResult::failed("play.start did not launch the standalone game on a temporary scene");
        }
        return SmokeStepResult::running();
    }

    SmokeStepResult updateStandalone(SmokeStepContext& context, uint32_t stepFrame) {
        EditorSmokeHooks& editor = context.editor;
        const StandalonePlayer& standalone = editor.standalone();
        if (!standalone.running()) {
            // Ended by itself or after Stop: the session left Play.
            size_t loggedLines = 0;
            for (const LogEntry& entry : context.app.logger.entriesAfter(m_gameLogSequence)) {
                loggedLines += entry.category == StandalonePlayer::logCategory ? 1 : 0;
            }
            context.app.logger.log(LogLevel::Info, "editor",
                                   "Play target smoke: the standalone game ran for " +
                                       std::to_string(stepFrame - m_standaloneFrame) + " editor frames; " +
                                       std::to_string(standalone.outputLines()) + " output line(s) reached the log");
            if (editor.playTargetActive() || loggedLines < standalone.outputLines()) {
                return SmokeStepResult::failed(
                    "the standalone game's end should leave Play with its output in the log");
            }
            return SmokeStepResult::done();
        }
        // Stop once the game has run a while and printed something, or at the
        // end of the window.
        const bool ranAWhile = standalone.outputLines() > 0 && stepFrame >= m_standaloneFrame + 30;
        if (!standalone.stopRequested() && (ranAWhile || stepFrame >= standaloneStopFrame)) {
            editor.invokeOperator("play.stop", {}, KeymapRegion::Global);
        } else if (stepFrame >= lastFrame) {
            return SmokeStepResult::failed("the standalone game did not end after Stop");
        }
        return SmokeStepResult::running();
    }

    // The scene from before Play, the step frame the standalone game
    // launched, and the log sequence before that.
    std::string m_snapshot;
    uint32_t m_standaloneFrame{0};
    uint64_t m_gameLogSequence{0};
};

} // namespace

std::unique_ptr<SmokeStep> makePlayTargetSmokeStep() {
    return std::make_unique<PlayTargetSmokeStep>();
}
