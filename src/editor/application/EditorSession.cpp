#include "EditorSession.hpp"

#include "editor/application/EntityCommands.hpp"
#include "editor/application/PrefabInstanceRefresh.hpp"
#include "editor/areas/AreaWindows.hpp"
#include "editor/areas/DefaultWorkspaces.hpp"
#include "editor/document/DefaultScene.hpp"
#include "editor/document/SceneFragment.hpp"
#include "editor/menus/DefaultMenus.hpp"
#include "editor/operators/DefaultKeymap.hpp"
#include "editor/operators/EditorOperatorFamilies.hpp"
#include "editor/project/AssetReload.hpp"
#include "editor/project/CodeGenerator.hpp"
#include "editor/project/FastProjectBuild.hpp"
#include "editor/settings/SimulateSystemSettings.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/EditorWidgets.hpp"
#include "engine/scene/ComponentTypes.hpp"

#include <imgui.h>

#include <ImGuizmo.h>
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace {
constexpr const char* sceneExtension = ".zenith-scene";
} // namespace

EditorSession::EditorSession(std::filesystem::path settingsDirectory, std::filesystem::path assetRoot,
                             std::filesystem::path sceneRoot, EditorRenderSettings renderSettings,
                             EditorAppearance& appearance, EditorSceneLoader initialSceneLoader,
                             std::filesystem::path gameModule, EditorProjectServices projectServices)
    : m_settings(settingsDirectory / "settings.ini")
    , m_document(assetRoot)
    , m_recovery(settingsDirectory / (std::string("recovery") + sceneExtension))
    , m_prefabRecovery(settingsDirectory / "recovery.prefab-edit")
    , m_prefabWindowRecovery(settingsDirectory / "recovery-prefab-windows")
    , m_projectFile(projectServices.projectFile)
    , m_codeSnippetDirectory(projectServices.codeSnippetDirectory)
    , m_physicsLayers(projectServices.physicsLayers)
    , m_projectNames{.physicsLayers = m_physicsLayers.names, .tags = projectServices.tags}
    , m_playServices(projectServices.play)
    , m_standalone(m_playServices.makeGameProcess ? m_playServices.makeGameProcess() : makeGameProcess())
    , m_mouseCaptureChanged(projectServices.mouseCaptureChanged)
    , m_crashRecoveryChanged(projectServices.crashRecoveryChanged)
    , m_fileDialog("Zenith scene", sceneExtension)
    , m_operatorRunner(m_operators, m_history)
    , m_gameModulePath(std::move(gameModule))
    , m_simulateSystemsPath(settingsDirectory / "simulate-systems.json")
    , m_keymap(defaultEditorKeymap())
    , m_keymapPath(settingsDirectory / "keymap.json")
    , m_appearance(appearance)
    // The game module's scripts are listed beside the assets.
    , m_projectAssets(std::move(assetRoot), sceneRoot, settingsDirectory / "asset-browser.json",
                      {.sourceRoot = projectServices.projectFile.empty()
                                         ? std::filesystem::path{}
                                         : projectSourceDirectory(projectServices.projectFile)})
    , m_renderSettings(std::move(renderSettings))
    , m_componentLayoutPath(settingsDirectory / "component-inspector.json")
    , m_workspacesPath(settingsDirectory / "workspaces.json")
    , m_legacyLayoutPath(settingsDirectory / "layout.ini")
    , m_sceneRoot(std::move(sceneRoot))
    , m_initialSceneLoader(std::move(initialSceneLoader))
    , m_leaveForProjectHub(std::move(projectServices.leaveForProjectHub))
    , m_smokeScratchDirectory(settingsDirectory) {
    m_navigationSettings = projectServices.navigation;
    for (const std::string& problem : m_keymap.defaultProblems()) {
        m_diagnostics.reportError(problem);
    }
    // A gesture bound to an operator no one registered (a module's, or one
    // still to come) leaves the press to camera navigation.
    m_keymapDispatcher.setOperatorFilter(
        [this](std::string_view operatorId) { return m_operators.find(operatorId) != nullptr; });
    const EditorOperatorServices services = operatorServices();
    for (const OperatorFamily& family : editorOperatorFamilies()) {
        if (const OperatorRegistrationResult result = family.registerOperators(m_operators, services); !result) {
            m_diagnostics.reportError(std::string(family.name) + ": " + result.error);
        }
    }
    registerAreaTypes();
    const auto projectPrefabs = [this] {
        std::vector<std::filesystem::path> prefabs;
        for (const EditorAssetRecord& asset : m_projectAssets.catalog().assets()) {
            if (asset.kind == EditorAssetKind::Prefab) {
                prefabs.push_back(asset.absolutePath);
            }
        }
        return prefabs;
    };
    if (const MenuResult result = registerDefaultMenus(m_menus.registry(), {.projectPrefabs = projectPrefabs});
        !result) {
        m_diagnostics.reportError(result.error);
    }
    // Scene references resolve through the project's asset database by GUID.
    m_document.setAssetDatabase(m_projectAssets.assetDatabase());
    m_projectBuild.setProjectBuild(std::move(projectServices.build));
    // Captures the compiler's developer environment in the background now,
    // so the first build of the session is already a fast one.
    if (!m_projectFile.empty()) {
        m_projectBuild.setFastBuild(
            std::make_unique<FastProjectBuild>(m_projectFile, settingsDirectory / "developer-environment.json"));
    }
    m_moduleHost.setProjectInputActions(std::move(projectServices.inputActions));
    if (!projectServices.inputActionsError.empty()) {
        m_diagnostics.add(EditorDiagnosticSeverity::Error, "Project", projectServices.inputActionsError,
                          "Fix the project's input action file, or its actions in Edit > Project Settings.");
    }
}

