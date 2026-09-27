#pragma once

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/application/EditorPlayMode.hpp"
#include "editor/application/EditorShortcuts.hpp"
#include "editor/document/CommandHistory.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/document/EditorRecovery.hpp"
#include "editor/interaction/EditorCamera.hpp"
#include "editor/project/AssetCatalog.hpp"
#include "editor/project/ThumbnailCache.hpp"
#include "editor/settings/EditorSettings.hpp"
#include "engine/application/Application.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// Owns editor-only state. The runtime Scene remains owned by Engine and is
// changed only by explicit document commands introduced by later milestones.
class EditorSession final : public ApplicationScene {
  public:
    EditorSession(std::filesystem::path settingsDirectory, std::filesystem::path assetRoot,
                  std::filesystem::path sceneRoot,
                  std::function<SceneResult(ApplicationContext&)> initialSceneLoader = {});

    SceneResult load(ApplicationContext& context) override;
    void update(ApplicationContext& context, const InputState& input, float deltaSeconds) override;
    void fixedUpdate(ApplicationContext& context, const InputState& input, float deltaSeconds) override;
    bool fixedUpdatesEnabled() const override;
    bool beginFixedUpdate() override;
    void requestQuit(ApplicationContext& context) override;
    void drawUi(ApplicationContext& context) override;
    void unload(ApplicationContext& context) override;

  private:
    enum class GizmoMode {
        Select,
        Translate,
        Rotate,
        Scale,
    };

    enum class TransformSpace {
        Global,
        Local,
    };

    void drawMainMenu(ApplicationContext& context);
    void drawPanels(ApplicationContext& context);
    void drawHierarchy(ApplicationContext& context);
    void drawViewport(ApplicationContext& context);
    bool drawViewportToolbar(glm::vec2 viewportPosition, glm::vec2 viewportSize);
    bool drawGizmo(ApplicationContext& context, glm::vec2 position, glm::vec2 size);
    void drawHierarchyEntity(ApplicationContext& context, Entity entity);
    void drawInspector(ApplicationContext& context);
    void drawAssetBrowser(ApplicationContext& context);
    void drawComponentControls(ApplicationContext& context, Entity entity);
    void drawStatusBar(const RenderStats& stats);
    void drawPathDialog(ApplicationContext& context);
    void drawUnsavedChangesDialog(ApplicationContext& context);
    void drawRecoveryDialog(ApplicationContext& context);
    void initializeDefaultLayout(uint32_t dockspaceId);
    void requestPathDialog(bool saveAs);
    void reportDiagnostic(std::string message);
    bool selected(SceneEntityId id) const;
    void select(SceneEntityId id, bool additive);
    void removeInvalidSelection(const Scene& scene);
    bool captureSnapshot(ApplicationContext& context, std::string& snapshot);
    void commitMutation(ApplicationContext& context, std::string name, std::string coalesceKey, std::string before);
    void performMutation(ApplicationContext& context, std::string name, std::string coalesceKey,
                         const std::function<SceneResult()>& mutation);
    EditorCommandContext commandContext(ApplicationContext& context);
    void syncDocumentDirtyState();
    void focusOnSelection(ApplicationContext& context);
    void refreshAssetCatalog();
    void instantiateModelAsset(ApplicationContext& context, const std::filesystem::path& path, Entity parent);
    void enterPlayMode(ApplicationContext& context);
    void stopPlayMode(ApplicationContext& context);
    void requestNew(ApplicationContext& context);
    void requestOpen(ApplicationContext& context);
    void requestOpenPath(ApplicationContext& context, std::filesystem::path path);
    void requestClose(ApplicationContext& context);
    void finishPendingAction(ApplicationContext& context);
    bool saveDocument(ApplicationContext& context);
    void copySelection(ApplicationContext& context);
    void pasteClipboard(ApplicationContext& context);
    void duplicateSelection(ApplicationContext& context);

    enum class PendingAction {
        None,
        NewDocument,
        OpenDocument,
        CloseEditor
    };

    EditorSettings m_settings;
    EditorDocument m_document;
    EditorRecovery m_recovery;
    EditorDiagnostics m_diagnostics;
    EditorShortcuts m_shortcuts;
    AssetCatalog m_assetCatalog;
    ThumbnailCache m_thumbnailCache;
    CommandHistory m_history;
    EditorCamera m_camera;
    EditorPlayMode m_playMode;
    std::function<SceneResult(ApplicationContext&)> m_initialSceneLoader;
    std::string m_layoutPath;
    std::string m_diagnostic;
    std::string m_clipboardFragment;
    std::array<char, 1024> m_pathBuffer{};
    bool m_openPathDialog{false};
    bool m_openUnsavedDialog{false};
    bool m_openRecoveryDialog{false};
    bool m_continuePendingAfterSaveAs{false};
    PendingAction m_pendingAction{PendingAction::None};
    std::optional<std::filesystem::path> m_pendingOpenPath;
    bool m_pathDialogSaves{false};
    bool m_initializeLayout{false};
    std::vector<SceneEntityId> m_selection;
    std::vector<RenderObjectId> m_outlinedObjects;
    std::vector<Entity> m_outlineTraversal;
    bool m_preserveWorldOnReparent{true};
    bool m_viewportInputActive{false};
    bool m_viewportNavigating{false};
    bool m_showGrid{true};
    GizmoMode m_gizmoMode{GizmoMode::Translate};
    TransformSpace m_transformSpace{TransformSpace::Global};
    bool m_gizmoSnapping{false};
    float m_translationSnap{0.5f};
    float m_rotationSnap{15.0f};
    float m_scaleSnap{0.1f};
    bool m_gizmoWasUsing{false};
    std::string m_gizmoBefore;
    std::string m_hierarchyFilter;
    bool m_hierarchyFilterFocused{false};
    std::string m_inspectorFilter;
    bool m_inspectorFilterFocused{false};
    std::string m_assetFilter;
    std::filesystem::path m_selectedAsset;
    float m_assetRefreshElapsed{0.0f};
    float m_autosaveElapsed{0.0f};
    bool m_assetCatalogInitialized{false};
    bool m_smokeScenario{false};
    uint32_t m_smokeFrame{0};
};
