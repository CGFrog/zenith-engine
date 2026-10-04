#include "EditorPlayMode.hpp"

#include "engine/assets/Assets.hpp"

SceneDocumentResult EditorPlayMode::enter(Scene& activeScene, AssetManager& assets, const SceneDocumentOptions& options,
                                          std::span<const SceneEntityId> selection, EditorRuntime runtime) {
    if (m_mode != EditorMode::Edit) {
        return {.error = m_simulating ? "Simulate mode is already active" : "Play mode is already active"};
    }
    std::string snapshot;
    if (SceneDocumentResult result = SceneDocument::serialize(activeScene, assets, options, snapshot); !result) {
        return result;
    }

    activeScene.swap(m_editScene);
    if (SceneDocumentResult result = SceneDocument::deserialize(snapshot, assets, options, activeScene); !result) {
        activeScene.swap(m_editScene);
        return result;
    }
    m_editSelection.assign(selection.begin(), selection.end());
    m_simulating = runtime == EditorRuntime::Simulate;
    m_mode = m_simulating ? EditorMode::Simulate : EditorMode::Play;
    return {.success = true};
}

void EditorPlayMode::pause() {
    if (running()) {
        m_mode = EditorMode::Paused;
    }
}

void EditorPlayMode::resume() {
    if (m_mode == EditorMode::Paused) {
        m_mode = m_simulating ? EditorMode::Simulate : EditorMode::Play;
        m_frameStepSeconds = 0.0f;
    }
}

void EditorPlayMode::requestFrameStep(float stepSeconds) {
    if (m_mode == EditorMode::Paused && stepSeconds > 0.0f) {
        m_frameStepSeconds = stepSeconds;
    }
}

void EditorPlayMode::finishFrame() {
    m_frameStepSeconds = 0.0f;
}

void EditorPlayMode::stop(Scene& activeScene, std::vector<SceneEntityId>& selection) {
    if (m_mode == EditorMode::Edit) {
        return;
    }
    activeScene.swap(m_editScene);
    // Drops the runtime registry entirely rather than clearing it: its storage
    // for game module component types was created by module code, which a
    // module reload unloads.
    m_editScene.replaceWith(Scene{});
    selection = std::move(m_editSelection);
    m_editSelection.clear();
    m_mode = EditorMode::Edit;
    m_simulating = false;
    m_frameStepSeconds = 0.0f;
}

EditorMode EditorPlayMode::mode() const {
    return m_mode;
}

bool EditorPlayMode::simulating() const {
    return m_simulating;
}

bool EditorPlayMode::running() const {
    return m_mode == EditorMode::Play || m_mode == EditorMode::Simulate;
}

SimulationMode EditorPlayMode::simulationMode() const {
    if (m_mode == EditorMode::Edit) {
        return SimulationMode::Edit;
    }
    return m_simulating ? SimulationMode::Simulate : SimulationMode::Play;
}

bool EditorPlayMode::steppingFrame() const {
    return m_mode == EditorMode::Paused && m_frameStepSeconds > 0.0f;
}

bool EditorPlayMode::advancesThisFrame() const {
    return running() || steppingFrame();
}

bool EditorPlayMode::fixedUpdatesEnabled() const {
    return advancesThisFrame();
}

bool EditorPlayMode::beginFixedUpdate() {
    // A frame step's fixed steps are bounded by what its clock accumulates.
    return advancesThisFrame();
}

float EditorPlayMode::setTimeScale(float scale) {
    m_timeScale = clampTimeScale(scale);
    return m_timeScale;
}

float EditorPlayMode::timeScale() const {
    return m_timeScale;
}

SimulationClock EditorPlayMode::simulationClock() const {
    if (m_mode == EditorMode::Edit) {
        return {};
    }
    return {.timeScale = m_timeScale, .paused = m_mode == EditorMode::Paused, .frameStepSeconds = m_frameStepSeconds};
}

Scene* EditorPlayMode::editScene() {
    return m_mode == EditorMode::Edit ? nullptr : &m_editScene;
}