SceneEditing EditorSession::editing(ApplicationContext& context) {
    // In Play and Simulate edits go live to the runtime scene, unrecorded.
    return {context.scene, context.assets, m_document, m_history, m_diagnostics, m_playMode.editScene()};
}

EditorContext EditorSession::editorContext(ApplicationContext& context) {
    return {.app = context,
            .editing = editing(context),
            .selection = m_selection,
            .diagnostics = m_diagnostics,
            .mode = m_playMode.mode(),
            .actions = *this,
            .eyedropper = &m_eyedropper,
            .menus = m_menus,
            .simulating = m_playMode.simulating(),
            .componentEditors = &m_componentEditors,
            .navigationPreview = &m_navigationPreview,
            .navigationSettings = &m_navigationSettings,
            .gameServices = m_playMode.mode() == EditorMode::Edit ? nullptr : m_moduleHost.gameServices(),
            .scripts = this,
            .watches = &m_watches,
            .projectNames = &m_projectNames,
            .prefabEdit = activePrefabSummary(),
            .projectAssets = &m_projectAssets,
            .fileDrops = &m_fileDrops,
            .definitionSchemas =
                m_moduleHost.module() != nullptr ? m_moduleHost.module()->definitionSchemas().get() : nullptr};
}

OperatorContext EditorSession::levelOperatorContext(ApplicationContext& context, KeymapRegion region) {
    return {.editing = editing(context),
            .selection = m_selection,
            .mode = m_playMode.mode(),
            .region = region,
            .app = &context};
}

OperatorContext EditorSession::operatorContext(ApplicationContext& context, KeymapRegion region) {
    return operatorContextOn(context, focusedPrefabWindow(), region);
}

OperatorContext EditorSession::operatorContextOn(ApplicationContext& context, PrefabWindowDocument* window,
                                                 KeymapRegion region) {
    return window != nullptr ? prefabWindowOperatorContext(context, region, *window)
                             : levelOperatorContext(context, region);
}

EditorOperatorServices EditorSession::operatorServices() {
    return {.eyedropper = m_eyedropper,
            .componentClipboard = m_componentClipboard,
            .componentLayout = m_componentLayout,
            .menus = m_menus,
            .viewports = m_viewports,
            .workspaces = m_workspaces,
            .areaTypes = m_areaTypes,
            .playMode = m_playMode,
            .modules = m_moduleHost,
            .preferences = m_preferences,
            .packageRunner = m_packageRunner,
            .build = m_projectBuild,
            .watches = m_watches,
            .navigationPreview = m_navigationPreview,
            .navigationSettings = m_navigationSettings,
            .tools = m_settings.tools(),
            .projectFile = m_renderSettings.settings().projectFile(),
            .codeSnippetDirectory = m_codeSnippetDirectory,
            .revealAsset = [this](const std::filesystem::path& asset) { m_projectAssets.requestReveal(asset); },
            .simulateMaskChanged = [this] { saveSimulateSystems(); },
            .prefabWritten = prefabWriteHooks().fileWritten,
            .prefabRead = prefabWriteHooks().fileRead,
            .documentCommands = *this,
            .playCommands = *this,
            .objectModeCommands = *this,
            .windowCommands = *this,
            .prefabModeCommands = *this};
}

bool EditorSession::documentCommandsAvailable(const OperatorContext&) const {
    // The level's state, whichever document the operator runs on: a prefab
    // window stays in Edit mode while the level plays.
    return m_playMode.mode() == EditorMode::Edit && !m_fileDialog.pending() && !m_activeTransform;
}

bool EditorSession::editCommandsAvailable(const OperatorContext& context) const {
    return context.editMode() && !m_fileDialog.pending() && !m_activeTransform;
}

