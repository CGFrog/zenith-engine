#include "EditorSession.hpp"

#include "editor/application/EditorTheme.hpp"
#include "editor/application/EntityCommands.hpp"
#include "editor/document/DefaultScene.hpp"
#include "editor/document/SceneFragment.hpp"
#include "editor/operators/DefaultKeymap.hpp"
#include "editor/operators/ObjectOperators.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <ImGuizmo.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_video.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <iostream>
#include <limits>
#include <utility>

namespace {
// Layouts older than version 3 lack the Graphics panel below the Inspector;
// version 4 docks the Build panel beside Diagnostics.
constexpr uint32_t currentLayoutVersion = 4;
constexpr const char* sceneExtension = ".zenith-scene";
constexpr float editorFontPixelSize = 15.0f;
constexpr float autosaveIntervalSeconds = 30.0f;

struct PanelProperties {
    // The panel's window title, for example "Inspector".
    std::string panel;
};

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
                             std::filesystem::path sceneRoot, std::filesystem::path fontPath,
                             EditorRenderSettings renderSettings, EditorSceneLoader initialSceneLoader,
                             std::filesystem::path gameModule)
    : m_settings(settingsDirectory / "settings.ini")
    , m_document(assetRoot)
    , m_recovery(settingsDirectory / (std::string("recovery") + sceneExtension))
    , m_fileDialog("Zenith scene", sceneExtension)
    , m_operatorRunner(m_operators, m_history)
    , m_gameModulePath(std::move(gameModule))
    , m_keymap(defaultEditorKeymap())
    , m_keymapPath(settingsDirectory / "keymap.json")
    , m_assetBrowser(std::move(assetRoot), sceneRoot, settingsDirectory / "asset-browser.json")
    , m_graphics(std::move(renderSettings))
    , m_panels{{{&m_viewport, &EditorPanelVisibility::viewport},
                {&m_hierarchy, &EditorPanelVisibility::hierarchy},
                {&m_inspector, &EditorPanelVisibility::inspector},
                {&m_diagnosticsPanel, &EditorPanelVisibility::diagnostics},
                {&m_renderStatistics, &EditorPanelVisibility::renderStatistics},
                {&m_assetBrowser, &EditorPanelVisibility::assetBrowser},
                {&m_graphics, &EditorPanelVisibility::graphics},
                {&m_buildPanel, &EditorPanelVisibility::build}}}
    , m_sceneRoot(std::move(sceneRoot))
    , m_fontPath(std::move(fontPath))
    , m_initialSceneLoader(std::move(initialSceneLoader))
    , m_layoutPath((settingsDirectory / "layout.ini").string())
    , m_smokeScenePath(settingsDirectory / (std::string("streaming-smoke") + sceneExtension)) {
    for (const std::string& problem : m_keymap.defaultProblems()) {
        m_diagnostics.reportError(problem);
    }
    registerSessionOperators();
    registerObjectModeOperators();
    registerModuleOperators();
    if (const OperatorRegistrationResult result = registerObjectOperators(m_operators); !result) {
        m_diagnostics.reportError(result.error);
    }
}

SceneEditing EditorSession::editing(ApplicationContext& context) {
    return {context.scene, context.assets, m_document, m_history, m_diagnostics};
}

EditorContext EditorSession::editorContext(ApplicationContext& context) {
    return {.app = context,
            .editing = editing(context),
            .selection = m_selection,
            .diagnostics = m_diagnostics,
            .camera = m_camera,
            .mode = m_playMode.mode(),
            .actions = *this};
}

OperatorContext EditorSession::operatorContext(ApplicationContext& context, KeymapRegion region) {
    return {.editing = editing(context),
            .selection = m_selection,
            .mode = m_playMode.mode(),
            .region = region,
            .app = &context};
}

