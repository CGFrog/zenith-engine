#include "EditorSession.hpp"

#include "editor/application/EntityCommands.hpp"
#include "editor/areas/AreaWindows.hpp"
#include "editor/areas/DefaultWorkspaces.hpp"
#include "editor/document/DefaultScene.hpp"
#include "editor/document/SceneFragment.hpp"
#include "editor/menus/DefaultMenus.hpp"
#include "editor/operators/DefaultKeymap.hpp"
#include "editor/operators/EditorOperatorFamilies.hpp"
#include "editor/panels/SimulateSystemPicker.hpp"
#include "editor/settings/SimulateSystemSettings.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/EditorWidgets.hpp"
#include "engine/module/GameEvents.hpp"
#include "engine/scene/ComponentTypes.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <ImGuizmo.h>
#include <SDL3/SDL_stdinc.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <utility>

namespace {
constexpr const char* sceneExtension = ".zenith-scene";

// Native dialogs need an existing absolute folder to start in. A trailing
// separator marks the location as a folder with no suggested file name.
std::filesystem::path dialogStartLocation(const std::optional<std::filesystem::path>& documentPath,
                                          const std::filesystem::path& sceneRoot) {
    std::error_code error;
    if (documentPath) {
        std::filesystem::path absolute = std::filesystem::absolute(*documentPath, error);
        if (!error && std::filesystem::is_directory(absolute.parent_path(), error)) {
            return absolute.make_preferred();
        }
    }
    std::filesystem::path folder = std::filesystem::absolute(sceneRoot, error);
    if (error || !std::filesystem::is_directory(folder, error)) {
        return {};
    }
    return (folder / "").make_preferred();
}
} // namespace

EditorSession::EditorSession(std::filesystem::path settingsDirectory, std::filesystem::path assetRoot,
                             std::filesystem::path sceneRoot, EditorRenderSettings renderSettings,
                             EditorAppearance& appearance, EditorSceneLoader initialSceneLoader,
                             std::filesystem::path gameModule, EditorProjectServices projectServices)
    : m_settings(settingsDirectory / "settings.ini")
    , m_document(assetRoot)
    , m_recovery(settingsDirectory / (std::string("recovery") + sceneExtension))
    , m_projectFile(projectServices.projectFile)
    , m_playServices(projectServices.play)
    , m_standalone(m_playServices.makeGameProcess ? m_playServices.makeGameProcess() : makeGameProcess())
    , m_mouseCaptureChanged(projectServices.mouseCaptureChanged)
    , m_fileDialog("Zenith scene", sceneExtension)
    , m_operatorRunner(m_operators, m_history)
    , m_gameModulePath(std::move(gameModule))
    , m_simulateSystemsPath(settingsDirectory / "simulate-systems.json")
    , m_keymap(defaultEditorKeymap())
    , m_keymapPath(settingsDirectory / "keymap.json")
    , m_appearance(appearance)
    , m_projectAssets(std::move(assetRoot), sceneRoot, settingsDirectory / "asset-browser.json")
    , m_renderSettings(std::move(renderSettings))
    , m_componentLayoutPath(settingsDirectory / "component-inspector.json")
    , m_workspacesPath(settingsDirectory / "workspaces.json")
    , m_legacyLayoutPath(settingsDirectory / "layout.ini")
    , m_sceneRoot(std::move(sceneRoot))
    , m_initialSceneLoader(std::move(initialSceneLoader))
    , m_leaveForProjectHub(std::move(projectServices.leaveForProjectHub))
    , m_smokeScratchDirectory(settingsDirectory) {
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
    m_buildPanel.setProjectBuild(std::move(projectServices.build));
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
            .componentEditors = &m_componentEditors};
}

OperatorContext EditorSession::operatorContext(ApplicationContext& context, KeymapRegion region) {
    return {.editing = editing(context),
            .selection = m_selection,
            .mode = m_playMode.mode(),
            .region = region,
            .app = &context};
}

EditorOperatorServices EditorSession::operatorServices() {
    return {.document = m_document,
            .history = m_history,
            .eyedropper = m_eyedropper,
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
            .tools = m_settings.tools(),
            .projectFile = m_renderSettings.settings().projectFile(),
            .revealAsset = [this](const std::filesystem::path& asset) { m_projectAssets.requestReveal(asset); },
            .simulateMaskChanged = [this] { saveSimulateSystems(); },
            .documentCommands = *this,
            .playCommands = *this,
            .objectModeCommands = *this,
            .windowCommands = *this};
}

bool EditorSession::documentCommandsAvailable(const OperatorContext& context) const {
    return context.editMode() && !m_fileDialog.pending() && !m_activeTransform;
}