bool EditorSession::saveAsAvailable(const OperatorContext& context) const {
    return documentCommandsAvailable(context) && m_documents.empty() &&
           m_prefabWindows.owner(context.editing.document()) == nullptr;
}

bool EditorSession::projectHubAvailable() const {
    return m_leaveForProjectHub && !m_activeTransform && !m_fileDialog.pending();
}

void EditorSession::openOperatorSearch() {
    m_operatorSearch.open();
}

void EditorSession::openKeymapEditor() {
    m_showKeymapEditor = true;
}

// The level's areas run operators on the level; prefab windows' panels
// run them through PrefabWindowActions on their own document.
OperatorResult EditorSession::invokeOperator(ApplicationContext& context, std::string_view operatorId,
                                             std::string_view properties, KeymapRegion region) {
    return invokeOperatorOn(context, nullptr, operatorId, properties, region);
}

OperatorResult EditorSession::invokeOperator(ApplicationContext& context, std::string_view operatorId,
                                             const OperatorProperties& properties) {
    OperatorContext operatorContext = levelOperatorContext(context, KeymapRegion::Global);
    return m_operatorRunner.invoke(operatorId, operatorContext, &properties);
}

bool EditorSession::operatorAvailable(ApplicationContext& context, std::string_view operatorId) {
    return operatorAvailableOn(context, nullptr, operatorId);
}

OperatorResult EditorSession::invokeOperatorOn(ApplicationContext& context, PrefabWindowDocument* window,
                                               std::string_view operatorId, std::string_view properties,
                                               KeymapRegion region) {
    OperatorContext operatorContext = operatorContextOn(context, window, region);
    OperatorResult result = m_operatorRunner.invokeWithJson(operatorId, operatorContext, properties);
    if (window != nullptr) {
        // Adjust Last Operation re-runs on the level only.
        m_operatorRunner.forgetLast();
    }
    return result;
}

bool EditorSession::operatorAvailableOn(ApplicationContext& context, PrefabWindowDocument* window,
                                        std::string_view operatorId) {
    const Operator* op = m_operators.find(operatorId);
    return op != nullptr && op->canRun(operatorContextOn(context, window, KeymapRegion::Global));
}

std::string EditorSession::operatorShortcut(std::string_view operatorId, std::string_view properties) const {
    return m_keymap.shortcutLabel(operatorId, properties);
}

void EditorSession::dispatchKeymap(ApplicationContext& context, PrefabWindowDocument* window, KeymapRegion region) {
    // Global keys work while any panel has focus; region keys already require
    // the mouse over the region. A game view with input focus owns the keys
    // the game can use, so Space there reaches the game, not operator search.
    const ViewportPanel* game = gameViewport();
    const KeyboardOwner owner = game != nullptr && game->gameInputFocus() ? KeyboardOwner::Game : KeyboardOwner::Editor;
    if (const std::optional<KeymapInvocation> invocation = m_keymapDispatcher.pressKeys(m_keymap, region, owner)) {
        invokeOperatorOn(context, window, invocation->operatorId, invocation->properties, invocation->region);
    }
}

void EditorSession::updateKeymapGesture(ApplicationContext& context) {
    for (const KeymapInvocation& invocation : m_keymapDispatcher.update(m_keymap)) {
        invokeOperator(context, invocation.operatorId, invocation.properties, invocation.region);
    }
}

uint32_t EditorSession::dispatchMouseChords(ApplicationContext& context, KeymapRegion region, uint32_t allowedButtons) {
    return dispatchMouseChordsOn(context, nullptr, region, allowedButtons);
}

uint32_t EditorSession::dispatchMouseChordsOn(ApplicationContext& context, PrefabWindowDocument* window,
                                              KeymapRegion region, uint32_t allowedButtons) {
    if (m_activeTransform || m_fileDialog.pending() || m_keymapEditor.capturing()) {
        allowedButtons = 0;
    }
    return m_keymapDispatcher.dispatchMouse(
        m_keymap, region, allowedButtons, [this, &context, window](const KeymapInvocation& invocation) {
            invokeOperatorOn(context, window, invocation.operatorId, invocation.properties, invocation.region);
        });
}

bool EditorSession::mouseButtonBound(KeymapRegion region, int button) const {
    return m_keymapDispatcher.mouseButtonBound(m_keymap, region, button);
}