Operator EditorSession::applicationOperator(std::string id, std::string label, std::string description,
                                            std::function<bool(const OperatorContext&)> poll,
                                            std::function<void(ApplicationContext&)> run) {
    return {.id = std::move(id),
            .label = std::move(label),
            .description = std::move(description),
            .poll = [poll = std::move(poll)](
                        const OperatorContext& context) { return context.app != nullptr && (!poll || poll(context)); },
            .execute =
                [run = std::move(run)](OperatorContext& context, OperatorProperties&) {
                    run(*context.app);
                    return OperatorResult::finished();
                }};
}

void EditorSession::registerOperator(Operator op) {
    if (const OperatorRegistrationResult result = m_operators.add(std::move(op)); !result) {
        m_diagnostics.reportError(result.error);
    }
}

bool EditorSession::documentCommandsAvailable(const OperatorContext& context) const {
    return context.editMode() && !m_fileDialog.pending() && !m_activeTransform;
}

void EditorSession::registerSessionOperators() {
    const auto documentCommands = [this](const OperatorContext& context) { return documentCommandsAvailable(context); };
    registerOperator(applicationOperator("file.new", "New", "Start a new scene with the default content",
                                         documentCommands, [this](ApplicationContext& app) { requestNew(app); }));
    registerOperator(applicationOperator("file.open", "Open", "Open a scene file", documentCommands,
                                         [this](ApplicationContext& app) { requestOpen(app); }));
    registerOperator(applicationOperator("file.save", "Save", "Save the scene to its file", documentCommands,
                                         [this](ApplicationContext& app) { saveDocument(app); }));
    registerOperator(applicationOperator("file.save_as", "Save As", "Save the scene to a new file", documentCommands,
                                         [this](ApplicationContext&) { requestFileDialog(FileDialogKind::Save); }));
    registerOperator(applicationOperator(
        "file.quit", "Quit", "Close the editor, asking about unsaved changes first",
        [this](const OperatorContext&) { return !m_activeTransform; },
        [this](ApplicationContext& app) { requestClose(app); }));

    registerOperator(applicationOperator(
        "edit.undo", "Undo", "Undo the last scene change",
        [this](const OperatorContext& context) { return documentCommandsAvailable(context) && m_history.canUndo(); },
        [this](ApplicationContext& app) { undo(app); }));
    registerOperator(applicationOperator(
        "edit.redo", "Redo", "Redo the last undone scene change",
        [this](const OperatorContext& context) { return documentCommandsAvailable(context) && m_history.canRedo(); },
        [this](ApplicationContext& app) { redo(app); }));
    registerOperator(
        {.id = "edit.copy",
         .label = "Copy",
         .description = "Copy the selected objects to the clipboard",
         .poll =
             [](const OperatorContext& context) {
                 return context.app != nullptr && context.editMode() && !context.selection.empty();
             },
         .execute = [this](OperatorContext& context, OperatorProperties&) { return copySelection(*context.app); }});
    registerOperator(
        {.id = "edit.paste",
         .label = "Paste Entities",
         .description = "Paste objects from the clipboard and select them",
         .flags = OperatorFlags::Undo,
         .poll = [](const OperatorContext& context) { return context.app != nullptr && context.editMode(); },
         .execute = [this](OperatorContext& context, OperatorProperties&) { return pasteClipboard(*context.app); }});

    registerOperator(applicationOperator(
        "play.start", "Play", "Run the scene in Play mode on a copy of the document",
        [](const OperatorContext& context) { return context.mode == EditorMode::Edit; },
        [this](ApplicationContext& app) { enterPlayMode(app); }));
    registerOperator(applicationOperator(
        "play.toggle_pause", "Pause / Resume", "Pause or resume Play mode",
        [](const OperatorContext& context) { return context.mode != EditorMode::Edit; },
        [this](ApplicationContext&) {
            if (m_playMode.mode() == EditorMode::Play) {
                m_playMode.pause();
            } else {
                m_playMode.resume();
            }
        }));
    registerOperator(applicationOperator(
        "play.step", "Single Step", "Advance paused Play mode by one fixed update",
        [](const OperatorContext& context) { return context.mode == EditorMode::Paused; },
        [this](ApplicationContext&) { m_playMode.requestSingleStep(); }));
    registerOperator(applicationOperator(
        "play.stop", "Stop", "Leave Play mode and restore the authored scene",
        [](const OperatorContext& context) { return context.mode != EditorMode::Edit; },
        [this](ApplicationContext& app) { stopPlayMode(app); }));

    registerOperator(applicationOperator("wm.search_menu", "Search Operators",
                                         "Find and run any available operator by name", {},
                                         [this](ApplicationContext&) { m_operatorSearch.open(); }));
    registerOperator(applicationOperator("wm.keymap_editor", "Keymap Editor",
                                         "Change, add, or reset keyboard shortcuts", {},
                                         [this](ApplicationContext&) { m_showKeymapEditor = true; }));
    registerOperator(
        {.id = "wm.toggle_panel",
         .label = "Toggle Panel",
         .description = "Show or hide an editor panel",
         .flags = OperatorFlags::Internal,
         .properties = OperatorProperties::make(
             PanelProperties{}, std::move(Fields<PanelProperties>().add("panel", &PanelProperties::panel))),
         .poll = [](const OperatorContext& context) { return context.app != nullptr; },
         .execute =
             [this](OperatorContext&, OperatorProperties& properties) {
                 const PanelProperties* values = properties.get<PanelProperties>();
                 for (const PanelSlot& slot : m_panels) {
                     if (values != nullptr && values->panel == slot.panel->title()) {
                         bool& visible = m_settings.panels().*slot.visible;
                         visible = !visible;
                         return OperatorResult::finished();
                     }
                 }
                 return OperatorResult::failed("There is no panel named \"" + (values ? values->panel : "") + "\"");
             }});
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
    for (const ImGuiKeyChord chord : m_keymap.chords(region)) {
        // Global keys work while any panel has focus, so they route globally;
        // global routes rank below focused widgets, so text fields keep their
        // own Ctrl+Z. Region keys already require the mouse over the region.
        const bool pressed = region == KeymapRegion::Global ? ImGui::Shortcut(chord, ImGuiInputFlags_RouteGlobal)
                                                            : ImGui::IsKeyChordPressed(chord);
        if (!pressed) {
            continue;
        }
        const KeymapBinding* binding =
            m_keymap.resolve(region, chord, ImGui::GetTime(), ImGui::GetIO().MouseDoubleClickTime);
        if (binding != nullptr) {
            // Copied: an operator may change the keymap.
            const std::string operatorId = binding->operatorId;
            const std::string properties = binding->properties;
            invokeOperator(context, operatorId, properties, region);
        }
        return;
    }
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
    if (m_playMode.mode() != EditorMode::Edit || m_activeTransform) {
        return;
    }
    const glm::vec2 viewportCorner = m_viewport.position() + glm::vec2(0.0f, m_viewport.size().y);
    if (const std::optional<OperatorProperties> edited =
            m_lastOperatorPanel.draw(m_operatorRunner.lastOperator(), viewportCorner)) {
        OperatorContext operatorContext = this->operatorContext(context, KeymapRegion::Global);
        m_operatorRunner.adjustLast(operatorContext, *edited);
    }
}

