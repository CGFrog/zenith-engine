#pragma once

#include "engine/application/SimulationClock.hpp"
#include "engine/module/SystemScheduler.hpp"
#include "engine/scene/SceneDocument.hpp"

#include <span>
#include <vector>

class AssetManager;

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
    // Runtime changes are intentionally discarded on stop. Applying selected
    // runtime state back requires a future explicit document command.
    SceneDocumentResult enter(Scene& activeScene, AssetManager& assets, const SceneDocumentOptions& options,
                              std::span<const SceneEntityId> selection, EditorRuntime runtime = EditorRuntime::Play);
    void pause();
    void resume();
    void requestSingleStep();
    void stop(Scene& activeScene, std::vector<SceneEntityId>& selection);

    EditorMode mode() const;
    // Whether the runtime scene (running or paused) was entered as Simulate.
    bool simulating() const;
    // Play or Simulate running, not paused.
    bool running() const;
    // The SimulationMode systems run in: Edit, Play, or Simulate.
    SimulationMode simulationMode() const;
    bool fixedUpdatesEnabled() const;
    bool beginFixedUpdate();

    // Scales simulated time in Play and Simulate (see SimulationClock); kept
    // across runs. Clamped to [0, 10]; returns the value kept.
    float setTimeScale(float scale);
    float timeScale() const;
    // Edit mode runs at wall-clock rate (animation previews and audio keep
    // playing); a runtime scene runs at the time scale, or not at all while
    // paused.
    SimulationClock simulationClock() const;

  private:
    Scene m_editScene;
    std::vector<SceneEntityId> m_editSelection;
    EditorMode m_mode{EditorMode::Edit};
    bool m_simulating{false};
    bool m_singleStepRequested{false};
    float m_timeScale{1.0f};
};