void EditorSession::operatorMenuItem(ApplicationContext& context, std::string_view operatorId, const char* label,
                                     std::string_view properties, bool selected) {
    const Operator* op = m_operators.find(operatorId);
    if (op == nullptr) {
        return;
    }
    // The main menu acts on the document whose window last had focus.
    PrefabWindowDocument* window = focusedPrefabWindow();
    const bool enabled = op->canRun(operatorContextOn(context, window, KeymapRegion::Global));
    const std::string shortcut = m_keymap.shortcutLabel(operatorId, properties);
    if (ImGui::MenuItem(label != nullptr ? label : op->label.c_str(), shortcut.empty() ? nullptr : shortcut.c_str(),
                        selected, enabled)) {
        invokeOperatorOn(context, window, operatorId, properties, KeymapRegion::Global);
    }
    if (!op->description.empty()) {
        ImGui::SetItemTooltip("%s", op->description.c_str());
    }
}

void EditorSession::drawOperatorWindows(ApplicationContext& context) {
    if (const std::optional<std::string> chosen =
            m_operatorSearch.draw(m_operators, m_keymap, operatorContext(context, KeymapRegion::Global))) {
        invokeOperatorOn(context, focusedPrefabWindow(), *chosen, {}, KeymapRegion::Global);
    }
    if (m_keymapEditor.draw(m_showKeymapEditor, m_keymap, m_operators)) {
        saveKeymap();
    }
    if (m_preferences.draw(m_settings, preferencesContext(context))) {
        saveSettings();
    }
    drawProjectSettings(context);
    if (m_playMode.mode() != EditorMode::Edit || m_activeTransform) {
        return;
    }
    if (const std::optional<OperatorProperties> edited =
            m_lastOperatorPanel.draw(m_operatorRunner.lastOperator(), lastOperatorAnchor())) {
        OperatorContext operatorContext = levelOperatorContext(context, KeymapRegion::Global);
        m_operatorRunner.adjustLast(operatorContext, *edited);
    }
}

void EditorSession::saveSimulateSystems() {
    if (const SimulateSystemSettingsResult result =
            saveSimulateSystemSettings(m_simulateSystemsPath, m_moduleHost.simulateMask());
        !result) {
        m_diagnostics.reportError(result.error);
    }
}

void EditorSession::saveKeymap() {
    if (const KeymapResult result = m_keymap.save(m_keymapPath); !result) {
        m_diagnostics.reportError(result.error);
    }
}

void EditorSession::saveSettings() {
    if (const EditorSettingsResult result = m_settings.save(); !result) {
        m_diagnostics.reportError(result.error);
    }
}

SceneResult EditorSession::load(ApplicationContext& context) {
    if (const EditorSettingsResult result = m_settings.load(); !result) {
        m_diagnostics.reportError(result.error);
    }
    if (const EditorRenderSettingsResult result = m_renderSettings.load(context.rendering); !result) {
        m_diagnostics.reportError(result.error);
    }
    if (const ComponentInspectorLayoutResult result = m_componentLayout.load(m_componentLayoutPath); !result) {
        m_diagnostics.reportError(result.error);
    }
    const KeymapResult keymap = m_keymap.load(m_keymapPath);
    if (!keymap) {
        m_diagnostics.reportError(keymap.error);
    }
    for (const std::string& warning : keymap.warnings) {
        m_diagnostics.add(EditorDiagnosticSeverity::Warning, "Keymap", warning);
    }
    SimulateSystemMask simulateMask;
    if (const SimulateSystemSettingsResult result = loadSimulateSystemSettings(m_simulateSystemsPath, simulateMask);
        !result) {
        m_diagnostics.reportError(result.error);
    }
    m_moduleHost.setSimulateMask(std::move(simulateMask));
    m_moduleHost.setLogger(context.logger);
    m_moduleHost.setDebugDraw(context.rendering.debugDraw());
    // A previous project's bodies may still be in the world.
    context.physics.clear();
    if (const PhysicsResult result = context.physics.setLayers(m_physicsLayers); !result) {
        m_diagnostics.reportError("Could not use the project's physics layers: " + result.error);
    }
    m_projectAssets.refresh(m_diagnostics);
    m_openRecoveryDialog =
        m_recovery.available() || m_prefabRecovery.available() || !m_prefabWindowRecovery.available().empty();

    // The theme, fonts, popup bevels, and the disabled imgui.ini are the
    // application's (EditorApplication::onStart), so they outlive a switch
    // between projects. The session applies its preferences through the
    // application's appearance: the UI scale and the theme tuning.
    applyAppearance();
    // Asset paths in render settings (the colour-grading LUT) resolve against
    // the edited project's assets.
    context.rendering.setAssetRoot(m_document.options().assetRoot);
    // Before any scene loads, so documents read the module's component types.
    loadGameModule();
    // After the module, so its panels' areas are restored with their types.
    syncAreaTypes();
    loadWorkspaces();
    if (m_initialSceneLoader) {
        if (SceneResult result = m_initialSceneLoader(context, m_document.options()); !result) {
            return result;
        }
    } else if (const SceneResult result =
                   populateDefaultScene(context.scene, context.assets, m_document.options().assetRoot);
               !result) {
        m_diagnostics.reportError(result.error);
    }
    notePrefabFilesRead(context.scene, m_document.options().assetRoot, prefabWriteHooks());
    if (SDL_getenv("ZENITH_EDITOR_SMOKE") != nullptr) {
        return prepareSmokeScenario(context);
    }
    return {.success = true};
}

