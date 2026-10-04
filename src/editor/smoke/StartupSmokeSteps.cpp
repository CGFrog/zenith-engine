// The first editor smoke steps: render settings the renderer must accept, and
// a direct Play, pause, frame step, and Stop.

#include "editor/application/EditorPlayMode.hpp"
#include "editor/smoke/EditorSmokeHooks.hpp"
#include "editor/smoke/EditorSmokeSteps.hpp"

namespace {

class RenderSettingsSmokeStep final : public SmokeStep {
  public:
    std::string_view name() const override {
        return "render settings";
    }

    SmokeStepResult update(SmokeStepContext& context, uint32_t) override {
        RenderSettings settings = context.app.rendering.settings();
        settings.bloom = BloomQuality::Downsampled;
        settings.shadows = ShadowQuality::CascadedHigh;
        settings.visualizeShadowCascades = true;
        context.app.rendering.setSettings(settings);
        // Restored to 800x450 when the streaming load starts.
        context.app.rendering.requestViewportSize(640, 360);
        return SmokeStepResult::done();
    }
};

class PlayPauseStepStopSmokeStep final : public SmokeStep {
  public:
    std::string_view name() const override {
        return "play, pause, step, stop";
    }

    SmokeStepResult update(SmokeStepContext& context, uint32_t stepFrame) override {
        EditorSmokeHooks& editor = context.editor;
        switch (stepFrame) {
        case 0:
            editor.enterPlayMode();
            return SmokeStepResult::running();
        case 1:
            editor.playMode().pause();
            editor.playMode().requestFrameStep(context.app.physics.config().fixedTimeStep);
            return SmokeStepResult::running();
        default:
            editor.stopPlayMode();
            return SmokeStepResult::done();
        }
    }
};

} // namespace

std::unique_ptr<SmokeStep> makeRenderSettingsSmokeStep() {
    return std::make_unique<RenderSettingsSmokeStep>();
}

std::unique_ptr<SmokeStep> makePlayPauseStepStopSmokeStep() {
    return std::make_unique<PlayPauseStepStopSmokeStep>();
}
