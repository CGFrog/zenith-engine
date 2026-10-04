// Simulate: run at half speed until a physics-step system has run a few times
// (or for at most simulateFrames when the module has none), checking that
// only the systems Simulate runs do, then Stop must restore the scene.

#include "editor/application/EditorPlayMode.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/modules/GameModuleHost.hpp"
#include "editor/smoke/EditorSmokeHooks.hpp"
#include "editor/smoke/EditorSmokeSteps.hpp"
#include "engine/scene/SceneDocument.hpp"

namespace {

class SimulateSmokeStep final : public SmokeStep {
  public:
    std::string_view name() const override {
        return "Simulate";
    }

    SmokeStepResult update(SmokeStepContext& context, uint32_t stepFrame) override {
        EditorSmokeHooks& editor = context.editor;
        EditorPlayMode& playMode = editor.playMode();
        if (stepFrame == 0) {
            if (!snapshot(context, m_snapshot)) {
                return SmokeStepResult::failed("the scene could not be serialized: " + m_snapshot);
            }
            editor.invokeOperator("play.set_time_scale", R"({"scale":0.5})", KeymapRegion::Global);
            if (!editor.invokeOperator("play.simulate", {}, KeymapRegion::Global) ||
                playMode.mode() != EditorMode::Simulate || playMode.timeScale() != 0.5f) {
                return SmokeStepResult::failed("play.simulate did not enter Simulate at half speed");
            }
            return SmokeStepResult::running();
        }
        if (playMode.mode() == EditorMode::Edit) {
            return SmokeStepResult::failed("Simulate ended before the step stopped it");
        }
        bool fixedSystemsEnabled = false;
        if (std::string problem = countSystemRuns(editor.moduleHost(), fixedSystemsEnabled); !problem.empty()) {
            return SmokeStepResult::failed(problem);
        }
        const bool fixedDone = !fixedSystemsEnabled || m_fixedRuns >= 3;
        if (stepFrame < 3 || (!fixedDone && stepFrame < simulateFrames)) {
            return SmokeStepResult::running();
        }
        if (!fixedDone) {
            return SmokeStepResult::failed("no fixed-update system ran in Simulate");
        }
        editor.invokeOperator("play.stop", {}, KeymapRegion::Global);
        editor.invokeOperator("play.set_time_scale", R"({"scale":1.0})", KeymapRegion::Global);
        std::string after;
        if (!snapshot(context, after)) {
            return SmokeStepResult::failed("the scene could not be serialized: " + after);
        }
        if (playMode.mode() != EditorMode::Edit || after != m_snapshot) {
            return SmokeStepResult::failed("Stop did not restore the scene from before Simulate");
        }
        return SmokeStepResult::done();
    }

    uint32_t frameBudget() const override {
        return simulateFrames + 30;
    }

  private:
    // The frames the step simulates at most while waiting for fixed-update
    // systems.
    static constexpr uint32_t simulateFrames = 120;

    // The scene's document text in text, or the error there when false.
    static bool snapshot(SmokeStepContext& context, std::string& text) {
        if (const SceneDocumentResult result = SceneDocument::serialize(context.app.scene, context.app.assets,
                                                                        context.editor.document().options(), text);
            !result) {
            text = result.error;
            return false;
        }
        return true;
    }

    // Adds the fixed-update runs of the previous frame (the profile publishes
    // the previous frame, which ran in Simulate); a problem when a system ran
    // that Simulate does not run.
    std::string countSystemRuns(const GameModuleHost& host, bool& fixedSystemsEnabled) {
        const SystemProfile& profile = host.systemProfile();
        const SimulateSystemMask& mask = host.simulateMask();
        for (const SystemPhase phase : systemPhases) {
            for (const SystemDescriptor* system : host.systems(phase)) {
                const SystemTiming* timing = profile.find(system->name);
                const bool runs = runsIn(system->runIn, SimulationMode::Simulate) && mask.enabled(system->name, phase);
                if (!runs && timing != nullptr && timing->frameRuns > 0) {
                    return "system '" + system->name + "' ran although Simulate does not run it";
                }
                if (runs && phase == SystemPhase::FixedUpdate) {
                    fixedSystemsEnabled = true;
                    m_fixedRuns += timing != nullptr ? timing->frameRuns : 0;
                }
            }
        }
        return {};
    }

    // The scene before Simulate, and the fixed-update system runs seen.
    std::string m_snapshot;
    uint32_t m_fixedRuns{0};
};

} // namespace

std::unique_ptr<SmokeStep> makeSimulateSmokeStep() {
    return std::make_unique<SimulateSmokeStep>();
}