void EditorSession::saveKeymap() {
    if (const KeymapResult result = m_keymap.save(m_keymapPath); !result) {
        m_diagnostics.reportError(result.error);
    }
}

SceneResult EditorSession::load(ApplicationContext& context) {
    if (const EditorSettingsResult result = m_settings.load(); !result) {
        m_diagnostics.reportError(result.error);
    }
    m_graphics.loadSettings(context.rendering, m_diagnostics);
    const KeymapResult keymap = m_keymap.load(m_keymapPath);
    if (!keymap) {
        m_diagnostics.reportError(keymap.error);
    }
    for (const std::string& warning : keymap.warnings) {
        m_diagnostics.add(EditorDiagnosticSeverity::Warning, "Keymap", warning);
    }
    m_assetBrowser.refresh(m_diagnostics);
    m_openRecoveryDialog = m_recovery.available();
    m_smokeScenario = SDL_getenv("ZENITH_EDITOR_SMOKE") != nullptr;

    ImGuiIO& io = ImGui::GetIO();
    // Size the UI for the display's scale factor so text stays crisp on
    // high-DPI monitors instead of being stretched.
    float contentScale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (contentScale <= 0.0f) {
        contentScale = 1.0f;
    }
    applyEditorTheme(ImGui::GetStyle());
    ImGui::GetStyle().ScaleAllSizes(contentScale);
    if (const EditorFontResult result = loadEditorFont(io, m_fontPath, editorFontPixelSize * contentScale); !result) {
        m_diagnostics.reportError(result.error);
    }
    io.IniFilename = m_layoutPath.c_str();
    ImGui::LoadIniSettingsFromDisk(io.IniFilename);
    m_initializeLayout = m_settings.layoutVersion() < currentLayoutVersion;
    // Asset paths in render settings (the colour-grading LUT) resolve against
    // the edited project's assets.
    context.rendering.setAssetRoot(m_document.options().assetRoot);
    // Before any scene loads, so documents read the module's component types.
    loadGameModule();
    if (m_initialSceneLoader) {
        if (SceneResult result = m_initialSceneLoader(context, m_document.options()); !result) {
            return result;
        }
    } else if (const SceneResult result =
                   populateDefaultScene(context.scene, context.assets, m_document.options().assetRoot);
               !result) {
        m_diagnostics.reportError(result.error);
    }
    if (m_smokeScenario) {
        return prepareSmokeScenario(context);
    }
    return {.success = true};
}