void EditorSession::update(ApplicationContext& context, const InputState& input, float deltaSeconds) {
    // Between frames, where a new UI scale may add fonts to the atlas.
    applyAppearance();
    // Before the smoke scenario, which checks the switch it requested.
    updateWorkspaces();
    // Between frames: no window's panels are drawing.
    applyPrefabWindowCloses(context);
    beginViewportFrame(context);
    if (m_smokeRunner) {
        updateSmokeScenario(context);
    }
    registerCrashRecovery(context);
    updateAutosave(context, deltaSeconds);
    updatePackaging();
    m_projectBuild.poll();
    updateStandalone(context);
    // Once per frame, however many asset browsers are open.
    m_projectAssets.pollChanges(m_diagnostics, projectAssetActions(context), deltaSeconds);
    takeFileDrops(input);
    reconcileChangedPrefabs(context);
    updateAreas(context, input, deltaSeconds);
    updatePrefabWindows(context, input, deltaSeconds);
    leasePrefabWindowAssets(context);
    updateGameModule(context, input, deltaSeconds);
    syncAreaTypes();
    saveComponentLayoutIfChanged();
}

void EditorSession::takeFileDrops(const InputState& input) {
    // The windows drawn last frame claimed the drops inside them.
    if (!m_fileDrops.empty()) {
        m_fileDrops.takeAll();
        m_diagnostics.add(EditorDiagnosticSeverity::Info, "Asset Browser",
                          "Drop files on an Asset Browser folder to import them, or a model on a viewport to "
                          "import and place it.");
    }
    m_fileDrops.add(input.droppedFiles);
}

void EditorSession::saveComponentLayoutIfChanged() {
    if (!m_componentLayout.takeChanged()) {
        return;
    }
    if (const ComponentInspectorLayoutResult result = m_componentLayout.save(m_componentLayoutPath); !result) {
        m_diagnostics.reportError(result.error);
    }
}

ProjectAssetActions EditorSession::projectAssetActions(ApplicationContext& context) {
    return {
        .reloadModel =
            [this, &context](const std::filesystem::path& path) {
                reloadProjectModel(context.assets, m_diagnostics, path);
            },
        // The scene shown and, in Play, the parked edit scene follow the
        // material.
        .reloadMaterial =
            [this, &context](const std::filesystem::path& path) {
                const std::array<Scene*, 2> scenes{&context.scene, m_playMode.editScene()};
                reloadProjectMaterial(context.assets, scenes, m_diagnostics, path);
            },
        .reloadTexture =
            [this, &context](const std::filesystem::path& path) {
                const std::array<Scene*, 2> scenes{&context.scene, m_playMode.editScene()};
                reloadProjectTexture(context.assets, scenes, componentTypesFor(m_document.options()),
                                     m_projectAssets.assetDatabase().get(), m_diagnostics, path);
            },
        .reloadAudio =
            [this, &context](const std::filesystem::path& path) {
                const std::array<Scene*, 2> scenes{&context.scene, m_playMode.editScene()};
                reloadProjectAudio(context.audio, scenes, m_diagnostics, path);
            },
        .prefabChanged =
            [this](const std::filesystem::path& path) {
                invalidateGamePrefab(path);
                if (std::ranges::find(m_changedPrefabFiles, path) == m_changedPrefabFiles.end()) {
                    m_changedPrefabFiles.push_back(path);
                }
            },
        .filesMoved = [this, &context](std::span<const ProjectFileMove> moves) { projectFilesMoved(context, moves); },
        .filesRemoved =
            [this, &context](std::span<const std::filesystem::path> removed) { projectFilesRemoved(context, removed); },
    };
}

void EditorSession::invalidateGamePrefab(const std::filesystem::path& file) {
    // Editor prefab operations build a fresh PrefabSystem each time; the
    // game's lives as long as the module host.
    if (GameServices* services = m_moduleHost.gameServices()) {
        services->prefabs().invalidate(file);
    }
}

PrefabWriteHooks EditorSession::prefabWriteHooks() {
    return {.fileWritten =
                [this](const std::filesystem::path& file, const std::optional<std::string>& bytes) {
                    m_prefabWrites.record(file, bytes);
                    invalidateGamePrefab(file);
                },
            // The first content seen is what an outside change is compared
            // with; the editor's own later writes replace it.
            .fileRead =
                [this](const std::filesystem::path& file, const std::optional<std::string>& bytes) {
                    if (m_prefabWrites.recorded(file) == nullptr) {
                        m_prefabWrites.record(file, bytes);
                    }
                }};
}