void EditorSession::requestSaveAs() {
    requestFileDialog(FileDialogKind::Save);
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

OperatorResult EditorSession::invokeOperator(ApplicationContext& context, std::string_view operatorId,
                                             std::string_view properties, KeymapRegion region) {
    OperatorContext operatorContext = this->operatorContext(context, region);
    return m_operatorRunner.invokeWithJson(operatorId, operatorContext, properties);
}

OperatorResult EditorSession::invokeOperator(ApplicationContext& context, std::string_view operatorId,
                                             const OperatorProperties& properties) {
    OperatorContext operatorContext = this->operatorContext(context, KeymapRegion::Global);
    return m_operatorRunner.invoke(operatorId, operatorContext, &properties);
}

bool EditorSession::operatorAvailable(ApplicationContext& context, std::string_view operatorId) {
    const Operator* op = m_operators.find(operatorId);
    return op != nullptr && op->canRun(operatorContext(context, KeymapRegion::Global));
}

std::string EditorSession::operatorShortcut(std::string_view operatorId, std::string_view properties) const {
    return m_keymap.shortcutLabel(operatorId, properties);
}

void EditorSession::dispatchKeymap(ApplicationContext& context, KeymapRegion region) {
    // Global keys work while any panel has focus; region keys already require
    // the mouse over the region.
    if (const std::optional<KeymapInvocation> invocation = m_keymapDispatcher.pressKeys(m_keymap, region)) {
        invokeOperator(context, invocation->operatorId, invocation->properties, invocation->region);
    }
}

void EditorSession::updateKeymapGesture(ApplicationContext& context) {
    for (const KeymapInvocation& invocation : m_keymapDispatcher.update(m_keymap)) {
        invokeOperator(context, invocation.operatorId, invocation.properties, invocation.region);
    }
}

uint32_t EditorSession::dispatchMouseChords(ApplicationContext& context, KeymapRegion region, uint32_t allowedButtons) {
    if (m_activeTransform || m_fileDialog.pending() || m_keymapEditor.capturing()) {
        allowedButtons = 0;
    }
    return m_keymapDispatcher.dispatchMouse(
        m_keymap, region, allowedButtons, [this, &context](const KeymapInvocation& invocation) {
            invokeOperator(context, invocation.operatorId, invocation.properties, invocation.region);
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
    const bool enabled = op->canRun(operatorContext(context, KeymapRegion::Global));
    const std::string shortcut = m_keymap.shortcutLabel(operatorId, properties);
    if (ImGui::MenuItem(label != nullptr ? label : op->label.c_str(), shortcut.empty() ? nullptr : shortcut.c_str(),
                        selected, enabled)) {
        invokeOperator(context, operatorId, properties, KeymapRegion::Global);
    }
    if (!op->description.empty()) {
        ImGui::SetItemTooltip("%s", op->description.c_str());
    }
}

void EditorSession::drawOperatorWindows(ApplicationContext& context) {
    if (const std::optional<std::string> chosen =
            m_operatorSearch.draw(m_operators, m_keymap, operatorContext(context, KeymapRegion::Global))) {
        invokeOperator(context, *chosen, {}, KeymapRegion::Global);
    }
    if (m_keymapEditor.draw(m_showKeymapEditor, m_keymap, m_operators)) {
        saveKeymap();
    }
    if (m_preferences.draw(m_settings, preferencesContext(context))) {
        saveSettings();
    }
    if (m_playMode.mode() != EditorMode::Edit || m_activeTransform) {
        return;
    }
    if (const std::optional<OperatorProperties> edited =
            m_lastOperatorPanel.draw(m_operatorRunner.lastOperator(), lastOperatorAnchor())) {
        OperatorContext operatorContext = this->operatorContext(context, KeymapRegion::Global);
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
    m_projectAssets.refresh(m_diagnostics);
    m_openRecoveryDialog = m_recovery.available();

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
    beginViewportFrame(context);
    if (m_smokeRunner) {
        updateSmokeScenario(context);
    }
    updateAutosave(context, deltaSeconds);
    updatePackaging();
    updateStandalone(context);
    // Once per frame, however many asset browsers are open.
    m_projectAssets.pollChanges(m_diagnostics, projectAssetActions(context), deltaSeconds);
    updateAreas(context, input, deltaSeconds);
    updateGameModule(context, input, deltaSeconds);
    syncAreaTypes();
    saveComponentLayoutIfChanged();
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
        .filesMoved = [this, &context](std::span<const ProjectFileMove> moves) { projectFilesMoved(context, moves); },
        .filesRemoved =
            [this, &context](std::span<const std::filesystem::path> removed) { projectFilesRemoved(context, removed); },
    };
}

void EditorSession::updateAutosave(ApplicationContext& context, float deltaSeconds) {
    // An interval of 0 turns autosave off (Edit > Preferences > Saving).
    const float intervalSeconds = m_settings.autosave().intervalSeconds;
    if (intervalSeconds <= 0.0f || !m_document.dirty() || m_playMode.mode() != EditorMode::Edit) {
        m_autosaveElapsed = 0.0f;
        return;
    }
    m_autosaveElapsed += deltaSeconds;
    if (m_autosaveElapsed >= intervalSeconds) {
        m_autosaveElapsed = 0.0f;
        if (const SceneDocumentResult result = m_recovery.autosave(context.scene, context.assets, m_document.options());
            !result) {
            m_diagnostics.reportError("Autosave failed: " + result.error);
        }
    }
}

void EditorSession::fixedUpdate(ApplicationContext& context, const InputState& input, float deltaSeconds) {
    // Fixed updates run only while simulating (including a paused single
    // step); Edit mode keeps its own fixed clock in updateGameModule.
    // Simulate passes no input; the host enforces that too.
    if (m_playMode.mode() != EditorMode::Edit) {
        const SimulationMode mode = m_playMode.simulationMode();
        m_moduleHost.runSystems(SystemPhase::FixedUpdate, context.scene, &context, mode, deltaSeconds,
                                mode == SimulationMode::Simulate ? nullptr : &gameInput(input));
    }
}

void EditorSession::afterPhysicsStep(ApplicationContext&) {}

void EditorSession::lateUpdate(ApplicationContext& context, const InputState&, float deltaSeconds) {
    if (m_playMode.mode() == EditorMode::Edit || !m_playMode.advancesThisFrame()) {
        return;
    }
    const SimulationMode mode = m_playMode.simulationMode();
    m_moduleHost.runSystems(SystemPhase::LateUpdate, context.scene, &context, mode,
                            simulatedSeconds(m_playMode.simulationClock(), deltaSeconds), &m_frameGameInput);
}

void EditorSession::drawGameViewUi(ApplicationContext& context) {
    ViewportPanel* viewport = gameViewport();
    if (viewport == nullptr || m_playMode.mode() == EditorMode::Edit || m_playMode.simulating()) {
        return;
    }
    // The game sees the pointer relative to its view, as it would its window.
    InputState input = m_frameGameInput;
    input.mousePosition -= viewport->position();
    const glm::vec2 size = glm::max(viewport->size(), glm::vec2(1.0f));
    m_gameViewUi.beginFrame({static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y)}, input);
    m_moduleHost.runSystems(SystemPhase::Ui, context.scene, &context, m_playMode.simulationMode(), 0.0f, &input,
                            &m_gameViewUi);
    viewport->setGameUi(&m_gameViewUi);
}

bool EditorSession::fixedUpdatesEnabled() const {
    return m_playMode.fixedUpdatesEnabled();
}

bool EditorSession::beginFixedUpdate() {
    return m_playMode.beginFixedUpdate();
}

SimulationClock EditorSession::simulationClock() const {
    return m_playMode.simulationClock();
}

void EditorSession::requestQuit(ApplicationContext& context) {
    requestClose(context);
}

void EditorSession::drawUi(ApplicationContext& context) {
    // Every system of this frame ran in update and fixedUpdate; publish
    // their timings for the System Profiler areas drawn below.
    m_moduleHost.finishSystemFrame();
    // The engine has run this frame's fixed steps, so a frame step is done;
    // play.step below requests the next one.
    m_playMode.finishFrame();
    // Before the areas, so the game view draws this frame's game UI.
    drawGameViewUi(context);
    ImGuizmo::BeginFrame();
    m_themeChrome.setModeTint(playModeTint(m_playMode.mode(), m_playMode.simulating()));
    m_themeChrome.beginFrame(context.rendering);
    // The dockspace host is transparent so the wallpaper shows between areas.
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
    syncActiveRenderView(context);
    drawObjectPopups(context);
    drawOperatorWindows(context);
    drawStatusBar(context.rendering.stats());
    publishMouseCapture();
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
    // The hint follows the mouse on smoked glass, like the viewport labels.
    const std::string hint = "Pick " + resolved.field->presentation.label + ": click an entity (Esc cancels)";
    const float artPixel = editorArtPixel();
    const ImVec2 mouse = ImGui::GetMousePos();
    const ImVec2 textPosition{mouse.x + 8.0f * artPixel, mouse.y + 8.0f * artPixel};
    const ImVec2 textSize = ImGui::CalcTextSize(hint.c_str());
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    ThemeChrome::paintSmokedGlass(
        drawList, {textPosition.x - 3.0f * artPixel, textPosition.y - 2.0f * artPixel},
        {textPosition.x + textSize.x + 3.0f * artPixel, textPosition.y + textSize.y + 2.0f * artPixel});
    drawList->AddText(textPosition, toImU32(palette::activeObjectLabel), hint.c_str());
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
}

void EditorSession::unload(ApplicationContext& context) {
    // An unfinished smoke run releases its streaming layers (which share the
    // module's component types) and removes its files.
    m_smokeRunner.reset();
    stopPlay(context);
    if (m_mouseCaptureChanged) {
        m_mouseCaptureChanged(MouseCapturePolicy::WhileRightMouseDown);
    }
    // The scene and document release module code before the module unloads.
    context.audio.stopSceneVoices();
    context.physics.clear();
    m_moduleHost.unload(context.scene, m_document);
    context.rendering.setGridOverlay(std::nullopt);
    context.rendering.clearOutlinedObjects();
    context.rendering.clearViewOverride();
    m_themeChrome.release(context.rendering);
    if (!m_workspaces.empty()) {
        m_workspaces.active().setLayout(captureImGuiLayout());
        saveWorkspaces();
    }
    saveSettings();
    saveComponentLayoutIfChanged();
}

void EditorSession::handleGlobalShortcuts(ApplicationContext& context) {
    // Operator polls decide which shortcuts apply in the current mode.
    if (ImGui::GetIO().WantTextInput || m_keymapEditor.capturing()) {
        m_keymapDispatcher.cancel();
        return;
    }
    // Before any panel, so a press followed from a region finishes even
    // when the mouse has left it.
    updateKeymapGesture(context);
    dispatchKeymap(context, KeymapRegion::Global);
}

void EditorSession::drawMainMenu(ApplicationContext& context) {
    handleGlobalShortcuts(context);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, toImVec4(palette::transparent));
    const bool menuBarOpen = ImGui::BeginMainMenuBar();
    ImGui::PopStyleColor();
    if (!menuBarOpen) {
        return;
    }
    m_themeChrome.paintBarWindow();

    if (ImGui::BeginMenu("File")) {
        operatorMenuItem(context, "file.new", "New");
        operatorMenuItem(context, "file.open", "Open...");
        operatorMenuItem(context, "file.save", "Save");
        operatorMenuItem(context, "file.save_as", "Save As...");
        ImGui::Separator();
        operatorMenuItem(context, "wm.open_project", "Open Project...");
        operatorMenuItem(context, "wm.project_hub", "Project Hub");
        drawBuildPackageMenu(context);
        ImGui::Separator();
        operatorMenuItem(context, "file.quit", "Quit");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        const bool editing = m_playMode.mode() == EditorMode::Edit;
        operatorMenuItem(context, "edit.undo", "Undo");
        operatorMenuItem(context, "edit.redo", "Redo");
        ImGui::Separator();
        operatorMenuItem(context, "edit.copy", "Copy");
        operatorMenuItem(context, "edit.paste", "Paste");
        operatorMenuItem(context, "object.duplicate", "Duplicate");
        operatorMenuItem(context, "object.delete", "Delete");
        ImGui::Separator();
        operatorMenuItem(context, "object.select_all", "Select All", R"({"action":"select"})");
        operatorMenuItem(context, "object.select_all", "Deselect All", R"({"action":"deselect"})");
        operatorMenuItem(context, "object.select_all", "Invert Selection", R"({"action":"invert"})");
        ImGui::Separator();
        if (ImGui::BeginMenu("Clear", editing && !m_selection.empty())) {
            operatorMenuItem(context, "object.location_clear", "Location");
            operatorMenuItem(context, "object.rotation_clear", "Rotation");
            operatorMenuItem(context, "object.scale_clear", "Scale");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Add", editing)) {
            if (const std::optional<ResolvedMenu> add = m_menus.resolve(
                    {.menu = addMenuId, .category = {}}, operatorContext(context, KeymapRegion::Global))) {
                EditorContext editor = editorContext(context);
                if (const std::optional<MenuChoice> chosen = drawMenuItems(editor, *add)) {
                    editor.invoke(chosen->operatorId, chosen->properties);
                }
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        operatorMenuItem(context, "wm.search_menu", "Search Operators...");
        operatorMenuItem(context, "wm.keymap_editor", "Keymap...");
        operatorMenuItem(context, "wm.preferences", "Preferences...");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Object")) {
        if (ImGui::BeginMenu("Prefab")) {
            operatorMenuItem(context, "prefab.create_from_selection", "Create from Selection");
            ImGui::Separator();
            operatorMenuItem(context, "prefab.apply", "Apply");
            operatorMenuItem(context, "prefab.revert", "Revert");
            operatorMenuItem(context, "prefab.unpack", "Unpack");
            operatorMenuItem(context, "prefab.select_asset", "Select Prefab Asset");
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        drawAreaMenuItems(context);
        ImGui::Separator();
        operatorMenuItem(context, "view3d.frame_selected", "Frame Selected");
        operatorMenuItem(context, "view3d.frame_all", "Frame All");
        if (ImGui::BeginMenu("Viewpoint")) {
            for (const char* axis : {"front", "back", "right", "left", "top", "bottom"}) {
                const std::string label = labelFromKey(axis);
                operatorMenuItem(context, "view3d.view_axis", label.c_str(), nlohmann::json{{"type", axis}}.dump());
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Play")) {
        operatorMenuItem(context, "play.start", "Play");
        for (const PlayTargetInfo& target : playTargets()) {
            const std::string label = "Play in " + std::string(target.label);
            const std::string properties = nlohmann::json{{"target", target.key}}.dump();
            operatorMenuItem(context, "play.start", label.c_str(), properties, target.target == m_lastPlayTarget);
        }
        operatorMenuItem(context, "play.simulate", "Simulate");
        ImGui::Separator();
        operatorMenuItem(context, "play.toggle_pause", m_playMode.mode() == EditorMode::Paused ? "Resume" : "Pause");
        operatorMenuItem(context, "play.step", "Step Frame");
        operatorMenuItem(context, "play.stop", "Stop");
        ImGui::Separator();
        operatorMenuItem(context, "play.apply_to_edit", "Apply Selection to Edit Scene");
        if (ImGui::BeginMenu("Simulate Systems")) {
            EditorContext editor = editorContext(context);
            drawSimulateSystemPicker(editor, m_moduleHost);
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    drawModuleMenu(context);
    drawPlaybackToolbar(context);
    drawWorkspaceTabs(context);
    ImGui::EndMainMenuBar();
}

void EditorSession::drawStatusBar(const RenderStats& stats) {
    const float height = ImGui::GetFrameHeight() + editorArtPixel() * 2.0f;
    // A viewport side bar reserves its strip, so the dockspace ends above it
    // from the next frame on, as it does below the main menu bar.
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, toImVec4(palette::transparent));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {editorArtPixel() * 4.0f, editorArtPixel()});
    const bool open =
        ImGui::BeginViewportSideBar("##EditorStatus", ImGui::GetMainViewport(), ImGuiDir_Down, height, flags);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if (open) {
        m_themeChrome.paintBarWindow();
        constexpr const char* modeNames[] = {"Select", "Move", "Rotate", "Scale"};
        constexpr const char* spaceNames[] = {"Global", "Local"};
        std::array<char, 160> text{};
        const auto segment = [](const char* caption, ThemeColor ink) {
            ImGui::SameLine();
            widgets::CaptionText(caption, ink);
        };
        widgets::CaptionText(m_document.displayName().c_str(), palette::captionInk);
        if (m_diagnostics.hasUnacknowledgedError()) {
            segment("Diagnostics available", palette::warning);
        } else {
            segment("Ready", palette::success);
        }
        // The transform tools are the active viewport's.
        if (const ViewportPanel* activeViewport = m_viewports.active()) {
            const TransformToolSettings& tools = activeViewport->tools();
            std::snprintf(text.data(), text.size(), "Mode: %s  Space: %s  Snap: %s",
                          modeNames[static_cast<int>(tools.gizmo)], spaceNames[static_cast<int>(tools.space)],
                          tools.snapping ? "On" : "Off");
            segment(text.data(), palette::captionInk);
        } else {
            segment("No viewport", palette::inkMuted);
        }
        std::snprintf(text.data(), text.size(), "Selection: %zu  Draws %u  Visible %u  GPU %.2f ms", m_selection.size(),
                      stats.drawCalls, stats.visibleCount, stats.gpuFrameMilliseconds);
        segment(text.data(), palette::captionInk);
        ImGui::SameLine();
        drawPlayModePill();
        if (m_moduleHost.status() != GameModuleStatus::None) {
            segment(gameModuleStatusText().c_str(), palette::captionInk);
        }
    }
    ImGui::End();
}

void EditorSession::drawPlayModePill() const {
    using namespace palette;
    switch (m_playMode.mode()) {
    case EditorMode::Edit:
        if (m_standalone.running()) {
            widgets::StatusPill(EditorIcon::Play, "Standalone", buttonActiveTop, buttonActiveBottom, activeInk);
        } else {
            widgets::StatusPill(EditorIcon::Stop, "Edit", buttonTop, buttonBottom, ink);
        }
        break;
    case EditorMode::Play:
        widgets::StatusPill(EditorIcon::Play, "Play", buttonActiveTop, buttonActiveBottom, activeInk);
        break;
    case EditorMode::Simulate:
        widgets::StatusPill(EditorIcon::Play, "Simulate", buttonActiveTop, buttonActiveBottom, activeInk);
        break;
    case EditorMode::Paused:
        widgets::StatusPill(EditorIcon::Pause, m_playMode.simulating() ? "Simulate Paused" : "Paused", buttonHoverTop,
                            buttonHoverBottom, ink);
        break;
    }
}

void EditorSession::drawPathDialog(ApplicationContext& context) {
    if (m_openPathDialog) {
        ImGui::OpenPopup(m_pathDialogSaves ? "Save Scene As" : "Open Scene");
        m_openPathDialog = false;
    }

    bool open = true;
    const char* title = m_pathDialogSaves ? "Save Scene As" : "Open Scene";
    if (!ImGui::BeginPopupModal(title, &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::InputText("Path", m_pathBuffer.data(), m_pathBuffer.size());
    if (widgets::Button(m_pathDialogSaves ? "Save" : "Open")) {
        const std::filesystem::path path(m_pathBuffer.data());
        const bool completed = m_pathDialogSaves ? completeSaveAs(context, path) : completeOpen(context, path);
        if (completed) {
            ImGui::CloseCurrentPopup();
            if (m_pathDialogSaves && m_continuePendingAfterSaveAs) {
                m_continuePendingAfterSaveAs = false;
                finishPendingAction(context);
            }
        }
    }
    ImGui::SameLine();
    if (widgets::Button("Cancel")) {
        m_continuePendingAfterSaveAs = false;
        m_pendingAction = PendingAction::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void EditorSession::pollFileDialog(ApplicationContext& context) {
    const std::optional<FileDialogOutcome> outcome = m_fileDialog.poll();
    if (!outcome) {
        return;
    }
    if (outcome->status == FileDialogStatus::Failed) {
        // The typed-path prompt still works when the platform has no dialog.
        m_diagnostics.reportError(outcome->error);
        openPathDialog(outcome->kind);
        return;
    }
    if (outcome->status == FileDialogStatus::Cancelled) {
        m_continuePendingAfterSaveAs = false;
        m_pendingAction = PendingAction::None;
        return;
    }
    if (outcome->kind == FileDialogKind::Open) {
        completeOpen(context, outcome->path);
        return;
    }
    if (completeSaveAs(context, outcome->path) && m_continuePendingAfterSaveAs) {
        m_continuePendingAfterSaveAs = false;
        finishPendingAction(context);
    }
}

bool EditorSession::completeOpen(ApplicationContext& context, const std::filesystem::path& path) {
    if (const SceneDocumentResult result = m_document.open(path, context.scene, context.assets); !result) {
        m_diagnostics.reportError(result.error);
        return false;
    }
    m_history.crossBoundary(CommandHistoryBoundary::OpenDocument);
    m_selection.clear();
    onDocumentPathCommitted();
    if (const std::string warning = m_document.takeEditorDataWarning(); !warning.empty()) {
        m_diagnostics.add(EditorDiagnosticSeverity::Warning, "Editor", warning);
    }
    return true;
}

bool EditorSession::completeSaveAs(ApplicationContext& context, const std::filesystem::path& path) {
    if (const SceneDocumentResult result = m_document.saveAs(path, context.scene, context.assets); !result) {
        m_diagnostics.reportError(result.error);
        return false;
    }
    m_history.crossBoundary(CommandHistoryBoundary::SaveDocument);
    onDocumentPathCommitted();
    return true;
}

void EditorSession::onDocumentPathCommitted() {
    m_document.setDirty(m_history.dirty());
    m_diagnostics.acknowledgeErrors();
    std::string recoveryError;
    if (!m_recovery.discard(recoveryError)) {
        m_diagnostics.reportError(recoveryError);
    }
}

void EditorSession::drawUnsavedChangesDialog(ApplicationContext& context) {
    if (m_openUnsavedDialog) {
        ImGui::OpenPopup("Unsaved Changes");
        m_openUnsavedDialog = false;
    }
    if (!ImGui::BeginPopupModal("Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::TextWrapped("%s has unsaved changes.", m_document.displayName().c_str());
    if (widgets::Button("Save")) {
        if (m_document.path()) {
            if (saveDocument(context)) {
                ImGui::CloseCurrentPopup();
                finishPendingAction(context);
            }
        } else {
            m_continuePendingAfterSaveAs = true;
            requestFileDialog(FileDialogKind::Save);
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (widgets::Button("Discard")) {
        std::string ignored;
        m_recovery.discard(ignored);
        ImGui::CloseCurrentPopup();
        finishPendingAction(context);
    }
    ImGui::SameLine();
    if (widgets::Button("Cancel")) {
        m_pendingAction = PendingAction::None;
        m_pendingOpenPath.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void EditorSession::drawRecoveryDialog(ApplicationContext& context) {
    if (m_openRecoveryDialog) {
        ImGui::OpenPopup("Recover Autosave");
        m_openRecoveryDialog = false;
    }
    if (!ImGui::BeginPopupModal("Recover Autosave", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::TextWrapped("A recovery document is available from the previous editor session.");
    if (widgets::Button("Recover")) {
        if (const SceneDocumentResult result = m_recovery.recover(context.scene, context.assets, m_document.options());
            !result) {
            m_diagnostics.reportError("Recovery failed: " + result.error);
        } else {
            m_history.crossBoundary(CommandHistoryBoundary::OpenDocument);
            m_document.markDirty();
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (widgets::Button("Discard Recovery")) {
        std::string error;
        if (!m_recovery.discard(error)) {
            m_diagnostics.reportError(error);
        } else {
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndPopup();
}

void EditorSession::requestFileDialog(FileDialogKind kind) {
    m_fileDialog.request(kind, dialogStartLocation(m_document.path(), m_sceneRoot));
}

void EditorSession::openPathDialog(FileDialogKind kind) {
    m_pathDialogSaves = kind == FileDialogKind::Save;
    m_pathBuffer.fill('\0');
    if (m_document.path()) {
        const std::string path = m_document.path()->string();
        const size_t count = std::min(path.size(), m_pathBuffer.size() - 1);
        std::copy_n(path.data(), count, m_pathBuffer.data());
    }
    m_openPathDialog = true;
}

void EditorSession::requestNew(ApplicationContext& context) {
    m_pendingOpenPath.reset();
    m_pendingAction = PendingAction::NewDocument;
    if (m_document.dirty()) {
        m_openUnsavedDialog = true;
    } else {
        finishPendingAction(context);
    }
}

void EditorSession::requestOpen(ApplicationContext& context) {
    m_pendingOpenPath.reset();
    m_pendingAction = PendingAction::OpenDocument;
    if (m_document.dirty()) {
        m_openUnsavedDialog = true;
    } else {
        finishPendingAction(context);
    }
}

void EditorSession::requestOpenScene(ApplicationContext& context, std::filesystem::path path) {
    m_pendingOpenPath = std::move(path);
    m_pendingAction = PendingAction::OpenDocument;
    if (m_document.dirty()) {
        m_openUnsavedDialog = true;
    } else {
        finishPendingAction(context);
    }
}

void EditorSession::projectFilesMoved(ApplicationContext& context, std::span<const ProjectFileMove> moves) {
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
    if (m_document.path() && isRemovedProjectPath(normalizedCatalogPath(*m_document.path()), removed)) {
        m_diagnostics.add(EditorDiagnosticSeverity::Warning, "Editor",
                          "The open scene's file was removed; save it to keep the scene.");
        m_document.detach();
    }
}

void EditorSession::requestClose(ApplicationContext& context) {
    m_pendingOpenPath.reset();
    stopPlay(context);
    m_pendingAction = PendingAction::CloseEditor;
    if (m_document.dirty()) {
        m_openUnsavedDialog = true;
    } else {
        finishPendingAction(context);
    }
}

void EditorSession::requestProjectHub(ApplicationContext& context, ProjectHubEntry entry) {
    m_pendingOpenPath.reset();
    stopPlay(context);
    m_pendingAction = PendingAction::LeaveForProjectHub;
    m_pendingHubEntry = entry;
    if (m_document.dirty()) {
        m_openUnsavedDialog = true;
    } else {
        finishPendingAction(context);
    }
}

void EditorSession::finishPendingAction(ApplicationContext& context) {
    const PendingAction action = m_pendingAction;
    m_pendingAction = PendingAction::None;
    if (action == PendingAction::NewDocument) {
        m_document.createNew(context.scene);
        if (const SceneResult result =
                populateDefaultScene(context.scene, context.assets, m_document.options().assetRoot);
            !result) {
            m_diagnostics.reportError(result.error);
        }
        m_history.crossBoundary(CommandHistoryBoundary::NewDocument);
        m_selection.clear();
        std::string ignored;
        m_recovery.discard(ignored);
    } else if (action == PendingAction::OpenDocument) {
        if (m_pendingOpenPath) {
            // A scene chosen in the asset browser opens without a dialog.
            const std::filesystem::path path = std::move(*m_pendingOpenPath);
            m_pendingOpenPath.reset();
            completeOpen(context, path);
        } else {
            requestFileDialog(FileDialogKind::Open);
        }
    } else if (action == PendingAction::CloseEditor) {
        context.control.requestQuit();
    } else if (action == PendingAction::LeaveForProjectHub && m_leaveForProjectHub) {
        m_leaveForProjectHub(context, m_pendingHubEntry);
    }
}

bool EditorSession::saveDocument(ApplicationContext& context) {
    if (!m_document.path()) {
        requestFileDialog(FileDialogKind::Save);
        return false;
    }
    if (const SceneDocumentResult result = m_document.save(context.scene, context.assets); !result) {
        m_diagnostics.reportError("Save failed: " + result.error);
        return false;
    }
    m_history.crossBoundary(CommandHistoryBoundary::SaveDocument);
    std::string recoveryError;
    if (!m_recovery.discard(recoveryError)) {
        m_diagnostics.reportError(recoveryError);
    }
    m_diagnostics.acknowledgeErrors();
    return true;
}

void EditorSession::undo(ApplicationContext& context) {
    editing(context).undo();
    m_selection.removeMissing(context.scene);
}

void EditorSession::undoSince(ApplicationContext& context, CommandHistoryMarker marker) {
    // Counted once: a failed undo is reported and must not repeat forever.
    for (size_t steps = m_history.stepsSince(marker); steps > 0; --steps) {
        undo(context);
    }
}

void EditorSession::redo(ApplicationContext& context) {
    editing(context).redo();
    m_selection.removeMissing(context.scene);
}

OperatorResult EditorSession::copySelection(ApplicationContext& context) {
    if (const SceneDocumentResult result = SceneFragment::copy(context.scene, context.assets, m_document.options(),
                                                               m_selection.ids(), m_clipboardFragment);
        !result) {
        return OperatorResult::failed("Copy failed: " + result.error);
    }
    ImGui::SetClipboardText(m_clipboardFragment.c_str());
    return OperatorResult::finished();
}

OperatorResult EditorSession::pasteClipboard(ApplicationContext& context) {
    const char* systemClipboard = ImGui::GetClipboardText();
    const std::string_view fragment =
        systemClipboard != nullptr && *systemClipboard != '\0' ? systemClipboard : m_clipboardFragment;
    if (fragment.empty()) {
        return OperatorResult::cancelled("The clipboard is empty");
    }
    std::vector<SceneEntityId> pasted;
    if (const SceneDocumentResult result =
            SceneFragment::paste(fragment, context.scene, context.assets, m_document.options(), entt::null, pasted);
        !result) {
        return OperatorResult::failed("Paste failed: " + result.error);
    }
    m_selection.assign(std::move(pasted));
    return OperatorResult::finished();
}

bool EditorSession::enterPlayMode(ApplicationContext& context, EditorRuntime runtime) {
    context.physics.clear();
    context.audio.stopSceneVoices();
    if (const SceneDocumentResult result =
            m_playMode.enter(context.scene, context.assets, m_document.options(), m_selection.ids(), runtime);
        !result) {
        m_diagnostics.reportError(std::string(runtime == EditorRuntime::Simulate ? "Could not enter Simulate: "
                                                                                 : "Could not enter Play mode: ") +
                                  result.error);
        return false;
    }
    m_moduleHost.startGameplay(context.assets, m_document.options(), context.rendering.debugDraw());
    if (GameServices* services = m_moduleHost.gameServices()) {
        services->events().publish(SceneLoaded{});
    }
    m_history.crossBoundary(CommandHistoryBoundary::EnterPlayMode);
    m_diagnostics.acknowledgeErrors();
    return true;
}

void EditorSession::stopPlayMode(ApplicationContext& context) {
    if (m_playMode.mode() == EditorMode::Edit) {
        return;
    }
    context.audio.stopSceneVoices();
    context.physics.clear();
    endGameView();
    m_moduleHost.stopGameplay();
    std::vector<SceneEntityId> restoredSelection;
    m_playMode.stop(context.scene, restoredSelection);
    m_selection.assign(std::move(restoredSelection));
    m_history.crossBoundary(CommandHistoryBoundary::LeavePlayMode);
    m_document.setDirty(m_history.dirty());
}