SceneResult EditorSession::prepareSmokeScenario(ApplicationContext& context) {
    Scene streamingFixture;
    for (uint32_t index = 0; index < 5'000; ++index) {
        streamingFixture.createEntity("Streamed entity " + std::to_string(index));
    }
    const auto models = context.scene.registry().view<const ModelComponent>();
    if (!models.empty()) {
        const ModelHandle model = models.get<const ModelComponent>(*models.begin()).model;
        const Entity modelRoot = streamingFixture.createEntity("Streamed model");
        if (SceneResult result = streamingFixture.instantiateModel(modelRoot, model, context.assets); !result) {
            return {.error = "Could not prepare streaming smoke model: " + result.error};
        }
    }
    if (SceneDocumentResult result =
            SceneDocument::save(m_smokeScenePath, streamingFixture, context.assets, m_document.options());
        !result) {
        return {.error = "Could not prepare streaming smoke scene: " + result.error};
    }
    m_smokeJobs = std::make_unique<JobSystem>(JobSystemConfig{.workerCount = 1, .queueCapacity = 4});
    m_smokeLayers = std::make_unique<AdditiveSceneManager>(*m_smokeJobs, context.assets, m_document.options());
    return {.success = true};
}

void EditorSession::update(ApplicationContext& context, const InputState& input, float deltaSeconds) {
    if (m_smokeScenario) {
        updateSmokeScenario(context);
    }
    updateAutosave(context, deltaSeconds);
    EditorContext editor = editorContext(context);
    for (const PanelSlot& slot : m_panels) {
        slot.panel->update(editor, input, deltaSeconds);
    }
    updateGameModule(context, input, deltaSeconds);
}

