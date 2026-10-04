#pragma once

#include "editor/application/EditorAppearance.hpp"
#include "editor/application/EditorDiagnostics.hpp"
#include "editor/application/EditorPlayMode.hpp"
#include "editor/application/EditorSessionCommands.hpp"
#include "editor/application/NativeFileDialog.hpp"
#include "editor/application/NewCodeDialog.hpp"
#include "editor/application/PlayTargets.hpp"
#include "editor/application/SceneEditing.hpp"
#include "editor/application/SystemClipboard.hpp"
#include "editor/areas/AreaRegistry.hpp"
#include "editor/areas/Workspace.hpp"
#include "editor/document/CommandHistory.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/document/EditorRecovery.hpp"
#include "editor/interaction/ComponentEditorOverrides.hpp"
#include "editor/interaction/EntityEyedropper.hpp"
#include "editor/interaction/ModalTransform.hpp"
#include "editor/interaction/ProjectNames.hpp"
#include "editor/interaction/Selection.hpp"
#include "editor/interaction/WatchList.hpp"
#include "editor/menus/EditorMenus.hpp"
#include "editor/modules/EditorScriptInfo.hpp"
#include "editor/modules/GameModuleHost.hpp"
#include "editor/operators/EditorOperatorFamilies.hpp"
#include "editor/operators/Keymap.hpp"
#include "editor/operators/KeymapDispatch.hpp"
#include "editor/operators/KeymapEditor.hpp"
#include "editor/operators/LastOperatorPanel.hpp"
#include "editor/operators/OperatorRegistry.hpp"
#include "editor/operators/OperatorRunner.hpp"
#include "editor/operators/OperatorSearch.hpp"
#include "editor/panels/BuildPanel.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/panels/InfoPanel.hpp"
#include "editor/panels/PreferencesWindow.hpp"
#include "editor/panels/ProjectSettingsWindow.hpp"
#include "editor/panels/ViewportPanel.hpp"
#include "editor/project/PackageRunner.hpp"
#include "editor/project/ProjectAssets.hpp"
#include "editor/project/ProjectBuild.hpp"
#include "editor/project/ProjectHub.hpp"
#include "editor/settings/ComponentInspectorLayout.hpp"
#include "editor/settings/EditorRenderSettings.hpp"
#include "editor/settings/EditorSettings.hpp"
#include "editor/settings/RenderSettingsController.hpp"
#include "editor/smoke/SmokeRunner.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/ThemeChrome.hpp"
#include "engine/application/Application.hpp"
#include "engine/navigation/NavigationWorld.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Loads the scene the editor opens with into the context's scene, reading
// documents with the given options (which carry the game module's component
// types when the project has a module).
using EditorSceneLoader = std::function<SceneResult(ApplicationContext&, const SceneDocumentOptions&)>;

// What the session gets from the editor application about the open project
// beyond its folders.
struct EditorProjectServices {
    // The project's own build (a generated project's CMake preset), which the
    // Build panel runs while build.command and build.directory are unchanged.
    std::optional<BuildRequest> build;
    // Ends the session and shows the Project Hub; the session asks about
    // unsaved changes first. Empty when the editor runs without a hub.
    std::function<void(ApplicationContext&, ProjectHubEntry)> leaveForProjectHub;
    // The open project's file, which the Standalone play target passes to
    // the game; empty for an editor running without one.
    std::filesystem::path projectFile;
    // The C++ game template's code snippets, which the module.new_* operators
    // generate from; empty when the editor found no template.
    std::filesystem::path codeSnippetDirectory;
    // The project's tags, which the inspector offers for TagsComponent.
    std::vector<std::string> tags;
    // The project's input action overrides (loadProjectInputActions), and
    // why they could not be read; empty when the project has none.
    InputActionMap inputActions;
    std::string inputActionsError;
    // The project's collision layers, which the session gives the physics
    // world when it loads.
    PhysicsLayerTable physicsLayers{PhysicsLayerTable::defaults()};
    // The game executable and process factory for the Standalone target.
    EditorPlayServices play;
    // Told the session's mouse capture policy every frame, and
    // WhileRightMouseDown when the session unloads, so the application can
    // answer Application::mouseCapturePolicy (EditorSession::
    // mouseCapturePolicy). May be empty.
    std::function<void(MouseCapturePolicy)> mouseCaptureChanged;
};

