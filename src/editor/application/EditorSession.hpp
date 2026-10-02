#pragma once

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/application/EditorPlayMode.hpp"
#include "editor/application/NativeFileDialog.hpp"
#include "editor/application/SceneEditing.hpp"
#include "editor/document/CommandHistory.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/document/EditorRecovery.hpp"
#include "editor/interaction/EditorCamera.hpp"
#include "editor/interaction/ModalTransform.hpp"
#include "editor/interaction/Selection.hpp"
#include "editor/modules/GameModuleHost.hpp"
#include "editor/operators/Keymap.hpp"
#include "editor/operators/KeymapEditor.hpp"
#include "editor/operators/LastOperatorPanel.hpp"
#include "editor/operators/OperatorRegistry.hpp"
#include "editor/operators/OperatorRunner.hpp"
#include "editor/operators/OperatorSearch.hpp"
#include "editor/panels/AssetBrowserPanel.hpp"
#include "editor/panels/BuildPanel.hpp"
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
#include <string_view>
#include <vector>

// Loads the scene the editor opens with into the context's scene, reading
// documents with the given options (which carry the game module's component
// types when the project has a module).
using EditorSceneLoader = std::function<SceneResult(ApplicationContext&, const SceneDocumentOptions&)>;

// Composes the editor: owns the document, history, selection, camera, panels,
// the operator registry and keymap, and the project's game module, runs the
// frame loop, and coordinates workflows that span panels (file commands, Play
// mode, module reloads, and modal transforms). Every menu item, button, and
// key runs a registered operator. The runtime Scene stays owned by Engine and
// is changed only through SceneEditing.
class EditorSession final : public ApplicationScene, private EditorSessionActions {
  public:
    // gameModule is the project's module library, or empty for none.
    EditorSession(std::filesystem::path settingsDirectory, std::filesystem::path assetRoot,
                  std::filesystem::path sceneRoot, std::filesystem::path fontPath, EditorRenderSettings renderSettings,
                  EditorSceneLoader initialSceneLoader = {}, std::filesystem::path gameModule = {});
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
    OperatorResult invokeOperator(ApplicationContext& context, std::string_view operatorId, std::string_view properties,
                                  KeymapRegion region) override;
    bool operatorAvailable(ApplicationContext& context, std::string_view operatorId) override;
    std::string operatorShortcut(std::string_view operatorId, std::string_view properties) const override;
    void requestOpenScene(ApplicationContext& context, std::filesystem::path path) override;
    void handleObjectKeys(ApplicationContext& context, KeymapRegion region) override;
    bool modalTransformActive() const override;
    void updateModalTransform(ApplicationContext& context) override;

    SceneEditing editing(ApplicationContext& context);
    EditorContext editorContext(ApplicationContext& context);
    OperatorContext operatorContext(ApplicationContext& context, KeymapRegion region);

    // Operators (session operators in EditorSession.cpp, object-mode ones in
    // EditorObjectCommands.cpp).
    // An operator that runs a session workflow. It needs the running
    // application, so it polls false in headless contexts.
    static Operator applicationOperator(std::string id, std::string label, std::string description,
                                        std::function<bool(const OperatorContext&)> poll,
                                        std::function<void(ApplicationContext&)> run);
    void registerOperator(Operator op);
    void registerSessionOperators();
    void registerObjectModeOperators();
    void registerModuleOperators();
    bool documentCommandsAvailable(const OperatorContext& context) const;
    OperatorResult invokeOperator(ApplicationContext& context, std::string_view operatorId,
                                  const OperatorProperties& properties);
    // Runs the binding of the first pressed chord bound in region.
    void dispatchKeymap(ApplicationContext& context, KeymapRegion region);
    // A menu item showing the operator's shortcut, disabled when its poll fails.
    void operatorMenuItem(ApplicationContext& context, std::string_view operatorId, const char* label,
                          std::string_view properties = {}, bool selected = false);
    void drawOperatorWindows(ApplicationContext& context);
    void saveKeymap();

    // Game module hosting.
    void loadGameModule();
    void updateGameModule(ApplicationContext& context, const InputState& input, float deltaSeconds);
    void reloadGameModule(ApplicationContext& context);
    void reportGameModule(const GameModuleHostResult& result);
    std::string gameModuleStatusText() const;
    void drawModuleMenu(ApplicationContext& context);
    void drawModulePanels(ApplicationContext& context);

    SceneResult prepareSmokeScenario(ApplicationContext& context);
    void updateSmokeScenario(ApplicationContext& context);
    void updateSmokeModuleReload(ApplicationContext& context);
    void releaseSmokeLayers();
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
    OperatorResult copySelection(ApplicationContext& context);
    // Pastes without recording undo; edit.paste is an Undo operator.
    OperatorResult pasteClipboard(ApplicationContext& context);

    // Blender object-mode workflows (EditorObjectCommands.cpp).
    void drawObjectPopups(ApplicationContext& context);
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
    CommandHistory m_history;
    EditorCamera m_camera;
    EditorPlayMode m_playMode;
    NativeFileDialog m_fileDialog;
    Selection m_selection;
    OperatorRegistry m_operators;
    OperatorRunner m_operatorRunner;
    // Declared after the operator registry and runner, which it removes the
    // module's operators from when destroyed.
    GameModuleHost m_moduleHost{m_operators, m_operatorRunner};
    std::filesystem::path m_gameModulePath;
    // A reload waits for Edit mode, a finished build, and no modal tool.
    bool m_moduleReloadPending{false};
    BuildState m_lastBuildState{BuildState::Idle};
    Keymap m_keymap;
    std::filesystem::path m_keymapPath;
    OperatorSearch m_operatorSearch;
    KeymapEditor m_keymapEditor;
    LastOperatorPanel m_lastOperatorPanel;
    bool m_showKeymapEditor{false};

    ViewportPanel m_viewport;
    HierarchyPanel m_hierarchy;
    InspectorPanel m_inspector;
    DiagnosticsPanel m_diagnosticsPanel{m_settings.tools()};
    RenderStatisticsPanel m_renderStatistics;
    AssetBrowserPanel m_assetBrowser;
    GraphicsPanel m_graphics;
    BuildPanel m_buildPanel{m_settings.tools()};
    // Drawing order, which is also the View menu order.
    std::array<PanelSlot, 8> m_panels;

    std::filesystem::path m_sceneRoot;
    std::filesystem::path m_fontPath;
    EditorSceneLoader m_initialSceneLoader;
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
    bool m_smokeModuleReloadRequested{false};

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
    bool m_openAddMenu{false};
    bool m_openDeleteMenu{false};
    bool m_openRenamePopup{false};
    bool m_focusRenameField{false};
    std::string m_renameBuffer;
};
