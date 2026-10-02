#include "EditorPlayMode.hpp"

#include "engine/assets/Assets.hpp"

SceneDocumentResult EditorPlayMode::enter(Scene& activeScene, AssetManager& assets, const SceneDocumentOptions& options,
                                          std::span<const SceneEntityId> selection) {
    if (m_mode != EditorMode::Edit) {
        return {.error = "Play mode is already active"};
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
    m_mode = EditorMode::Play;
    return {.success = true};
}

void EditorPlayMode::pause() {
    if (m_mode == EditorMode::Play) {
        m_mode = EditorMode::Paused;
    }
}

void EditorPlayMode::resume() {
    if (m_mode == EditorMode::Paused) {
        m_mode = EditorMode::Play;
        m_singleStepRequested = false;
    }
}

void EditorPlayMode::requestSingleStep() {
    if (m_mode == EditorMode::Paused) {
        m_singleStepRequested = true;
    }
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
    m_singleStepRequested = false;
}

EditorMode EditorPlayMode::mode() const {
    return m_mode;
}

bool EditorPlayMode::fixedUpdatesEnabled() const {
    return m_mode == EditorMode::Play || m_singleStepRequested;
}

bool EditorPlayMode::beginFixedUpdate() {
    if (m_mode == EditorMode::Play) {
        return true;
    }
    if (m_mode == EditorMode::Paused && m_singleStepRequested) {
        m_singleStepRequested = false;
        return true;
    }
    return false;
}