void EditorSession::updateSmokeScenario(ApplicationContext& context) {
    if (m_smokeLayers) {
        m_smokeLayers->update();
    }
    ++m_smokeFrame;
    if (m_smokeFrame == 1) {
        RenderSettings settings = context.rendering.settings();
        settings.bloom = BloomQuality::Downsampled;
        settings.shadows = ShadowQuality::CascadedHigh;
        settings.visualizeShadowCascades = true;
        context.rendering.setSettings(settings);
        context.rendering.requestViewportSize(640, 360);
    } else if (m_smokeFrame == 2) {
        enterPlayMode(context);
    } else if (m_smokeFrame == 3) {
        m_playMode.pause();
        m_playMode.requestSingleStep();
    } else if (m_smokeFrame == 4) {
        stopPlayMode(context);
    } else if (m_smokeFrame == 5) {
        context.rendering.requestViewportSize(800, 450);
        m_smokeStreamInstance = m_smokeLayers->requestLoad(m_smokeScenePath, 100);
    }
    if (m_smokeStreamInstance && !m_smokeStreamCompleted) {
        const auto state = m_smokeLayers->state(*m_smokeStreamInstance);
        if (state && state->status == AdditiveSceneStatus::Committed) {
            m_smokeLayers->unload(*m_smokeStreamInstance);
            context.assets.collectUnused(true);
            m_smokeStreamCompleted = true;
            // The layers share the document's component registry; release
            // them so the module reload below can unload the old module.
            releaseSmokeLayers();
        } else if (state &&
                   (state->status == AdditiveSceneStatus::Failed || state->status == AdditiveSceneStatus::Cancelled)) {
            m_diagnostics.reportError("Streaming smoke failed: " + state->error);
            context.control.requestSceneChange({std::numeric_limits<uint32_t>::max()});
        }
    }
    if (m_smokeFrame == 500 && !m_smokeStreamCompleted) {
        m_diagnostics.reportError("Streaming smoke did not commit while frames were being presented");
        context.control.requestSceneChange({std::numeric_limits<uint32_t>::max()});
    } else if (m_smokeFrame >= 8 && m_smokeStreamCompleted) {
        updateSmokeModuleReload(context);
    }
}

void EditorSession::updateSmokeModuleReload(ApplicationContext& context) {
    if (m_moduleHost.sourcePath().empty()) {
        context.control.requestQuit();
        return;
    }
    if (!m_smokeModuleReloadRequested) {
        // A project with a game module also hot reloads it once.
        m_smokeModuleReloadRequested = true;
        m_moduleReloadPending = true;
        return;
    }
    if (m_moduleReloadPending) {
        return;
    }
    if (m_moduleHost.status() == GameModuleStatus::Loaded && m_moduleHost.loadCount() >= 2) {
        context.control.requestQuit();
        return;
    }
    const std::string error = "Game module smoke reload failed: " + m_moduleHost.error();
    std::cerr << error << '\n';
    m_diagnostics.reportError(error);
    context.control.requestSceneChange({std::numeric_limits<uint32_t>::max()});
}

void EditorSession::releaseSmokeLayers() {
    if (m_smokeLayers) {
        m_smokeLayers->clear();
        m_smokeLayers.reset();
    }
    if (m_smokeJobs) {
        m_smokeJobs->shutdown(true);
        m_smokeJobs.reset();
    }
}

void EditorSession::updateAutosave(ApplicationContext& context, float deltaSeconds) {
    if (!m_document.dirty() || m_playMode.mode() != EditorMode::Edit) {
        m_autosaveElapsed = 0.0f;
        return;
    }
    m_autosaveElapsed += deltaSeconds;
    if (m_autosaveElapsed >= autosaveIntervalSeconds) {
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
    if (m_playMode.mode() != EditorMode::Edit) {
        m_moduleHost.runSystems(SystemPhase::FixedUpdate, context.scene, &context, SimulationMode::Play, deltaSeconds,
                                &input);
    }
}

bool EditorSession::fixedUpdatesEnabled() const {
    return m_playMode.fixedUpdatesEnabled();
}

bool EditorSession::beginFixedUpdate() {
    return m_playMode.beginFixedUpdate();
}

void EditorSession::requestQuit(ApplicationContext& context) {
    requestClose(context);
}

void EditorSession::drawUi(ApplicationContext& context) {
    ImGuizmo::BeginFrame();
    const ImGuiID dockspaceId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
    if (m_initializeLayout) {
        initializeDefaultLayout(dockspaceId);
    }
    drawMainMenu(context);
    pollFileDialog(context);
    drawPathDialog(context);
    drawUnsavedChangesDialog(context);
    drawRecoveryDialog(context);
    drawPanels(context);
    drawModulePanels(context);
    drawObjectPopups(context);
    drawOperatorWindows(context);
    drawStatusBar(context.rendering.stats());
}

void EditorSession::initializeDefaultLayout(uint32_t dockspaceId) {
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

    ImGuiID center = dockspaceId;
    const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20f, nullptr, &center);
    const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.25f, nullptr, &center);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.25f, nullptr, &center);
    ImGuiID inspector = right;
    const ImGuiID graphics = ImGui::DockBuilderSplitNode(inspector, ImGuiDir_Down, 0.70f, nullptr, &inspector);

    ImGui::DockBuilderDockWindow(m_viewport.title(), center);
    ImGui::DockBuilderDockWindow(m_hierarchy.title(), left);
    ImGui::DockBuilderDockWindow(m_inspector.title(), inspector);
    ImGui::DockBuilderDockWindow(m_graphics.title(), graphics);
    ImGui::DockBuilderDockWindow(m_diagnosticsPanel.title(), bottom);
    ImGui::DockBuilderDockWindow(m_buildPanel.title(), bottom);
    ImGui::DockBuilderDockWindow(m_renderStatistics.title(), bottom);
    ImGui::DockBuilderFinish(dockspaceId);

    m_settings.panels() = {};
    m_settings.setLayoutVersion(currentLayoutVersion);
    m_initializeLayout = false;
}

