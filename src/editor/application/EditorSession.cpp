#include "EditorSession.hpp"

#include "editor/application/EditorTheme.hpp"
#include "editor/application/EntityCommands.hpp"
#include "editor/document/DefaultScene.hpp"
#include "editor/document/SceneFragment.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <ImGuizmo.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace {
// Layouts older than version 3 lack the Graphics panel below the Inspector.
constexpr uint32_t currentLayoutVersion = 3;
constexpr const char* sceneExtension = ".zenith-scene";
constexpr float editorFontPixelSize = 15.0f;
constexpr float autosaveIntervalSeconds = 30.0f;

// Editor commands are registered outside any panel, so they must route
// globally; focus routing would drop them whenever a panel has focus. Global
// routes rank below focused widgets, so text fields keep their own Ctrl+Z.
bool editorShortcut(ImGuiKeyChord chord) {
    return ImGui::Shortcut(chord, ImGuiInputFlags_RouteGlobal);
}

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
                             EditorRenderSettings renderSettings,
                             std::function<SceneResult(ApplicationContext&)> initialSceneLoader)
    : m_settings(settingsDirectory / "settings.ini")
    , m_document(assetRoot)
    , m_recovery(settingsDirectory / (std::string("recovery") + sceneExtension))
    , m_fileDialog("Zenith scene", sceneExtension)
    , m_assetBrowser(std::move(assetRoot), sceneRoot)
    , m_graphics(std::move(renderSettings))
    , m_panels{{{&m_viewport, &EditorPanelVisibility::viewport},
                {&m_hierarchy, &EditorPanelVisibility::hierarchy},
                {&m_inspector, &EditorPanelVisibility::inspector},
                {&m_diagnosticsPanel, &EditorPanelVisibility::diagnostics},
                {&m_renderStatistics, &EditorPanelVisibility::renderStatistics},
                {&m_assetBrowser, &EditorPanelVisibility::assetBrowser},
                {&m_graphics, &EditorPanelVisibility::graphics}}}
    , m_sceneRoot(std::move(sceneRoot))
    , m_fontPath(std::move(fontPath))
    , m_initialSceneLoader(std::move(initialSceneLoader))
    , m_layoutPath((settingsDirectory / "layout.ini").string())
    , m_smokeScenePath(settingsDirectory / (std::string("streaming-smoke") + sceneExtension)) {
    const std::pair<const char*, const char*> bindings[]{
        {"New", "Ctrl+N"},  {"Open", "Ctrl+O"},       {"Save", "Ctrl+S"},  {"Save As", "Ctrl+Shift+S"},
        {"Undo", "Ctrl+Z"}, {"Redo", "Ctrl+Shift+Z"}, {"Quit", "Ctrl+Q"},  {"Play", "F5"},
        {"Pause", "F6"},    {"Step", "F7"},           {"Stop", "Shift+F5"}};
    for (const auto& [action, chord] : bindings) {
        if (const EditorShortcutResult result = m_shortcuts.bind(action, chord); !result) {
            m_diagnostics.reportError(result.error);
        }
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

SceneResult EditorSession::load(ApplicationContext& context) {
    if (const EditorSettingsResult result = m_settings.load(); !result) {
        m_diagnostics.reportError(result.error);
    }
    m_graphics.loadSettings(context.rendering, m_diagnostics);
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
    if (m_initialSceneLoader) {
        if (SceneResult result = m_initialSceneLoader(context); !result) {
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
        context.control.requestQuit();
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

void EditorSession::fixedUpdate(ApplicationContext&, const InputState&, float) {}

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
    drawObjectPopups(context);
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
    ImGui::DockBuilderDockWindow(m_renderStatistics.title(), bottom);
    ImGui::DockBuilderFinish(dockspaceId);

    m_settings.panels() = {};
    m_settings.setLayoutVersion(currentLayoutVersion);
    m_initializeLayout = false;
}

void EditorSession::unload(ApplicationContext& context) {
    if (m_smokeLayers) {
        m_smokeLayers->clear();
        m_smokeLayers.reset();
    }
    if (m_smokeJobs) {
        m_smokeJobs->shutdown(true);
        m_smokeJobs.reset();
    }
    if (m_smokeScenario) {
        std::error_code ignored;
        std::filesystem::remove(m_smokeScenePath, ignored);
    }
    stopPlayMode(context);
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
    if (ImGui::GetIO().WantTextInput) {
        return;
    }
    if (m_playMode.mode() == EditorMode::Edit && !m_fileDialog.pending() && !m_activeTransform) {
        if (editorShortcut(ImGuiMod_Ctrl | ImGuiKey_N)) {
            requestNew(context);
        }
        if (editorShortcut(ImGuiMod_Ctrl | ImGuiKey_O)) {
            requestOpen(context);
        }
        if (editorShortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S)) {
            requestFileDialog(FileDialogKind::Save);
        } else if (editorShortcut(ImGuiMod_Ctrl | ImGuiKey_S)) {
            saveDocument(context);
        }
        if (editorShortcut(ImGuiMod_Ctrl | ImGuiKey_Z)) {
            undo(context);
        }
        // Blender redoes with Ctrl Shift Z; Ctrl Y is kept as the Windows alias.
        if (editorShortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z) || editorShortcut(ImGuiMod_Ctrl | ImGuiKey_Y)) {
            redo(context);
        }
        if (editorShortcut(ImGuiMod_Ctrl | ImGuiKey_Q)) {
            requestClose(context);
        }
    }
    if (m_playMode.mode() == EditorMode::Edit && editorShortcut(ImGuiKey_F5)) {
        enterPlayMode(context);
    } else if (m_playMode.mode() != EditorMode::Edit && editorShortcut(ImGuiMod_Shift | ImGuiKey_F5)) {
        stopPlayMode(context);
    }
    if (m_playMode.mode() == EditorMode::Play && editorShortcut(ImGuiKey_F6)) {
        m_playMode.pause();
    } else if (m_playMode.mode() == EditorMode::Paused && editorShortcut(ImGuiKey_F6)) {
        m_playMode.resume();
    }
    if (m_playMode.mode() == EditorMode::Paused && editorShortcut(ImGuiKey_F7)) {
        m_playMode.requestSingleStep();
    }
}

void EditorSession::drawMainMenu(ApplicationContext& context) {
    handleGlobalShortcuts(context);
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    if (ImGui::BeginMenu("File")) {
        ImGui::BeginDisabled(m_playMode.mode() != EditorMode::Edit || m_fileDialog.pending());
        if (ImGui::MenuItem("New", m_shortcuts.chord("New").c_str())) {
            requestNew(context);
        }
        if (ImGui::MenuItem("Open...", m_shortcuts.chord("Open").c_str())) {
            requestOpen(context);
        }
        if (ImGui::MenuItem("Save", m_shortcuts.chord("Save").c_str())) {
            saveDocument(context);
        }
        if (ImGui::MenuItem("Save As...", m_shortcuts.chord("Save As").c_str())) {
            requestFileDialog(FileDialogKind::Save);
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", m_shortcuts.chord("Quit").c_str())) {
            requestClose(context);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        const bool editing = m_playMode.mode() == EditorMode::Edit;
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, editing && m_history.canUndo())) {
            undo(context);
        }
        if (ImGui::MenuItem("Redo", m_shortcuts.chord("Redo").c_str(), false, editing && m_history.canRedo())) {
            redo(context);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Copy", objectCommandLabel(ObjectCommand::Copy), false, editing && !m_selection.empty())) {
            copySelection(context);
        }
        if (ImGui::MenuItem("Paste", objectCommandLabel(ObjectCommand::Paste), false,
                            editing && !m_clipboardFragment.empty())) {
            pasteClipboard(context);
        }
        if (ImGui::MenuItem("Duplicate", objectCommandLabel(ObjectCommand::Duplicate), false,
                            editing && !m_selection.empty())) {
            EntityCommands::duplicateSelection(this->editing(context), m_selection);
        }
        if (ImGui::MenuItem("Delete", objectCommandLabel(ObjectCommand::Delete), false,
                            editing && !m_selection.empty())) {
            EntityCommands::deleteSelection(this->editing(context), m_selection);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Select All", objectCommandLabel(ObjectCommand::SelectAll), false, editing)) {
            m_selection.selectAll(context.scene);
        }
        if (ImGui::MenuItem("Deselect All", objectCommandLabel(ObjectCommand::DeselectAll), false, editing)) {
            m_selection.clear();
        }
        if (ImGui::MenuItem("Invert Selection", objectCommandLabel(ObjectCommand::InvertSelection), false, editing)) {
            m_selection.invert(context.scene);
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("Clear", editing && !m_selection.empty())) {
            if (ImGui::MenuItem("Location", objectCommandLabel(ObjectCommand::ClearLocation))) {
                clearSelectedTransforms(context, TransformPart::Location);
            }
            if (ImGui::MenuItem("Rotation", objectCommandLabel(ObjectCommand::ClearRotation))) {
                clearSelectedTransforms(context, TransformPart::Rotation);
            }
            if (ImGui::MenuItem("Scale", objectCommandLabel(ObjectCommand::ClearScale))) {
                clearSelectedTransforms(context, TransformPart::Scale);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Add", editing)) {
            if (ImGui::MenuItem("Cube")) {
                addObject(context, AddableObject::Cube);
            }
            if (ImGui::MenuItem("Sun")) {
                addObject(context, AddableObject::Sun);
            }
            if (ImGui::MenuItem("Environment")) {
                addObject(context, AddableObject::Environment);
            }
            if (ImGui::MenuItem("Empty")) {
                addObject(context, AddableObject::Empty);
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        EditorPanelVisibility& visibility = m_settings.panels();
        for (const PanelSlot& slot : m_panels) {
            ImGui::MenuItem(slot.panel->title(), nullptr, &(visibility.*slot.visible));
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Frame Selected", objectCommandLabel(ObjectCommand::FrameSelected), false,
                            !m_selection.empty())) {
            frameEntities(context, m_selection.ids());
        }
        if (ImGui::MenuItem("Frame All", objectCommandLabel(ObjectCommand::FrameAll))) {
            frameAll(context);
        }
        if (ImGui::BeginMenu("Viewpoint")) {
            constexpr std::pair<const char*, ObjectCommand> viewpoints[]{
                {"Front", ObjectCommand::ViewFront}, {"Back", ObjectCommand::ViewBack},
                {"Right", ObjectCommand::ViewRight}, {"Left", ObjectCommand::ViewLeft},
                {"Top", ObjectCommand::ViewTop},     {"Bottom", ObjectCommand::ViewBottom}};
            for (const auto& [name, command] : viewpoints) {
                if (ImGui::MenuItem(name, objectCommandLabel(command))) {
                    runObjectCommand(context, command, KeymapRegion::Viewport);
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Play")) {
        if (m_playMode.mode() == EditorMode::Edit) {
            if (ImGui::MenuItem("Play", "F5")) {
                enterPlayMode(context);
            }
        } else {
            if (m_playMode.mode() == EditorMode::Play && ImGui::MenuItem("Pause", "F6")) {
                m_playMode.pause();
            } else if (m_playMode.mode() == EditorMode::Paused && ImGui::MenuItem("Resume", "F6")) {
                m_playMode.resume();
            }
            if (ImGui::MenuItem("Single Step", "F7", false, m_playMode.mode() == EditorMode::Paused)) {
                m_playMode.requestSingleStep();
            }
            if (ImGui::MenuItem("Stop", "Shift+F5")) {
                stopPlayMode(context);
            }
        }
        ImGui::EndMenu();
    }
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

void EditorSession::copySelection(ApplicationContext& context) {
    if (m_selection.empty()) {
        return;
    }
    if (const SceneDocumentResult result = SceneFragment::copy(context.scene, context.assets, m_document.options(),
                                                               m_selection.ids(), m_clipboardFragment);
        !result) {
        m_diagnostics.reportError("Copy failed: " + result.error);
        return;
    }
    ImGui::SetClipboardText(m_clipboardFragment.c_str());
}

void EditorSession::pasteClipboard(ApplicationContext& context) {
    const char* systemClipboard = ImGui::GetClipboardText();
    const std::string_view fragment =
        systemClipboard != nullptr && *systemClipboard != '\0' ? systemClipboard : m_clipboardFragment;
    if (fragment.empty()) {
        return;
    }
    const SceneEditing sceneEditing = editing(context);
    std::string before;
    if (!sceneEditing.capture(before)) {
        return;
    }
    std::vector<SceneEntityId> pasted;
    if (const SceneDocumentResult result =
            SceneFragment::paste(fragment, context.scene, context.assets, m_document.options(), entt::null, pasted);
        !result) {
        m_diagnostics.reportError("Paste failed: " + result.error);
        return;
    }
    sceneEditing.commit("Paste Entities", {}, std::move(before));
    m_selection.assign(std::move(pasted));
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