void EditorSession::reconcileChangedPrefabs(ApplicationContext& context) {
    // A modal transform's undo step diffs against the scene it started from,
    // so a refresh recorded under it would be undone twice.
    if (m_changedPrefabFiles.empty() || m_activeTransform) {
        return;
    }
    for (const std::filesystem::path& file : std::exchange(m_changedPrefabFiles, {})) {
        reconcileChangedPrefab(context, file);
    }
}

void EditorSession::reconcileChangedPrefab(ApplicationContext& context, const std::filesystem::path& file) {
    PrefabFileChange change;
    if (const PrefabResult classified =
            classifyPrefabFileChange(m_prefabWrites, file, m_document.options().assetRoot, change);
        !classified) {
        m_diagnostics.add(EditorDiagnosticSeverity::Warning, "Prefabs", classified.error);
        return;
    }
    if (!change.external) {
        // The editor's own write (or undo or redo of one): caches were
        // already dropped when it wrote.
        return;
    }
    reconcileParkedDocuments(context, change);
    reconcilePrefabWindows(context, change);
    const PrefabEditTarget* openPrefab = m_documents.activePrefab();
    if (openPrefab != nullptr &&
        openPrefab->reference.generic_string() == change.reference.lexically_normal().generic_string()) {
        // The open prefab's own file: its document is the user's to save or
        // discard (reconcileParkedDocuments warned).
        return;
    }
    // During Play the parked edit scene is refreshed; the running scene keeps
    // its copies until Stop.
    Scene* editScene = m_playMode.editScene();
    Scene& scene = editScene != nullptr ? *editScene : context.scene;
    const SceneEditing documentEditing(scene, context.assets, m_document, m_history, m_diagnostics);
    size_t refreshed = 0;
    const PrefabResult result =
        refreshPrefabInstances(documentEditing, change.reference, change.previous ? &*change.previous : nullptr,
                               externalPrefabRefreshStepName(change.displayName), &refreshed);
    for (const std::string& diagnostic : result.diagnostics) {
        m_diagnostics.add(EditorDiagnosticSeverity::Warning, "Prefabs", diagnostic);
    }
    if (!result) {
        m_diagnostics.reportError("Could not refresh " + change.displayName + " instances: " + result.error);
        return;
    }
    if (refreshed == 0) {
        return;
    }
    if (editScene == nullptr) {
        m_selection.removeMissing(scene);
    }
    std::string message = change.displayName + " changed on disk; refreshed " + std::to_string(refreshed) +
                          (refreshed == 1 ? " instance" : " instances") + "; Undo puts them back.";
    if (!change.previous) {
        message += " Its previous content was unknown, so each instance kept the overrides of its last prefab "
                   "operation.";
    }
    m_diagnostics.add(EditorDiagnosticSeverity::Info, "Prefabs", std::move(message));
}

void EditorSession::fixedUpdate(ApplicationContext& context, const InputState& input, float deltaSeconds) {
    // Fixed updates run only while simulating (including a paused single
    // step); Edit mode keeps its own fixed clock in updateGameModule.
    // Simulate passes no input; the host enforces that too.
    if (m_playMode.mode() != EditorMode::Edit) {
        const SimulationMode mode = m_playMode.simulationMode();
        runGameplayStage(systemPhaseName(SystemPhase::FixedUpdate), [&] {
            m_moduleHost.runSystems(SystemPhase::FixedUpdate, context.scene, &context, mode, deltaSeconds,
                                    mode == SimulationMode::Simulate ? nullptr : &gameInput(input));
        });
    }
}

void EditorSession::afterPhysicsStep(ApplicationContext& context) {
    if (GameServices* services = m_moduleHost.gameServices()) {
        runGameplayStage("the physics events", [&] {
            services->publishPhysicsEvents(context.scene, context.physics.triggerEvents(),
                                           context.physics.contactEvents());
        });
    }
}

void EditorSession::lateUpdate(ApplicationContext& context, const InputState&, float deltaSeconds) {
    if (m_playMode.mode() == EditorMode::Edit || !m_playMode.advancesThisFrame()) {
        return;
    }
    const SimulationMode mode = m_playMode.simulationMode();
    runGameplayStage(systemPhaseName(SystemPhase::LateUpdate), [&] {
        m_moduleHost.runSystems(SystemPhase::LateUpdate, context.scene, &context, mode,
                                simulatedSeconds(simulationClock(), deltaSeconds), &m_frameGameInput, nullptr,
                                deltaSeconds);
    });
}

bool EditorSession::fixedUpdatesEnabled() const {
    return m_playMode.fixedUpdatesEnabled();
}

