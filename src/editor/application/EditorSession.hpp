#pragma once

// EditorSession: the per-project editor coordinator (document, history, selection, panels, Play, module host); its
// workflows live in Editor*.cpp.

#include "editor/application/DocumentBinding.hpp"
#include "editor/application/EditorAppearance.hpp"
#include "editor/application/EditorDiagnostics.hpp"
#include "editor/application/EditorPlayMode.hpp"
#include "editor/application/EditorPlayback.hpp"
#include "editor/application/EditorSessionCommands.hpp"
#include "editor/application/FileDropQueue.hpp"
#include "editor/application/NativeFileDialog.hpp"
#include "editor/application/NewCodeDialog.hpp"
#include "editor/application/PlayTargets.hpp"
#include "editor/application/PrefabWindowSet.hpp"
#include "editor/application/SceneEditing.hpp"
#include "editor/application/SystemClipboard.hpp"
#include "editor/areas/AreaRegistry.hpp"
#include "editor/areas/Workspace.hpp"
#include "editor/document/CommandHistory.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/document/EditorRecovery.hpp"
#include "editor/document/PrefabWindowRecovery.hpp"
#include "editor/document/PrefabWriteCommand.hpp"
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
#include "engine/platform/CrashHandler.hpp"

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
    // How the project bakes navigation meshes: navigation.bake's preview and
    // Play's sessions (GameServices::setDefaultNavigationSettings).
    NavMeshBuildSettings navigation;
    // The game executable and process factory for the Standalone target.
    EditorPlayServices play;
    // Told the session's mouse capture policy every frame, and
    // WhileRightMouseDown when the session unloads, so the application can
    // answer Application::mouseCapturePolicy (EditorSession::
    // mouseCapturePolicy). May be empty.
    std::function<void(MouseCapturePolicy)> mouseCaptureChanged;
    // Given the session's emergency recovery save (EditorSession::
    // saveRecoveryAfterCrash) once it runs, and an empty action when it
    // unloads, so the application can hand it to the process's crash handler
    // (CrashHandlerScope::setRecoveryAction). May be empty.
    std::function<void(CrashRecoveryAction)> crashRecoveryChanged;
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
                            private EditorObjectModeCommands,
                            private EditorWindowCommands,
                            private EditorPrefabWindowCommands,
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
    bool editCommandsAvailable(const OperatorContext& context) const override;
    bool saveAsAvailable(const OperatorContext& context) const override;
    void requestNew(ApplicationContext& context) override;
    void requestOpen(ApplicationContext& context) override;
    bool saveDocument(OperatorContext& context) override;
    void requestSaveAs() override;
    void undo(OperatorContext& context) override;
    void redo(OperatorContext& context) override;
    OperatorResult copySelection(OperatorContext& context) override;
    OperatorResult pasteClipboard(OperatorContext& context) override;
    bool projectHubAvailable() const override;
    // Leaves the project for the Project Hub, asking about unsaved changes.
    void requestProjectHub(ApplicationContext& context, ProjectHubEntry entry) override;
    void requestClose(ApplicationContext& context) override;

    // EditorObjectModeCommands (EditorObjectCommands.cpp)
    bool modalTransformRunning() const override;
    bool viewportsShowDocument(const OperatorContext& context) const override;
    bool beginModalTransform(OperatorContext& context, TransformOperation operation, std::string commandName,
                             std::optional<SceneEditing::EntitySnapshot> before, bool commitOnCancel) override;
    void openDeleteMenu() override;
    void openRenamePopup(OperatorContext& context) override;

    // EditorWindowCommands
    void openOperatorSearch() override;
    void openKeymapEditor() override;
    void workspacesChanged() override;
    void resetActiveWorkspace() override;
    void requestModuleReload() override;
    void openNewCodeDialog(GeneratedCodeKind kind) override;
    void openProjectSettings(ApplicationContext& context, std::string_view tab) override;

    // EditorPrefabWindowCommands (EditorPrefabWindows.cpp)
    bool prefabCloseAvailable() const override;
    bool prefabEditAvailable() const override;
    OperatorResult editPrefab(ApplicationContext& context, const std::filesystem::path& reference) override;
    void requestCloseFocusedPrefab(ApplicationContext& context) override;

    // EditorScriptInfo (EditorModuleCommands.cpp)
    std::optional<EditorSourceLocation> behaviourSource(std::string_view componentKey) const override;
    ScriptStatus scriptStatus() const override;

    // The level document's: what the level's areas and the session's own
    // workflows edit.
    SceneEditing editing(ApplicationContext& context);
    EditorContext editorContext(ApplicationContext& context);
    OperatorContext levelOperatorContext(ApplicationContext& context, KeymapRegion region);
    // For what the main menu, the global keys, and F3 run: the document whose
    // window last had focus (focusedPrefabWindow), or the level.
    OperatorContext operatorContext(ApplicationContext& context, KeymapRegion region);
    // window's document, or the level's when window is null.
    OperatorContext operatorContextOn(ApplicationContext& context, PrefabWindowDocument* window, KeymapRegion region);
    // Runs or polls an operator on window's document (the level's when null).
    OperatorResult invokeOperatorOn(ApplicationContext& context, PrefabWindowDocument* window,
                                    std::string_view operatorId, std::string_view properties, KeymapRegion region);
    bool operatorAvailableOn(ApplicationContext& context, PrefabWindowDocument* window, std::string_view operatorId);
    // The region's keys and mouse bindings, run on window's document (the
    // level's when null).
    void handleObjectKeysOn(ApplicationContext& context, PrefabWindowDocument* window, KeymapRegion region);
    uint32_t dispatchMouseChordsOn(ApplicationContext& context, PrefabWindowDocument* window, KeymapRegion region,
                                   uint32_t allowedButtons);
    // Saves the level document to its file, asking for a path when it has
    // none (file.save on the level, and the unsaved-changes prompt).
    bool saveLevelDocument(ApplicationContext& context);

    // What the operator families (editorOperatorFamilies) refer to.
    EditorOperatorServices operatorServices();
    OperatorResult invokeOperator(ApplicationContext& context, std::string_view operatorId,
                                  const OperatorProperties& properties);
    // Runs the binding of the first pressed chord bound in region, on
    // window's document (the level's when null).
    void dispatchKeymap(ApplicationContext& context, PrefabWindowDocument* window, KeymapRegion region);
    // Runs the Hold, Drag, Release, and Click bindings of the followed press
    // (once per frame) on the document the press started in.
    void updateKeymapGesture(ApplicationContext& context);
    // After a press call: when it started a gesture (the dispatcher's
    // gestureSerial moved past followedBefore), records window (null: the
    // level) as the gesture's document.
    void noteGestureOwner(uint64_t followedBefore, const PrefabWindowDocument* window);
    // A menu item showing the operator's shortcut, disabled when its poll fails.
    void operatorMenuItem(ApplicationContext& context, std::string_view operatorId, const char* label,
                          std::string_view properties = {}, bool selected = false);
    // A submenu listing a registered menu's items (the Add menu, Select
    // Grouped), running the chosen operator.
    void drawRegisteredMenu(ApplicationContext& context, const char* menuId, const char* label, bool enabled);
    void drawOperatorWindows(ApplicationContext& context);
    void saveKeymap();
    void saveSettings();

    // Preferences and packaging (EditorPreferences.cpp).
    // Applies the appearance settings through the application's appearance
    // when they changed (art-pixel metrics, fonts for that size, colours) and
    // the theme tuning to the chrome. Runs between frames, where fonts may be
    // added to the atlas.
    void applyAppearance();
    PreferencesContext preferencesContext(ApplicationContext& context);
    // Polls Build and Package; publishes its outcome and opens the output
    // folder when it succeeds.
    void updatePackaging();
    void drawBuildPackageMenu(ApplicationContext& context);
    void saveComponentLayoutIfChanged();
    // Reports last frame's unclaimed OS file drops and queues this frame's.
    void takeFileDrops(const InputState& input);
    void saveSimulateSystems();

    // Game module hosting.
    void loadGameModule();
    void updateGameModule(ApplicationContext& context, const InputState& input, float deltaSeconds);
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

    // Prefab files on disk (EditorSession.cpp). Every prefab file the editor
    // writes (operators, undo and redo of their steps, and saving a prefab
    // edited as its own document through savePrefabDocument) goes through
    // these hooks: they note the content in m_prefabWrites and drop the
    // game's cached copy.
    PrefabWriteHooks prefabWriteHooks();
    void invalidateGamePrefab(const std::filesystem::path& file);
    // Once per frame, after the asset watcher, unless a modal transform runs:
    // handles the prefab files it reported changed.
    void reconcileChangedPrefabs(ApplicationContext& context);
    // One reported file: the editor's own writes (the ledger matches) are
    // ignored; a change made outside the editor refreshes the open
    // document's instances of it (the edit scene during Play) as one undo
    // step, "Refresh <name> instances (changed on disk)", reported in Info.
    void reconcileChangedPrefab(ApplicationContext& context, const std::filesystem::path& file);
    // Cancels the entity eyedropper on Esc, a right-click, Play mode, or a
    // target that no longer exists, and shows its hint at the mouse.
    void updateEyedropper(ApplicationContext& context);
    // Undoes the steps recorded since marker, so a smoke step that took a
    // marker before editing undoes exactly its own steps.
    void undoSince(ApplicationContext& context, CommandHistoryMarker marker);

    // Prefab windows (EditorPrefabWindows.cpp): prefabs open in floating
    // windows of their own beside the level.
    class PrefabWindowActions;

    // The ImGui side of one prefab window: its own panels and layout state.
    struct PrefabWindowView {
        uint64_t id{0};
        std::shared_ptr<EditorPanel> hierarchy;
        std::shared_ptr<EditorPanel> viewport;
        std::shared_ptr<EditorPanel> inspector;
        bool layoutBuilt{false};
        // Bring the window to the front at the next draw.
        bool focusRequest{true};
    };

    // A prefab window waiting for Save, Discard, or Cancel before it closes;
    // continuesPendingAction when closing it is part of quitting or leaving
    // for the Project Hub.
    struct PrefabWindowPrompt {
        uint64_t id{0};
        bool continuesPendingAction{false};
    };

    // The prefab window that last had keyboard focus, or null when a level
    // area had it since.
    PrefabWindowDocument* focusedPrefabWindow() const;
    std::shared_ptr<EditorPanel> createPrefabWindowPanel(std::string_view typeId);
    PrefabWindowView* prefabWindowView(uint64_t id);
    // The open windows with their views, copied so that an operator a
    // window's panel runs may open another window meanwhile.
    std::vector<std::pair<PrefabWindowDocument*, PrefabWindowView*>> prefabWindowSnapshot();
    // Closes the window at the next update, first asking about unsaved
    // changes (Save, Discard, or Cancel).
    void requestClosePrefabWindow(uint64_t id);
    // Between frames: closes the windows whose close was requested.
    void applyPrefabWindowCloses(ApplicationContext& context);
    // Drops every window with its unsaved changes (the session ending).
    void closeAllPrefabWindows(ApplicationContext& context);
    // Creates the panels of a window just opened or recovered, its render
    // scene, and frames its viewport on the prefab.
    void addPrefabWindowView(ApplicationContext& context, PrefabWindowDocument& window);
    // Once per frame before the renderer draws: each window's world
    // transforms, and its scene bound as its render scene's content.
    void bindPrefabWindowScenes(ApplicationContext& context);
    // Writes window's prefab to its file and refreshes its instances in every
    // other open document; false after reporting why not.
    bool savePrefabWindow(ApplicationContext& context, PrefabWindowDocument& window);
    // After window's prefab was saved: refreshes its instances in every other
    // open document (refreshPrefabInstancesElsewhere).
    void refreshAfterPrefabSave(ApplicationContext& context, PrefabWindowDocument& window,
                                const SavedPrefabDocument& saved);
    // The editor changed change.reference's prefab file: refreshes its
    // instances in every open document but the one that wrote it
    // (originWindow, and the level unless refreshLevel): the level (its edit
    // scene during Play) and the windows, each as a
    // step "Refresh <name> instances" on its own history. Reports problems
    // and returns how many instances were refreshed.
    size_t refreshPrefabInstancesElsewhere(ApplicationContext& context, const PrefabFileChange& change,
                                           const PrefabWindowDocument* originWindow, bool refreshLevel);
    // Queues a prefab file an operator or its undo step rewrote from origin
    // (a document's scene), for refreshAfterOwnPrefabWrites.
    void queueOwnPrefabWrite(const std::filesystem::path& file, const std::optional<std::string>& bytes,
                             const Scene* origin);
    // The queued writes, once per frame with the outside changes
    // (reconcileChangedPrefabs).
    void refreshAfterOwnPrefabWrites(ApplicationContext& context);
    OperatorContext prefabWindowOperatorContext(ApplicationContext& context, KeymapRegion region,
                                                PrefabWindowDocument& window);
    // What a window's panels get: windowContext is the session's with the
    // window's scene; context is the session's own.
    EditorContext prefabWindowEditorContext(ApplicationContext& windowContext, ApplicationContext& context,
                                            PrefabWindowDocument& window, PrefabWindowActions& actions);
    void updatePrefabWindows(ApplicationContext& context, const InputState& input, float deltaSeconds);
    // Keeps the assets each window's scene uses loaded (once per frame).
    void leasePrefabWindowAssets(ApplicationContext& context);
    void drawPrefabWindows(ApplicationContext& context);
    void drawPrefabWindow(ApplicationContext& context, PrefabWindowDocument& window, PrefabWindowView& view);
    void buildPrefabWindowLayout(const PrefabWindowDocument& window, unsigned int dockspaceId);
    void drawPrefabWindowToolbar(ApplicationContext& context, PrefabWindowDocument& window);
    void drawPrefabWindowPrompt(ApplicationContext& context);
    // Runs draw with the EditorContext of the document whose window last had
    // focus: a prefab window's (as its panels get it), or the level's.
    void withFocusedEditorContext(ApplicationContext& context, const std::function<void(EditorContext&)>& draw);
    // A prefab file changed outside the editor: refreshes its instances in
    // the windows, and warns when a window edits that prefab itself.
    void reconcilePrefabWindows(ApplicationContext& context, const PrefabFileChange& change);
    // For quitting and the Project Hub: asks about the first window with
    // unsaved changes; false when none has any.
    bool askAboutPrefabWindows();
    // Writes the windows with unsaved changes to m_prefabWindowRecovery,
    // replacing its previous set; reportErrors is false in a crashed process.
    void autosavePrefabWindows(ApplicationContext& context, bool reportErrors);
    // Recover: reopens the autosaved prefab windows.
    void recoverPrefabWindows(ApplicationContext& context);

    // Autosave and the recovery prompt (EditorSessionRecovery.cpp).
    void updateAutosave(ApplicationContext& context, float deltaSeconds);
    // Gives the crash handler the session's emergency save, once, with the
    // context the session runs in (the context load gets may be a staging
    // one).
    void registerCrashRecovery(ApplicationContext& context);
    // Writes the open document, the edit scene even during Play, to the
    // recovery file when it has unsaved changes. Runs in a crashed process
    // (CrashRecoveryAction): best effort, reporting nothing.
    void saveRecoveryAfterCrash(ApplicationContext& context);
    void drawRecoveryDialog(ApplicationContext& context);

    // The main menu bar (EditorMainMenu.cpp) and the status bar
    // (EditorStatusBar.cpp).
    void drawMainMenu(ApplicationContext& context);
    // View > Overlays: the active viewport's debugging overlays.
    void drawDebugOverlaysMenu(ApplicationContext& context);
    void handleGlobalShortcuts(ApplicationContext& context);
    void drawStatusBar(const RenderStats& stats);
    void drawPlayModePill() const;

    // File dialogs and the unsaved-changes workflow (EditorFileDialogs.cpp).
    void drawPathDialog(ApplicationContext& context);
    void drawUnsavedChangesDialog(ApplicationContext& context);
    void requestFileDialog(FileDialogKind kind);
    void openPathDialog(FileDialogKind kind);
    void pollFileDialog(ApplicationContext& context);
    bool completeOpen(ApplicationContext& context, const std::filesystem::path& path);
    bool completeSaveAs(ApplicationContext& context, const std::filesystem::path& path);
    void onDocumentPathCommitted();
    // Runs the pending action once nothing in its way has unsaved changes:
    // Close and leaving for the Project Hub first ask about each prefab
    // window with unsaved changes, then every action asks about the level.
    // Cancelling any prompt cancels the action.
    void continuePendingAction(ApplicationContext& context);
    // Lists a save's warnings (references to generated entities saved as
    // none) in the Info log.
    void reportSaveWarnings(const SceneDocumentResult& result);
    void finishPendingAction(ApplicationContext& context);

    // The playback toolbar in the main menu bar (EditorMainMenu.cpp); Play
    // itself is m_playback's.
    void drawPlaybackToolbar(ApplicationContext& context);
    // Slot in the toolbar's Simulate dropdown for the Simulate system picker
    // (Unit 18); shows a pointer to the System Profiler until then.
    void drawSimulateOptions(ApplicationContext& context);

    // Blender object-mode workflows (EditorObjectCommands.cpp).
    void drawObjectPopups(ApplicationContext& context);
    // The New Behaviour, Component, and System dialog (EditorModuleCommands.cpp).
    void drawNewCodeDialog(ApplicationContext& context);
    // Project Settings, and applying what it saves (EditorProjectSettings.cpp).
    void drawProjectSettings(ApplicationContext& context);
    std::string applyProjectSettings(ApplicationContext& context, const ProjectSettingsValues& values);
    void finishModalTransform(ApplicationContext& context, bool confirm);
    // The scene the running modal transform edits (the level's, or its
    // prefab window's) and an editing over its document; null when that
    // window is gone.
    Scene* modalTransformScene(ApplicationContext& context);
    std::optional<SceneEditing> modalTransformEditing(ApplicationContext& context);
    void drawModalTransformOverlay(const ViewportPanel& viewport, const ModalTransformResult& result) const;

    // Viewports (EditorViewports.cpp).
    // Start of a frame: settles which viewports are open and cancels a modal
    // transform whose viewport closed.
    void beginViewportFrame(ApplicationContext& context);
    // After the areas are drawn: shadows and streaming follow the active
    // level viewport's render view.
    void syncActiveRenderView(ApplicationContext& context);
    // The open viewports showing the level (not a prefab window's), and the
    // active viewport when it is one of them, else the first.
    std::vector<ViewportPanel*> levelViewports() const;
    ViewportPanel* activeLevelViewport() const;
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
    // The autosaves of the prefab windows with unsaved changes.
    PrefabWindowRecovery m_prefabWindowRecovery;
    EditorDiagnostics m_diagnostics;
    // The game module's build: started by build.run (Ctrl B), the Build
    // panel, or build on save, and polled every frame. Declared before the
    // operators that refer to it.
    ProjectBuild m_projectBuild{m_settings.tools(), m_diagnostics};
    // The fields the Properties panel watches; declared before the operators
    // that edit it.
    WatchList m_watches;
    CommandHistory m_history;
    // The content the editor last wrote to, or reconciled for, each prefab
    // file, so the asset watcher's reports of the editor's own writes are
    // ignored and an outside change knows what the file held before.
    PrefabWriteLedger m_prefabWrites;
    // Prefab files the asset watcher reported changed, until
    // reconcileChangedPrefabs handles them.
    std::vector<std::filesystem::path> m_changedPrefabFiles;

    // A prefab file an operator, or the undo or redo of its step, rewrote in
    // one open document: the others catch up with it once per frame
    // (refreshAfterOwnPrefabWrites). The origin is kept as a window id, which
    // is never reused, and the level's scene, which is only compared.
    struct OwnPrefabWrite {
        std::filesystem::path file;
        std::optional<std::string> before;
        std::optional<std::string> after;
        uint64_t originWindow{0};
        const Scene* originScene{nullptr};
    };

    std::vector<OwnPrefabWrite> m_ownPrefabWrites;
    EditorPlayMode m_playMode;
    std::filesystem::path m_projectFile;
    std::filesystem::path m_codeSnippetDirectory;
    PhysicsLayerTable m_physicsLayers;
    // The project's layer and tag names the inspector offers.
    EditorProjectNames m_projectNames;
    std::function<void(CrashRecoveryAction)> m_crashRecoveryChanged;
    bool m_crashRecoveryRegistered{false};
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
    // Play and Simulate, their targets and game views, and the standalone
    // game. Declared after the module host and the registries it uses; it
    // binds the workspaces, area types, and navigation settings, declared
    // later, by reference only, and outlives the areas, whose game view
    // draws its game Ui.
    EditorPlayback m_playback;
    // Prefab windows: declared after the module host, so their scenes, which
    // may hold module component storage, go before the module does.
    PrefabWindowSet m_prefabWindows;
    // The window last focused (0: a level area was focused since), which
    // the main menu, global keys, and F3 act on.
    uint64_t m_focusedPrefabWindow{0};
    std::vector<uint64_t> m_prefabWindowCloses;
    std::optional<PrefabWindowPrompt> m_prefabWindowPrompt;
    bool m_openPrefabWindowPrompt{false};
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
    // The prefab window whose document the followed press started in; 0: the
    // level. Window ids are never reused, so a closed window's gesture is
    // cancelled when it next advances.
    uint64_t m_gestureWindow{0};
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
    // OS file drops of the current frame, claimed by the windows they land in.
    FileDropQueue m_fileDrops;

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
    // The preview navigation mesh navigation.bake builds and the viewports
    // draw, and the project's settings it and Play bake with.
    NavigationWorld m_navigationPreview;
    NavMeshBuildSettings m_navigationSettings;
    BuildPanel m_buildPanel{m_settings.tools(), m_projectBuild, &m_packageRunner};
    // Shows the engine log, diagnostics, and operator history.
    InfoPanel m_infoPanel{m_operatorRunner.history()};

    // Declared after everything area panels refer to (the panels above, the
    // settings, and the module host), so the areas are destroyed first.
    // Registration order is the View menu order.
    AreaRegistry m_areaTypes;
    WorkspaceSet m_workspaces;
    // The prefab windows' panels, made by the area types above.
    std::vector<std::unique_ptr<PrefabWindowView>> m_prefabWindowViews;
    std::filesystem::path m_workspacesPath;
    // layout.ini from editors before workspaces.json, read once to migrate.
    std::filesystem::path m_legacyLayoutPath;
    // The active workspace has no saved layout; build its default one when
    // the next frame's dockspace exists.
    bool m_buildDefaultLayout{false};
    bool m_workspacesDirty{false};

    std::filesystem::path m_sceneRoot;
    // The gap, area headers, and panes behind every window.
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
        // The prefab window whose document is transformed; 0 for the level.
        uint64_t window{0};
        std::vector<SceneEntityId> entities;
        std::vector<glm::mat4> startLocal;
        // The moved entities (or the new copies) from before the command,
        // for undo.
        SceneEditing::EntitySnapshot before;
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