// Composes the editor: owns the document, history, selection, camera, the
// panels that hold editor-wide state, the area types and workspaces that show
// panels, the operator registry and keymap, and the project's game module,
// runs the frame loop, and coordinates workflows that span panels (file
// commands, Play mode, module reloads, and modal transforms). Every menu item,
// button, and key runs a registered operator. The runtime Scene stays owned by
// Engine and is changed only through SceneEditing.
class EditorSession final : public ApplicationScene,
                            private EditorSessionActions,
                            private EditorDocumentCommands,
                            private EditorPlayCommands,
                            private EditorObjectModeCommands,
                            private EditorWindowCommands,
                            private EditorScriptInfo {
  public:
    // gameModule is the project's module library, or empty for none.
    // appearance is the application's and must outlive the session.
    EditorSession(std::filesystem::path settingsDirectory, std::filesystem::path assetRoot,
                  std::filesystem::path sceneRoot, EditorRenderSettings renderSettings, EditorAppearance& appearance,
                  EditorSceneLoader initialSceneLoader = {}, std::filesystem::path gameModule = {},
                  EditorProjectServices projectServices = {});
    // Area types and operators refer to the session's members by address.
    EditorSession(const EditorSession&) = delete;
    EditorSession& operator=(const EditorSession&) = delete;

    SceneResult load(ApplicationContext& context) override;
    void update(ApplicationContext& context, const InputState& input, float deltaSeconds) override;
    void fixedUpdate(ApplicationContext& context, const InputState& input, float deltaSeconds) override;
    void afterPhysicsStep(ApplicationContext& context) override;
    void lateUpdate(ApplicationContext& context, const InputState& input, float deltaSeconds) override;
    bool fixedUpdatesEnabled() const override;
    bool beginFixedUpdate() override;
    SimulationClock simulationClock() const override;
    void requestQuit(ApplicationContext& context) override;
    void drawUi(ApplicationContext& context) override;
    void unload(ApplicationContext& context) override;

    // Persistent while a game view (the play target's viewport) has game
    // input focus and the mouse over it (or the cursor captured), so a click
    // into it captures the cursor until Esc; otherwise WhileRightMouseDown
    // for editor camera flight.
    MouseCapturePolicy mouseCapturePolicy() const;

  private:
    // EditorSessionActions
    OperatorResult invokeOperator(ApplicationContext& context, std::string_view operatorId, std::string_view properties,
                                  KeymapRegion region) override;
    bool operatorAvailable(ApplicationContext& context, std::string_view operatorId) override;
    std::string operatorShortcut(std::string_view operatorId, std::string_view properties) const override;
    void requestOpenScene(ApplicationContext& context, std::filesystem::path path) override;
    void projectFilesMoved(ApplicationContext& context, std::span<const ProjectFileMove> moves) override;
    void projectFilesRemoved(ApplicationContext& context, std::span<const std::filesystem::path> removed) override;
    void handleObjectKeys(ApplicationContext& context, KeymapRegion region) override;
    bool modalTransformActive() const override;
    void updateModalTransform(ApplicationContext& context, ViewportId viewport) override;
    uint32_t dispatchMouseChords(ApplicationContext& context, KeymapRegion region, uint32_t allowedButtons) override;
    bool mouseButtonBound(KeymapRegion region, int button) const override;

    // EditorDocumentCommands
    bool documentCommandsAvailable(const OperatorContext& context) const override;
    void requestNew(ApplicationContext& context) override;
    void requestOpen(ApplicationContext& context) override;
    bool saveDocument(ApplicationContext& context) override;
    void requestSaveAs() override;
    void undo(ApplicationContext& context) override;
    void redo(ApplicationContext& context) override;
    OperatorResult copySelection(ApplicationContext& context) override;
    OperatorResult pasteClipboard(ApplicationContext& context) override;
    bool projectHubAvailable() const override;
    // Leaves the project for the Project Hub, asking about unsaved changes.
    void requestProjectHub(ApplicationContext& context, ProjectHubEntry entry) override;
    void requestClose(ApplicationContext& context) override;

    // EditorPlayCommands (EditorPlayback.cpp)
    bool standaloneRunning() const override;
    PlayTarget lastPlayTarget() const override;
    OperatorResult startPlay(ApplicationContext& context, PlayTarget target) override;
    void startSimulate(ApplicationContext& context) override;
    bool playActive() const override;
    // Leaves Play or Simulate, or stops the standalone game, and ends the
    // target's game view (closing a floating one).
    void stopPlay(ApplicationContext& context) override;
    ViewportPanel* gameViewport() const override;

    // EditorObjectModeCommands (EditorObjectCommands.cpp)
    bool modalTransformRunning() const override;
    bool beginModalTransform(ApplicationContext& context, TransformOperation operation, std::string commandName,
                             std::string before, bool commitOnCancel) override;
    void openDeleteMenu() override;
    void openRenamePopup(ApplicationContext& context) override;

    // EditorWindowCommands
    void openOperatorSearch() override;
    void openKeymapEditor() override;
    void workspacesChanged() override;
    void resetActiveWorkspace() override;
    void requestModuleReload() override;
    void openNewCodeDialog(GeneratedCodeKind kind) override;
    void openProjectSettings(ApplicationContext& context) override;

    // EditorScriptInfo (EditorModuleCommands.cpp)
    std::optional<EditorSourceLocation> behaviourSource(std::string_view componentKey) const override;
    ScriptStatus scriptStatus() const override;

    SceneEditing editing(ApplicationContext& context);
    EditorContext editorContext(ApplicationContext& context);
    OperatorContext operatorContext(ApplicationContext& context, KeymapRegion region);

    // What the operator families (editorOperatorFamilies) refer to.
    EditorOperatorServices operatorServices();
    OperatorResult invokeOperator(ApplicationContext& context, std::string_view operatorId,
                                  const OperatorProperties& properties);
    // Runs the binding of the first pressed chord bound in region.
    void dispatchKeymap(ApplicationContext& context, KeymapRegion region);
    // Runs the Hold, Drag, Release, and Click bindings of the followed press
    // (once per frame).
    void updateKeymapGesture(ApplicationContext& context);
    // A menu item showing the operator's shortcut, disabled when its poll fails.
    void operatorMenuItem(ApplicationContext& context, std::string_view operatorId, const char* label,
                          std::string_view properties = {}, bool selected = false);
    void drawOperatorWindows(ApplicationContext& context);
    void saveKeymap();
    void saveSettings();

    // Preferences and packaging (EditorPreferences.cpp).
    // Applies the appearance settings through the application's appearance
    // when they changed (art-pixel metrics, fonts for that size, colours) and
    // the theme tuning to the glass. Runs between frames, where fonts may be
    // added to the atlas.
    void applyAppearance();
    PreferencesContext preferencesContext(ApplicationContext& context);
    // Polls Build and Package; publishes its outcome and opens the output
    // folder when it succeeds.
    void updatePackaging();
    void drawBuildPackageMenu(ApplicationContext& context);
    void saveComponentLayoutIfChanged();
    void saveSimulateSystems();

    // Game module hosting.
    void loadGameModule();
    void updateGameModule(ApplicationContext& context, const InputState& input, float deltaSeconds);
    // Runs the module's Ui systems into m_gameViewUi for the game view.
    void drawGameViewUi(ApplicationContext& context);
    void reloadGameModule(ApplicationContext& context);
    void reportGameModule(const GameModuleHostResult& result);
    std::string gameModuleStatusText() const;
    void drawModuleMenu(ApplicationContext& context);

    // Areas and workspaces (EditorAreas.cpp).
    void addAreaType(AreaType type);
    void registerAreaTypes();
    // Registers an area type per game module panel and drops the types of
    // panels the module no longer has, then syncs the workspaces.
    void syncAreaTypes();
    void loadWorkspaces();
    void saveWorkspaces();
    // Between frames: applies a requested workspace switch and saves
    // workspaces.json when the layout or areas changed.
    void updateWorkspaces();
    void updateAreas(ApplicationContext& context, const InputState& input, float deltaSeconds);
    void drawAreas(ApplicationContext& context);
    void drawAreaMenuItems(ApplicationContext& context);
    void drawWorkspaceTabs(ApplicationContext& context);

    // The editor smoke (EditorSessionSmoke.cpp; the steps are in
    // editor/smoke/): builds the scenario when ZENITH_EDITOR_SMOKE is set,
    // and runs it once per frame, quitting when it passes.
    class SmokeHooks;
    SceneResult prepareSmokeScenario(ApplicationContext& context);
    void updateSmokeScenario(ApplicationContext& context);
    // What the editor does when project files change; dispatched by m_projectAssets.
    ProjectAssetActions projectAssetActions(ApplicationContext& context);
    void updateAutosave(ApplicationContext& context, float deltaSeconds);
    void drawMainMenu(ApplicationContext& context);
    void handleGlobalShortcuts(ApplicationContext& context);
    void drawPanels(ApplicationContext& context);
    void drawStatusBar(const RenderStats& stats);
    void drawPlayModePill() const;
    // Cancels the entity eyedropper on Esc, a right-click, Play mode, or a
    // target that no longer exists, and shows its hint at the mouse.
    void updateEyedropper(ApplicationContext& context);
    void drawPathDialog(ApplicationContext& context);
    void drawUnsavedChangesDialog(ApplicationContext& context);
    void drawRecoveryDialog(ApplicationContext& context);
    void requestFileDialog(FileDialogKind kind);
    void openPathDialog(FileDialogKind kind);
    void pollFileDialog(ApplicationContext& context);
    bool completeOpen(ApplicationContext& context, const std::filesystem::path& path);
    bool completeSaveAs(ApplicationContext& context, const std::filesystem::path& path);
    void onDocumentPathCommitted();
    // Undoes the steps recorded since marker, so a smoke step that took a
    // marker before editing undoes exactly its own steps.
    void undoSince(ApplicationContext& context, CommandHistoryMarker marker);
    // Runs the document on a throwaway copy as Play or Simulate; false (after
    // reporting why) when it could not.
    bool enterPlayMode(ApplicationContext& context, EditorRuntime runtime = EditorRuntime::Play);
    void stopPlayMode(ApplicationContext& context);

    // Play targets, game views, the standalone game, and the playback
    // toolbar (EditorPlayback.cpp).
    OperatorResult startStandalone(ApplicationContext& context);
    void endGameView();
    // The input module systems get: the frame's input while a game view has
    // focus (or Play runs without one), otherwise none.
    const InputState& gameInput(const InputState& input) const;
    // Streams the standalone game's output and leaves Play when it exits.
    void updateStandalone(ApplicationContext& context);
    // Tells the application the mouse capture policy, once per frame.
    void publishMouseCapture();
    void drawPlaybackToolbar(ApplicationContext& context);
    // Slot in the toolbar's Simulate dropdown for the Simulate system picker
    // (Unit 18); shows a pointer to the System Profiler until then.
    void drawSimulateOptions(ApplicationContext& context);
    void finishPendingAction(ApplicationContext& context);

    // Blender object-mode workflows (EditorObjectCommands.cpp).
    void drawObjectPopups(ApplicationContext& context);
    // The New Behaviour, Component, and System dialog (EditorModuleCommands.cpp).
    void drawNewCodeDialog(ApplicationContext& context);
    // Project Settings, and applying what it saves (EditorProjectSettings.cpp).
    void drawProjectSettings(ApplicationContext& context);
    std::string applyProjectSettings(ApplicationContext& context, const ProjectSettingsValues& values);
    void finishModalTransform(ApplicationContext& context, bool confirm);
    void drawModalTransformOverlay(const ViewportPanel& viewport, const ModalTransformResult& result) const;

    // Viewports (EditorViewports.cpp).
    // Start of a frame: settles which viewports are open and cancels a modal
    // transform whose viewport closed.
    void beginViewportFrame(ApplicationContext& context);
    // After the areas are drawn: shadows and streaming follow the active
    // viewport's render view.
    void syncActiveRenderView(ApplicationContext& context);
    // The bottom-left corner of the active viewport's image, or of the work
    // area when no viewport is open.
    glm::vec2 lastOperatorAnchor() const;

    enum class PendingAction {
        None,
        NewDocument,
        OpenDocument,
        CloseEditor,
        LeaveForProjectHub
    };

    EditorSettings m_settings;
    EditorDocument m_document;
    EditorRecovery m_recovery;
    EditorDiagnostics m_diagnostics;
    // The game module's build: started by build.run (Ctrl B), the Build
    // panel, or build on save, and polled every frame. Declared before the
    // operators that refer to it.
    ProjectBuild m_projectBuild{m_settings.tools(), m_diagnostics};
    // The fields the Properties panel watches; declared before the operators
    // that edit it.
    WatchList m_watches;
    CommandHistory m_history;
    EditorPlayMode m_playMode;
    // The target F5 (play.start without a target) runs: the last one used.
    PlayTarget m_lastPlayTarget{PlayTarget::ActiveViewport};
    // The running target, while Play or Simulate runs or the standalone game
    // does; Simulate always uses its editor cameras (ActiveViewport, with no
    // game view).
    std::optional<PlayTarget> m_activePlayTarget;
    // The viewport showing the game; empty without a game view.
    ViewportId m_gameViewport;
    // The floating target's area and the workspace it was opened in.
    std::string m_floatingPlayArea;
    std::string m_floatingPlayWorkspace;
    // What module systems get while the game view lacks focus.
    InputState m_idleGameInput;
    // The input the frame's Update systems saw, for its LateUpdate and Ui
    // systems; idle outside Play.
    InputState m_frameGameInput;
    // What the module's Ui systems draw over the game view, sized to it.
    GameUiContext m_gameViewUi;
    std::filesystem::path m_projectFile;
    std::filesystem::path m_codeSnippetDirectory;
    PhysicsLayerTable m_physicsLayers;
    // The project's layer and tag names the inspector offers.
    EditorProjectNames m_projectNames;
    EditorPlayServices m_playServices;
    StandalonePlayer m_standalone;
    std::function<void(MouseCapturePolicy)> m_mouseCaptureChanged;
    NativeFileDialog m_fileDialog;
    Selection m_selection;
    // Declared before the operator registry, whose eyedropper operators
    // refer to it.
    EntityEyedropper m_eyedropper;
    // Menus and the open popup menu; declared before the operators that
    // open them.
    EditorMenus m_menus;
    OperatorRegistry m_operators;
    OperatorRunner m_operatorRunner;
    // Every viewport area registers here; declared before the areas, which
    // own the viewport panels, and before the module host, which adds the
    // module's sidebar tabs, so it outlives both.
    ViewportRegistry m_viewports;
    // Inspector overrides from the game module, shared by the Inspector and
    // Properties areas.
    ComponentEditorOverrides m_componentEditors;
    // Declared after the registries it adds the module's editor extensions
    // to, which it removes them from when destroyed.
    GameModuleHost m_moduleHost{{.operators = m_operators,
                                 .runner = m_operatorRunner,
                                 .menus = m_menus.registry(),
                                 .viewports = m_viewports,
                                 .componentEditors = m_componentEditors}};
    std::filesystem::path m_gameModulePath;
    // A reload waits for Edit mode, a finished build, and no modal tool.
    bool m_moduleReloadPending{false};
    // The user's Simulate system choices (SimulateSystemSettings).
    std::filesystem::path m_simulateSystemsPath;
    BuildState m_lastBuildState{BuildState::Idle};
    Keymap m_keymap;
    std::filesystem::path m_keymapPath;
    // Turns key and mouse input into bindings to run, following a press for
    // its Hold, Release, Click, and Drag bindings.
    KeymapDispatcher m_keymapDispatcher;
    OperatorSearch m_operatorSearch;
    KeymapEditor m_keymapEditor;
    LastOperatorPanel m_lastOperatorPanel;
    bool m_showKeymapEditor{false};
    PreferencesWindow m_preferences;
    ProjectSettingsWindow m_projectSettings;
    // The application's look on ImGui, which outlives the session; the
    // session's preference changes go through it, so the Project Hub shows
    // them after the session ends.
    EditorAppearance& m_appearance;
    // The project's asset service, shared by every Asset Browser area;
    // declared before the areas that reference it.
    ProjectAssets m_projectAssets;

    // Shared by the Graphics and Properties areas, so declared before them.
    RenderSettingsController m_renderSettings;
    // Component section order and collapse state, shared by the Inspector
    // and Properties areas and the component operators, so declared before
    // them; saved to component-inspector.json whenever it changes.
    ComponentInspectorLayout m_componentLayout;
    std::filesystem::path m_componentLayoutPath;
    // Where the component copy and paste operators exchange values.
    SystemClipboard m_componentClipboard;
    // Panels holding editor-wide state, shown by single-instance area types.
    // Per-area panels (Viewport, Hierarchy, Asset Browser, Graphics, ...) are
    // created by their types (EditorAreaTypes.hpp, EditorAreas.cpp).
    // Build and Package; declared before the Build panel, which shows it.
    PackageRunner m_packageRunner{makeProcessPackageStepRunner()};
    // The preview navigation mesh navigation.bake builds and the viewports draw.
    NavigationWorld m_navigationPreview;
    BuildPanel m_buildPanel{m_settings.tools(), m_projectBuild, &m_packageRunner};
    // Shows the engine log, diagnostics, and operator history.
    InfoPanel m_infoPanel{m_operatorRunner.history()};

    // Declared after everything area panels refer to (the panels above, the
    // settings, and the module host), so the areas are destroyed first.
    // Registration order is the View menu order.
    AreaRegistry m_areaTypes;
    WorkspaceSet m_workspaces;
    std::filesystem::path m_workspacesPath;
    // layout.ini from editors before workspaces.json, read once to migrate.
    std::filesystem::path m_legacyLayoutPath;
    // The active workspace has no saved layout; build its default one when
    // the next frame's dockspace exists.
    bool m_buildDefaultLayout{false};
    bool m_workspacesDirty{false};

    std::filesystem::path m_sceneRoot;
    // The wallpaper and glass behind every window.
    ThemeChrome m_themeChrome;
    EditorSceneLoader m_initialSceneLoader;
    std::string m_clipboardFragment;
    std::array<char, 1024> m_pathBuffer{};
    bool m_openPathDialog{false};
    bool m_openUnsavedDialog{false};
    bool m_openRecoveryDialog{false};
    bool m_continuePendingAfterSaveAs{false};
    PendingAction m_pendingAction{PendingAction::None};
    ProjectHubEntry m_pendingHubEntry{ProjectHubEntry::Hub};
    std::function<void(ApplicationContext&, ProjectHubEntry)> m_leaveForProjectHub;
    std::optional<std::filesystem::path> m_pendingOpenPath;
    bool m_pathDialogSaves{false};
    float m_autosaveElapsed{0.0f};

    // Where the smoke steps write their files: the settings directory, which
    // automated runs give their own.
    std::filesystem::path m_smokeScratchDirectory;
    // The smoke scenario; only while ZENITH_EDITOR_SMOKE runs it.
    std::unique_ptr<SmokeRunner> m_smokeRunner;

    struct ActiveTransform {
        ModalTransform transform;
        // The viewport the transform started in; it alone advances and draws
        // the transform, which is cancelled if the viewport closes.
        ViewportId viewport;
        std::vector<SceneEntityId> entities;
        std::vector<glm::mat4> startLocal;
        // Scene snapshot from before the command, for undo.
        std::string before;
        std::string commandName;
        // Shift D keeps the duplicate even when its move is cancelled.
        bool commitOnCancel{false};
    };

    std::optional<ActiveTransform> m_activeTransform;
    // Set when a modal transform ends, until the next frame starts.
    bool m_modalTransformFinishedThisFrame{false};
    bool m_openDeleteMenu{false};
    bool m_openRenamePopup{false};
    NewCodeDialog m_newCodeDialog;
    bool m_focusRenameField{false};
    std::string m_renameBuffer;
};