bool EditorSession::beginFixedUpdate() {
    return m_playMode.beginFixedUpdate();
}

SimulationClock EditorSession::simulationClock() const {
    // The game's own pause and time scale apply on top of the editor's.
    const SimulationClock editorClock = m_playMode.simulationClock();
    const GameServices* services = m_moduleHost.gameServices();
    return services != nullptr ? services->simulationClock(editorClock) : editorClock;
}

void EditorSession::requestQuit(ApplicationContext& context) {
    requestClose(context);
}

void EditorSession::drawUi(ApplicationContext& context) {
    // Every system of this frame ran in update and fixedUpdate; publish
    // their timings for the System Profiler areas drawn below.
    m_moduleHost.finishSystemFrame();
    // Between the frame's gameplay and the game view's UI: a run whose
    // gameplay failed ends here, so the rest of the frame draws Edit mode.
    stopFailedGameplay(context);
    // The engine has run this frame's fixed steps, so a frame step is done;
    // play.step below requests the next one.
    m_playMode.finishFrame();
    // Before the areas, so the game view draws this frame's game UI.
    drawGameViewUi(context);
    ImGuizmo::BeginFrame();
    m_themeChrome.setModeTint(m_documents.empty() ? playModeTint(m_playMode.mode(), m_playMode.simulating())
                                                  : ThemeModeTint::Prefab);
    m_themeChrome.beginFrame();
    // The dockspace host is transparent so the dark gap shows between areas.
    ImGui::PushStyleColor(ImGuiCol_WindowBg, toImVec4(palette::transparent));
    const ImGuiID dockspaceId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
    ImGui::PopStyleColor();
    if (m_buildDefaultLayout) {
        buildDefaultDockLayout(dockspaceId, defaultWorkspaceFor(m_workspaces.active().name()), m_workspaces.active(),
                               m_areaTypes);
        m_buildDefaultLayout = false;
    }
    drawMainMenu(context);
    pollFileDialog(context);
    drawPathDialog(context);
    drawUnsavedChangesDialog(context);
    drawRecoveryDialog(context);
    updateEyedropper(context);
    drawAreas(context);
    drawPrefabWindows(context);
    syncActiveRenderView(context);
    drawObjectPopups(context);
    drawNewCodeDialog(context);
    drawOperatorWindows(context);
    drawStatusBar(context.rendering.stats());
    publishMouseCapture();
    // Last, after every edit of this frame: the renderer draws next.
    bindPrefabWindowScenes(context);
}

void EditorSession::updateEyedropper(ApplicationContext& context) {
    if (m_eyedropper.cancellingClickHeld() && !ImGui::IsMouseDown(ImGuiMouseButton_Right) &&
        !ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        m_eyedropper.setCancellingClickHeld(false);
    }
    const EntityReferenceTarget* target = m_eyedropper.target();
    if (target == nullptr) {
        return;
    }
    const bool cancelKey = ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    const ResolvedEntityReference resolved =
        resolveEntityReference(context.scene, componentTypesFor(m_document.options()), *target);
    if (cancelKey || m_playMode.mode() != EditorMode::Edit || !resolved) {
        invokeOperator(context, "ui.eyedropper_cancel", {}, KeymapRegion::Global);
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            // The cancelling click must not also start a viewport camera drag
            // or open a context menu when it is released.
            m_eyedropper.setCancellingClickHeld(true);
            for (ViewportPanel* viewport : m_viewports.openViewports()) {
                viewport->suppressCameraUntilRightRelease();
            }
        }
        return;
    }
    // The hint follows the mouse on a translucent header, like the viewport labels.
    const std::string hint = "Pick " + resolved.field->presentation.label + ": click an entity (Esc cancels)";
    const float artPixel = editorArtPixel();
    const ImVec2 mouse = ImGui::GetMousePos();
    const ImVec2 textPosition{mouse.x + 8.0f * artPixel, mouse.y + 8.0f * artPixel};
    const ImVec2 textSize = ImGui::CalcTextSize(hint.c_str());
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    ThemeChrome::paintOverlayPanel(
        drawList, {textPosition.x - 3.0f * artPixel, textPosition.y - 2.0f * artPixel},
        {textPosition.x + textSize.x + 3.0f * artPixel, textPosition.y + textSize.y + 2.0f * artPixel});
    drawList->AddText(textPosition, toImU32(palette::activeObjectLabel), hint.c_str());
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
}