void EditorSession::unload(ApplicationContext& context) {
    releaseSmokeLayers();
    if (m_smokeScenario) {
        std::error_code ignored;
        std::filesystem::remove(m_smokeScenePath, ignored);
    }
    stopPlayMode(context);
    // The scene and document release module code before the module unloads.
    context.audio.stopSceneVoices();
    context.physics.clear();
    m_moduleHost.unload(context.scene, m_document);
    context.rendering.setGridOverlay(std::nullopt);
    context.rendering.clearOutlinedObjects();
    context.rendering.clearViewOverride();
    ImGui::SaveIniSettingsToDisk(m_layoutPath.c_str());
    if (const EditorSettingsResult result = m_settings.save(); !result) {
        m_diagnostics.reportError(result.error);
    }
    ImGui::GetIO().IniFilename = nullptr;
}

void EditorSession::handleGlobalShortcuts(ApplicationContext& context) {
    // Operator polls decide which shortcuts apply in the current mode.
    if (ImGui::GetIO().WantTextInput || m_keymapEditor.capturing()) {
        return;
    }
    dispatchKeymap(context, KeymapRegion::Global);
}

void EditorSession::drawMainMenu(ApplicationContext& context) {
    handleGlobalShortcuts(context);
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    if (ImGui::BeginMenu("File")) {
        operatorMenuItem(context, "file.new", "New");
        operatorMenuItem(context, "file.open", "Open...");
        operatorMenuItem(context, "file.save", "Save");
        operatorMenuItem(context, "file.save_as", "Save As...");
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
            operatorMenuItem(context, "object.add_cube", "Cube");
            operatorMenuItem(context, "object.add_sun", "Sun");
            operatorMenuItem(context, "object.add_environment", "Environment");
            operatorMenuItem(context, "object.add_empty", "Empty");
            ImGui::EndMenu();
        }
        ImGui::Separator();
        operatorMenuItem(context, "wm.search_menu", "Search Operators...");
        operatorMenuItem(context, "wm.keymap_editor", "Keymap...");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        const EditorPanelVisibility& visibility = m_settings.panels();
        for (const PanelSlot& slot : m_panels) {
            operatorMenuItem(context, "wm.toggle_panel", slot.panel->title(),
                             nlohmann::json{{"panel", slot.panel->title()}}.dump(), visibility.*slot.visible);
        }
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
        if (m_playMode.mode() == EditorMode::Edit) {
            operatorMenuItem(context, "play.start", "Play");
        } else {
            operatorMenuItem(context, "play.toggle_pause", m_playMode.mode() == EditorMode::Play ? "Pause" : "Resume");
            operatorMenuItem(context, "play.step", "Single Step");
            operatorMenuItem(context, "play.stop", "Stop");
        }
        ImGui::EndMenu();
    }
    drawModuleMenu(context);
    ImGui::EndMainMenuBar();
}

