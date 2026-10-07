#pragma once

// Edit/Play/Simulate state: snapshot on enter, restore on stop, time scale, pause, frame step.

#include "engine/application/SimulationClock.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/module/SystemScheduler.hpp"
#include "engine/scene/SceneDocument.hpp"

#include <span>
#include <vector>

// What the editor is doing with the scene. Play and Simulate both run on a
// throwaway copy of the document (the "runtime scene"): anything that treats
// the scene as authored data (undo, autosave, module reloads, document
// operators) waits for Edit. Paused holds either of them; see
// EditorPlayMode::simulating().
enum class EditorMode {
    Edit,
    Play,
    Paused,
    // Physics and the systems enabled for Simulate run while the editor
    // keeps its own camera, and no player input reaches the scene.
    Simulate,
};

// What EditorPlayMode::enter starts.
enum class EditorRuntime {
    // The game as a player sees it: every Play system, with player input.
    Play,
    // Physics and the Play systems the Simulate mask enables, without input.
    Simulate,
};

class EditorPlayMode {
  public:
    // Runtime changes are intentionally discarded on stop; Apply to Edit
    // Scene carries selected runtime state back by patching editScene().
    // The runtime scene is built from the edit scene's document, so
    // transient entities (a generator's editor preview) do not enter Play:
    // the game's own generator builds its content there. They stay in the
    // parked edit scene and are back after Stop.
    SceneDocumentResult enter(Scene& activeScene, AssetManager& assets, const SceneDocumentOptions& options,
                              std::span<const SceneEntityId> selection, EditorRuntime runtime = EditorRuntime::Play);
    void pause();
    void resume();
    // While paused, advances the next frame (the current one when asked
    // before the engine reads the clock) by stepSeconds of simulated time:
    // Update systems, animation, and audio for one frame, plus the fixed
    // steps that frame accumulates (one when stepSeconds is the fixed time
    // step). The step lasts until finishFrame. Ignored unless paused or for
    // a step that is not positive.
    void requestFrameStep(float stepSeconds);
    // Ends the frame a step was requested for; call once per frame after the
    // engine's fixed steps (EditorSession::drawUi).
    void finishFrame();
    void stop(Scene& activeScene, std::vector<SceneEntityId>& selection);

    EditorMode mode() const;
    // Whether the runtime scene (running or paused) was entered as Simulate.
    bool simulating() const;
    // Play or Simulate running, not paused.
    bool running() const;
    // Paused with a frame step requested for this frame.
    bool steppingFrame() const;
    // Whether this frame advances the runtime scene: running or stepping.
    bool advancesThisFrame() const;
    // The SimulationMode systems run in: Edit, Play, or Simulate.
    SimulationMode simulationMode() const;
    bool fixedUpdatesEnabled() const;
    bool beginFixedUpdate();

    // Scales simulated time in Play and Simulate (see SimulationClock); kept
    // across runs. Clamped to [0, 10]; returns the value kept.
    float setTimeScale(float scale);
    float timeScale() const;
    // Edit mode runs at wall-clock rate (animation previews and audio keep
    // playing); a runtime scene runs at the time scale, or while paused only
    // for a requested frame step.
    SimulationClock simulationClock() const;

    // The edit scene parked while Play or Simulate runs, which stop() swaps
    // back; null in Edit mode. Apply to Edit Scene patches it (through
    // SceneEditing::applyToEditScene) so Stop restores the patched version.
    Scene* editScene();
    // Renews the leases on the parked edit scene's assets; call once per
    // frame. The engine leases only the active (runtime) scene, and the edit
    // scene can hold assets the runtime scene no longer uses (transient
    // entities never enter Play; Play may destroy entities), which a forced
    // AssetManager::collectUnused would otherwise evict before Stop. No-op in
    // Edit mode.
    void leaseEditSceneAssets(const AssetManager& assets);

  private:
    Scene m_editScene;
    // Held from enter() to stop(); see leaseEditSceneAssets.
    AssetLeaseSet m_editSceneAssets;
    std::vector<SceneEntityId> m_editSelection;
    EditorMode m_mode{EditorMode::Edit};
    bool m_simulating{false};
    // The requested frame step's length; zero for none.
    float m_frameStepSeconds{0.0f};
    float m_timeScale{1.0f};
};
