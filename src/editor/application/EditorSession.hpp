#pragma once

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/application/EditorPlayMode.hpp"
#include "editor/application/EditorShortcuts.hpp"
#include "editor/application/NativeFileDialog.hpp"
#include "editor/application/SceneEditing.hpp"
#include "editor/document/CommandHistory.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/document/EditorRecovery.hpp"
#include "editor/interaction/EditorCamera.hpp"
#include "editor/interaction/ModalTransform.hpp"
#include "editor/interaction/ObjectKeymap.hpp"
#include "editor/interaction/Selection.hpp"
#include "editor/panels/AssetBrowserPanel.hpp"
#include "editor/panels/DiagnosticsPanel.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/panels/GraphicsPanel.hpp"
#include "editor/panels/HierarchyPanel.hpp"
#include "editor/panels/InspectorPanel.hpp"
#include "editor/panels/RenderStatisticsPanel.hpp"
#include "editor/panels/ViewportPanel.hpp"
#include "editor/settings/EditorRenderSettings.hpp"
#include "editor/settings/EditorSettings.hpp"
#include "engine/application/Application.hpp"
#include "engine/jobs/JobSystem.hpp"
#include "engine/scene/AdditiveSceneManager.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Composes the editor: owns the document, history, selection, camera, and
// panels, runs the frame loop, and coordinates workflows that span panels
// (file commands, Play mode, object-mode keys, and modal transforms). The
// runtime Scene stays owned by Engine and is changed only through SceneEditing.
class EditorSession final : public ApplicationScene, private EditorSessionActions {
  public:
    EditorSession(std::filesystem::path settingsDirectory, std::filesystem::path assetRoot,
                  std::filesystem::path sceneRoot, std::filesystem::path fontPath, EditorRenderSettings renderSettings,
                  std::function<SceneResult(ApplicationContext&)> initialSceneLoader = {});
    // Panels are referenced by address from the panel table.
    EditorSession(const EditorSession&) = delete;
    EditorSession& operator=(const EditorSession&) = delete;

    SceneResult load(ApplicationContext& context) override;
    void update(ApplicationContext& context, const InputState& input, float deltaSeconds) override;
    void fixedUpdate(ApplicationContext& context, const InputState& input, float deltaSeconds) override;
    bool fixedUpdatesEnabled() const override;
    bool beginFixedUpdate() override;
    void requestQuit(ApplicationContext& context) override;
    void drawUi(ApplicationContext& context) override;
    void unload(ApplicationContext& context) override;

  private:
    struct PanelSlot {
        EditorPanel* panel;
        bool EditorPanelVisibility::* visible;
    };

    // EditorSessionActions
    void requestOpenScene(ApplicationContext& context, std::filesystem::path path) override;
    void handleObjectKeys(ApplicationContext& context, KeymapRegion region) override;
    bool modalTransformActive() const override;
    void updateModalTransform(ApplicationContext& context) override;

    SceneEditing editing(ApplicationContext& context);
    EditorContext editorContext(ApplicationContext& context);

    SceneResult prepareSmokeScenario(ApplicationContext& context);
    void updateSmokeScenario(ApplicationContext& context);
    void updateAutosave(ApplicationContext& context, float deltaSeconds);
    void drawMainMenu(ApplicationContext& context);
    void handleGlobalShortcuts(ApplicationContext& context);
    void drawPanels(ApplicationContext& context);
    void drawStatusBar(const RenderStats& stats);
    void drawPathDialog(ApplicationContext& context);
    void drawUnsavedChangesDialog(ApplicationContext& context);
    void drawRecoveryDialog(ApplicationContext& context);
    void initializeDefaultLayout(uint32_t dockspaceId);
    void requestFileDialog(FileDialogKind kind);
    void openPathDialog(FileDialogKind kind);
    void pollFileDialog(ApplicationContext& context);
    bool completeOpen(ApplicationContext& context, const std::filesystem::path& path);
    bool completeSaveAs(ApplicationContext& context, const std::filesystem::path& path);
    void onDocumentPathCommitted();
    void undo(ApplicationContext& context);
    void redo(ApplicationContext& context);
    void enterPlayMode(ApplicationContext& context);
    void stopPlayMode(ApplicationContext& context);
    void requestNew(ApplicationContext& context);
    void requestOpen(ApplicationContext& context);
    void requestClose(ApplicationContext& context);
    void finishPendingAction(ApplicationContext& context);
    bool saveDocument(ApplicationContext& context);
    void copySelection(ApplicationContext& context);
    void pasteClipboard(ApplicationContext& context);

    // Blender object-mode commands (EditorObjectCommands.cpp).
    enum class AddableObject {
        Cube,
        Sun,
        Environment,
        Empty,
    };
    void runObjectCommand(ApplicationContext& context, ObjectCommand command, KeymapRegion region);
    void drawObjectPopups(ApplicationContext& context);
    void clearSelectedTransforms(ApplicationContext& context, TransformPart part);
    void addObject(ApplicationContext& context, AddableObject object);
    void frameEntities(ApplicationContext& context, const std::vector<SceneEntityId>& entities);
    void frameAll(ApplicationContext& context);
    bool beginModalTransform(ApplicationContext& context, TransformOperation operation, std::string commandName,
                             std::string before, bool commitOnCancel);
    void finishModalTransform(ApplicationContext& context, bool confirm);
    void drawModalTransformOverlay(const ModalTransformResult& result) const;

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
    CommandHistory m_history;
    EditorCamera m_camera;
    EditorPlayMode m_playMode;
    NativeFileDialog m_fileDialog;
    Selection m_selection;

    ViewportPanel m_viewport;
    HierarchyPanel m_hierarchy;
    InspectorPanel m_inspector;
    DiagnosticsPanel m_diagnosticsPanel;
    RenderStatisticsPanel m_renderStatistics;
    AssetBrowserPanel m_assetBrowser;
    GraphicsPanel m_graphics;
    // Drawing order, which is also the View menu order.
    std::array<PanelSlot, 7> m_panels;

    std::filesystem::path m_sceneRoot;
    std::filesystem::path m_fontPath;
    std::function<SceneResult(ApplicationContext&)> m_initialSceneLoader;
    std::string m_layoutPath;
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
    float m_autosaveElapsed{0.0f};

    bool m_smokeScenario{false};
    uint32_t m_smokeFrame{0};
    std::filesystem::path m_smokeScenePath;
    std::unique_ptr<JobSystem> m_smokeJobs;
    std::unique_ptr<AdditiveSceneManager> m_smokeLayers;
    std::optional<SceneInstanceId> m_smokeStreamInstance;
    bool m_smokeStreamCompleted{false};

    struct ActiveTransform {
        ModalTransform transform;
        std::vector<SceneEntityId> entities;
        std::vector<glm::mat4> startLocal;
        // Scene snapshot from before the command, for undo.
        std::string before;
        std::string commandName;
        // Shift D keeps the duplicate even when its move is cancelled.
        bool commitOnCancel{false};
    };

    std::optional<ActiveTransform> m_activeTransform;
    double m_lastSelectAllTime{-1.0};
    bool m_openAddMenu{false};
    bool m_openDeleteMenu{false};
    bool m_openRenamePopup{false};
    bool m_focusRenameField{false};
    std::string m_renameBuffer;
};
