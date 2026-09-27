#pragma once

#include "engine/scene/SceneDocument.hpp"

#include <span>
#include <vector>

class AssetManager;

enum class EditorMode {
    Edit,
    Play,
    Paused,
};

class EditorPlayMode {
  public:
    // Runtime changes are intentionally discarded on stop. Applying selected
    // runtime state back requires a future explicit document command.
    SceneDocumentResult enter(Scene& activeScene, AssetManager& assets, const SceneDocumentOptions& options,
                              std::span<const SceneEntityId> selection);
    void pause();
    void resume();
    void requestSingleStep();
    void stop(Scene& activeScene, std::vector<SceneEntityId>& selection);

    EditorMode mode() const;
    bool fixedUpdatesEnabled() const;
    bool beginFixedUpdate();

  private:
    Scene m_editScene;
    std::vector<SceneEntityId> m_editSelection;
    EditorMode m_mode{EditorMode::Edit};
    bool m_singleStepRequested{false};
};
