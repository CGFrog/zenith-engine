#pragma once

// The editor smoke scenario's steps, each defined in its own file under
// smoke/. EditorSmokeScenario.cpp decides their order.

#include "editor/smoke/SmokeRunner.hpp"

#include <filesystem>
#include <memory>
#include <string>

struct ApplicationContext;
struct SceneDocumentOptions;

// Bloom, high cascaded shadows with the cascade view, and a small viewport.
std::unique_ptr<SmokeStep> makeRenderSettingsSmokeStep();
// Play, pause and step one frame, then Stop.
std::unique_ptr<SmokeStep> makePlayPauseStepStopSmokeStep();

// The streaming smoke: one step starts loading a large scene as an additive
// layer, and a later one waits for it to commit and releases the layers
// (which share the document's component registry) before the module reload.
struct StreamingSmokeSteps {
    std::unique_ptr<SmokeStep> startLoad;
    std::unique_ptr<SmokeStep> waitForCommit;
    // Why the streamed scene could not be written; the steps are null then.
    std::string error;
};

// Writes the scene to stream to scenePath, which the steps remove again.
StreamingSmokeSteps makeStreamingSmokeSteps(ApplicationContext& app, const SceneDocumentOptions& options,
                                            std::filesystem::path scenePath);

// The viewport sidebar and tool shelf, the lens, and the Info log.
std::unique_ptr<SmokeStep> makeViewportChromeSmokeStep();
// A second Hierarchy area survives a workspace switch and back.
std::unique_ptr<SmokeStep> makeWorkspaceSmokeStep();
// H hides everything in the editor views, Alt+H reveals it.
std::unique_ptr<SmokeStep> makeHideSmokeStep();
// Prefab create, instantiate, overrides, apply, and nesting on a staging
// scene, with prefabs written under assetRoot (removed again).
std::unique_ptr<SmokeStep> makePrefabSmokeStep(std::filesystem::path assetRoot);
// Two viewports with their own cameras, shading, and render views.
std::unique_ptr<SmokeStep> makeMultiViewportSmokeStep();
// Multi-entity component operators and their undo.
std::unique_ptr<SmokeStep> makeComponentSmokeStep();
// The Add menu through wm.call_menu and its search.
std::unique_ptr<SmokeStep> makeMenuSmokeStep();
// Simulate at half speed runs only the systems it should; Stop restores.
std::unique_ptr<SmokeStep> makeSimulateSmokeStep();
// The Hierarchy's Collections mode with collection operators and undo.
std::unique_ptr<SmokeStep> makeOutlinerSmokeStep();
// A GPU click pick and local view in the active viewport.
std::unique_ptr<SmokeStep> makePickingSmokeStep();
// An Empty spawns at the 3D cursor placed in the viewport.
std::unique_ptr<SmokeStep> makeCursorSmokeStep();
// The Shading pie through wm.call_pie.
std::unique_ptr<SmokeStep> makePieMenuSmokeStep();
// Play in the active viewport, then the standalone game when it is built.
std::unique_ptr<SmokeStep> makePlayTargetSmokeStep();
// An edit in Play, Apply to Edit Scene, Stop, and undo.
std::unique_ptr<SmokeStep> makeLiveEditSmokeStep();
// Hot reloads the project's game module once (nothing without a module).
std::unique_ptr<SmokeStep> makeModuleReloadSmokeStep();
