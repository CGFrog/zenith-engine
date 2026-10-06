#include "EditorSmokeScenario.hpp"

#include "editor/smoke/EditorSmokeSteps.hpp"
#include "engine/scene/SceneDocument.hpp"

#include <utility>

EditorSmokeScenario buildEditorSmokeScenario(ApplicationContext& app, const SceneDocumentOptions& options,
                                             const std::filesystem::path& scratchDirectory) {
    StreamingSmokeSteps streaming =
        makeStreamingSmokeSteps(app, options, scratchDirectory / "streaming-smoke.zenith-scene");
    if (!streaming.error.empty()) {
        return {.error = std::move(streaming.error)};
    }

    // The order the steps run in, one after another. A new step adds one
    // line here; each step restores what it changed, so it may go anywhere
    // that suits its preconditions.
    auto runner = std::make_unique<SmokeRunner>();
    runner->add(makeRenderSettingsSmokeStep());
    runner->add(makePlayPauseStepStopSmokeStep());
    // Streams while the steps below run.
    runner->add(std::move(streaming.startLoad));
    runner->add(makeViewportChromeSmokeStep());
    runner->add(makeWorkspaceSmokeStep());
    runner->add(makeHideSmokeStep());
    runner->add(makePrefabSmokeStep(scratchDirectory / "prefab-smoke"));
    runner->add(makePrefabEditSmokeStep());
    runner->add(makePrefabWindowSmokeStep());
    runner->add(makeMultiViewportSmokeStep());
    runner->add(makeComponentSmokeStep());
    runner->add(makeMenuSmokeStep());
    runner->add(makeSimulateSmokeStep());
    runner->add(makeOutlinerSmokeStep());
    runner->add(makePickingSmokeStep());
    runner->add(makeCursorSmokeStep());
    runner->add(makeGeneratedPreviewSmokeStep());
    runner->add(makePieMenuSmokeStep());
    runner->add(makePlayTargetSmokeStep());
    runner->add(makeLiveEditSmokeStep());
    // Releases the streaming layers, which hold the module's component types.
    runner->add(std::move(streaming.waitForCommit));
    runner->add(makeModuleReloadSmokeStep());
    return {.runner = std::move(runner)};
}