void EditorSession::unload(ApplicationContext& context) {
    // An unfinished smoke run releases its streaming layers (which share the
    // module's component types) and removes its files.
    m_smokeRunner.reset();
    // First: the crash handler must not save through a session that is going.
    if (m_crashRecoveryRegistered && m_crashRecoveryChanged) {
        m_crashRecoveryChanged({});
        m_crashRecoveryRegistered = false;
    }
    stopPlay(context);
    m_projectAssets.releaseThumbnailTextures(context.rendering);
    if (m_mouseCaptureChanged) {
        m_mouseCaptureChanged(MouseCapturePolicy::WhileRightMouseDown);
    }
    // The scene and document release module code before the module unloads;
    // parked documents (prefab mode) go first, unsaved prefab edits with them.
    context.audio.stopSceneVoices();
    context.physics.clear();
    m_documents.unwind(activeDocument(context));
    closeAllPrefabWindows(context);
    m_moduleHost.unload(context.scene, m_document);
    context.rendering.setGridOverlay(std::nullopt);
    context.rendering.clearOutlinedObjects();
    context.rendering.clearViewOverride();
    if (!m_workspaces.empty()) {
        m_workspaces.active().setLayout(captureImGuiLayout());
        saveWorkspaces();
    }
    saveSettings();
    saveComponentLayoutIfChanged();
}

void EditorSession::projectFilesMoved(ApplicationContext& context, std::span<const ProjectFileMove> moves) {
    m_documents.filesMoved(moves, context.assets, m_diagnostics);
    m_prefabWindows.filesMoved(moves, m_document.options().assetRoot, context.assets, m_diagnostics);
    if (m_document.path()) {
        if (std::optional<std::filesystem::path> moved =
                relocatedProjectPath(normalizedCatalogPath(*m_document.path()), moves)) {
            m_diagnostics.add(EditorDiagnosticSeverity::Info, "Editor",
                              "The open scene now lives at '" + moved->string() + "'.");
            m_document.relocate(std::move(*moved));
        }
    }
    // Play mode runs on a copy; the edit scene it restores resolves moved
    // assets by GUID.
    if (m_playMode.mode() != EditorMode::Edit) {
        return;
    }
    size_t relocated = 0;
    if (const SceneDocumentResult result =
            relocateSceneAssets(context.scene, context.assets, m_document.options(), moves, relocated);
        !result) {
        m_diagnostics.reportError("Could not follow moved assets in the open scene: " + result.error);
    } else if (relocated > 0) {
        // The file on disk may name the old paths without GUIDs (version 1,
        // or saved without a database), so the new paths need saving.
        m_document.markDirty();
    }
}

void EditorSession::projectFilesRemoved(ApplicationContext&, std::span<const std::filesystem::path> removed) {
    m_documents.filesRemoved(removed, m_diagnostics);
    m_prefabWindows.filesRemoved(removed, m_diagnostics);
    if (m_document.path() && isRemovedProjectPath(normalizedCatalogPath(*m_document.path()), removed)) {
        m_diagnostics.add(EditorDiagnosticSeverity::Warning, "Editor",
                          "The open scene's file was removed; save it to keep the scene.");
        m_document.detach();
    }
}

void EditorSession::undo(OperatorContext& context) {
    context.editing.undo();
    context.selection.removeMissing(context.editing.scene());
}

void EditorSession::undoSince(ApplicationContext& context, CommandHistoryMarker marker) {
    // Counted once: a failed undo is reported and must not repeat forever.
    OperatorContext level = levelOperatorContext(context, KeymapRegion::Global);
    for (size_t steps = m_history.stepsSince(marker); steps > 0; --steps) {
        undo(level);
    }
}

void EditorSession::redo(OperatorContext& context) {
    context.editing.redo();
    context.selection.removeMissing(context.editing.scene());
}

OperatorResult EditorSession::copySelection(OperatorContext& context) {
    if (const SceneDocumentResult result =
            SceneFragment::copy(context.editing.scene(), context.editing.assets(), context.editing.document().options(),
                                context.selection.ids(), m_clipboardFragment);
        !result) {
        return OperatorResult::failed("Copy failed: " + result.error);
    }
    ImGui::SetClipboardText(m_clipboardFragment.c_str());
    return OperatorResult::finished();
}

OperatorResult EditorSession::pasteClipboard(OperatorContext& context) {
    const char* systemClipboard = ImGui::GetClipboardText();
    const std::string_view fragment =
        systemClipboard != nullptr && *systemClipboard != '\0' ? systemClipboard : m_clipboardFragment;
    if (fragment.empty()) {
        return OperatorResult::cancelled("The clipboard is empty");
    }
    std::vector<SceneEntityId> pasted;
    if (const SceneDocumentResult result =
            SceneFragment::paste(fragment, context.editing.scene(), context.editing.assets(),
                                 context.editing.document().options(), entt::null, pasted);
        !result) {
        return OperatorResult::failed("Paste failed: " + result.error);
    }
    context.selection.assign(std::move(pasted));
    return OperatorResult::finished();
}