void EditorSession::drawPanels(ApplicationContext& context) {
    EditorContext editor = editorContext(context);
    EditorPanelVisibility& visibility = m_settings.panels();
    for (const PanelSlot& slot : m_panels) {
        bool& visible = visibility.*slot.visible;
        if (!visible) {
            slot.panel->hidden(editor);
            continue;
        }
        if (ImGui::Begin(slot.panel->title(), &visible, slot.panel->windowFlags())) {
            const bool readOnly = slot.panel->editModeOnly() && !editor.editMode();
            ImGui::BeginDisabled(readOnly);
            slot.panel->draw(editor);
            ImGui::EndDisabled();
        }
        ImGui::End();
    }
}

void EditorSession::drawStatusBar(const RenderStats& stats) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float height = ImGui::GetFrameHeight();
    ImGui::SetNextWindowPos({viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - height});
    ImGui::SetNextWindowSize({viewport->WorkSize.x, height});
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("##EditorStatus", nullptr, flags)) {
        constexpr const char* modeNames[] = {"Select", "Move", "Rotate", "Scale"};
        constexpr const char* spaceNames[] = {"Global", "Local"};
        constexpr const char* playModeNames[] = {"Edit", "Play", "Paused"};
        const TransformToolSettings& tools = m_viewport.tools();
        ImGui::Text("%s | %s", m_document.displayName().c_str(),
                    m_diagnostics.hasUnacknowledgedError() ? "Diagnostics available" : "Ready");
        ImGui::SameLine();
        ImGui::Text("| Mode: %s | Space: %s | Snap: %s", modeNames[static_cast<int>(tools.gizmo)],
                    spaceNames[static_cast<int>(tools.space)], tools.snapping ? "On" : "Off");
        ImGui::SameLine();
        ImGui::Text("| Selection: %zu | Draws %u | Visible %u | GPU %.2f ms", m_selection.size(), stats.drawCalls,
                    stats.visibleCount, stats.gpuFrameMilliseconds);
        ImGui::SameLine();
        ImGui::Text("| %s", playModeNames[static_cast<int>(m_playMode.mode())]);
        if (m_moduleHost.status() != GameModuleStatus::None) {
            ImGui::SameLine();
            ImGui::Text("| %s", gameModuleStatusText().c_str());
        }
    }
    ImGui::End();
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
    if (ImGui::Button(m_pathDialogSaves ? "Save" : "Open")) {
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
    if (ImGui::Button("Cancel")) {
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
    if (ImGui::Button("Save")) {
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
    if (ImGui::Button("Discard")) {
        std::string ignored;
        m_recovery.discard(ignored);
        ImGui::CloseCurrentPopup();
        finishPendingAction(context);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
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
    if (ImGui::Button("Recover")) {
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
    if (ImGui::Button("Discard Recovery")) {
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

void EditorSession::requestClose(ApplicationContext& context) {
    m_pendingOpenPath.reset();
    if (m_playMode.mode() != EditorMode::Edit) {
        stopPlayMode(context);
    }
    m_pendingAction = PendingAction::CloseEditor;
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

void EditorSession::enterPlayMode(ApplicationContext& context) {
    context.physics.clear();
    context.audio.stopSceneVoices();
    if (const SceneDocumentResult result =
            m_playMode.enter(context.scene, context.assets, m_document.options(), m_selection.ids());
        !result) {
        m_diagnostics.reportError("Could not enter Play mode: " + result.error);
        return;
    }
    m_history.crossBoundary(CommandHistoryBoundary::EnterPlayMode);
    m_diagnostics.acknowledgeErrors();
}

void EditorSession::stopPlayMode(ApplicationContext& context) {
    if (m_playMode.mode() == EditorMode::Edit) {
        return;
    }
    context.audio.stopSceneVoices();
    context.physics.clear();
    std::vector<SceneEntityId> restoredSelection;
    m_playMode.stop(context.scene, restoredSelection);
    m_selection.assign(std::move(restoredSelection));
    m_history.crossBoundary(CommandHistoryBoundary::LeavePlayMode);
    m_document.setDirty(m_history.dirty());
}
