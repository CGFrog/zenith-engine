#include "EditorSession.hpp"

#include "editor/application/EntityCommands.hpp"
#include "editor/areas/AreaWindows.hpp"
#include "editor/areas/DefaultWorkspaces.hpp"
#include "editor/document/DefaultScene.hpp"
#include "editor/document/SceneFragment.hpp"
#include "editor/menus/DefaultMenus.hpp"
#include "editor/operators/ComponentOperators.hpp"
#include "editor/operators/DefaultKeymap.hpp"
#include "editor/operators/EyedropperOperators.hpp"
#include "editor/operators/ObjectOperators.hpp"
#include "editor/operators/PrefabOperators.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/EditorWidgets.hpp"
#include "engine/scene/ComponentTypes.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <ImGuizmo.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_video.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
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
    , m_projectAssets(std::move(assetRoot), sceneRoot, settingsDirectory / "asset-browser.json")
    , m_renderSettings(std::move(renderSettings))
    , m_componentLayoutPath(settingsDirectory / "component-inspector.json")
    , m_workspacesPath(settingsDirectory / "workspaces.json")
    , m_legacyLayoutPath(settingsDirectory / "layout.ini")
    , m_sceneRoot(std::move(sceneRoot))
    , m_fontPath(std::move(fontPath))
    , m_initialSceneLoader(std::move(initialSceneLoader))
    , m_smokeScenePath(settingsDirectory / (std::string("streaming-smoke") + sceneExtension)) {
    for (const std::string& problem : m_keymap.defaultProblems()) {
        m_diagnostics.reportError(problem);
    }
    registerSessionOperators();
    registerObjectModeOperators();
    registerViewportOperators();
    registerModuleOperators();
    registerAreaOperators();
    registerPreferenceOperators();
    registerAreaTypes();
    if (const OperatorRegistrationResult result = registerObjectOperators(m_operators); !result) {
        m_diagnostics.reportError(result.error);
    }
    if (const OperatorRegistrationResult result = registerEyedropperOperators(m_operators, m_eyedropper); !result) {
        m_diagnostics.reportError(result.error);
    }
    if (const OperatorRegistrationResult result =
            registerComponentOperators(m_operators, {.clipboard = m_componentClipboard, .layout = m_componentLayout});
        !result) {
        m_diagnostics.reportError(result.error);
    }
    if (const OperatorRegistrationResult result = registerMenuOperators(m_operators, m_menus); !result) {
        m_diagnostics.reportError(result.error);
    }
    if (const MenuResult result = registerDefaultMenus(m_menus.registry()); !result) {
        m_diagnostics.reportError(result.error);
    }
    if (const OperatorRegistrationResult result = registerPrefabOperators(m_operators); !result) {
        m_diagnostics.reportError(result.error);
    }
    // Scene references resolve through the project's asset database by GUID.
    m_document.setAssetDatabase(m_projectAssets.assetDatabase());
}

SceneEditing EditorSession::editing(ApplicationContext& context) {
    return {context.scene, context.assets, m_document, m_history, m_diagnostics};
}

EditorContext EditorSession::editorContext(ApplicationContext& context) {
    return {.app = context,
            .editing = editing(context),
            .selection = m_selection,
            .diagnostics = m_diagnostics,
            .mode = m_playMode.mode(),
            .actions = *this,
            .eyedropper = &m_eyedropper,
            .menus = m_menus};
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
    m_projectAssets.refresh(m_diagnostics);
    m_openRecoveryDialog = m_recovery.available();
    m_smokeScenario = SDL_getenv("ZENITH_EDITOR_SMOKE") != nullptr;

    ImGuiIO& io = ImGui::GetIO();
    // Size the UI for the display's scale factor so text stays crisp on
    // high-DPI monitors instead of being stretched, unless the preferences
    // override it. The pixel theme rounds it to a whole number of screen
    // pixels per art pixel.
    m_displayContentScale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (m_displayContentScale <= 0.0f) {
        m_displayContentScale = 1.0f;
    }
    applyAppearance();
    ThemeChrome::installPopupBevels(*ImGui::GetCurrentContext());
    // Each workspace keeps its own ImGui layout in workspaces.json.
    io.IniFilename = nullptr;
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
    // Between frames, where a new UI scale may add fonts to the atlas.
    applyAppearance();
    // Before the smoke scenario, which checks the switch it requested.
    updateWorkspaces();
    beginViewportFrame(context);
    if (m_smokeScenario) {
        updateSmokeScenario(context);
    }
    updateAutosave(context, deltaSeconds);
    updatePackaging();
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
    } else if (m_smokeFrame == 6) {
        // The viewport chrome: open the sidebar, hide and restore the tool
        // shelf, and render through a narrower lens with tighter clipping.
        ViewportPanel* viewport = m_viewports.active();
        for (const char* operatorId : {"view3d.toggle_sidebar", "view3d.toggle_toolbar", "view3d.toggle_toolbar"}) {
            invokeOperator(context, operatorId, {}, KeymapRegion::Viewport);
        }
        if (viewport == nullptr) {
            m_diagnostics.reportError("Viewport chrome smoke: no viewport is open");
            context.control.requestSceneChange({std::numeric_limits<uint32_t>::max()});
        } else {
            viewport->camera().setVerticalFovRadians(glm::radians(45.0f));
            viewport->camera().setClipPlanes(0.05f, 500.0f);
            if (!viewport->chrome().sidebarVisible || !viewport->chrome().toolbarVisible) {
                m_diagnostics.reportError("Viewport chrome smoke: the sidebar and toolbar should be visible");
                context.control.requestSceneChange({std::numeric_limits<uint32_t>::max()});
            }
        }
        // The renderer logs at least its validation-layer state at startup.
        if (m_infoPanel.log().count(InfoSource::Engine) == 0) {
            m_diagnostics.reportError("The Info panel received no engine log entries");
            context.control.requestSceneChange({std::numeric_limits<uint32_t>::max()});
        }
    } else if (m_smokeFrame == 7 || m_smokeFrame == 8) {
        updateSmokeHide(context);
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
    const bool areasDone = updateSmokeAreas(context);
    const bool viewportsDone = updateSmokeViewports(context);
    const bool componentsDone = updateSmokeComponents(context);
    const bool menusDone = updateSmokeMenus(context);
    if (m_smokeFrame == 500 && !m_smokeStreamCompleted) {
        m_diagnostics.reportError("Streaming smoke did not commit while frames were being presented");
        context.control.requestSceneChange({std::numeric_limits<uint32_t>::max()});
    } else if (areasDone && viewportsDone && componentsDone && menusDone && m_smokeStreamCompleted) {
        updateSmokeModuleReload(context);
    }
}

bool EditorSession::updateSmokeComponents(ApplicationContext& context) {
    // Frames 20-21: add two empties, select both, give the active one a
    // point light, copy it to the other and reset both through the component
    // operators (the Inspector draws the shared component in between), then
    // undo every step.
    constexpr uint32_t firstFrame = 20;
    const auto fail = [&](const std::string& message) {
        m_diagnostics.reportError("Component smoke failed: " + message);
        context.control.requestSceneChange({std::numeric_limits<uint32_t>::max()});
    };
    const auto pointLights = [&] {
        return static_cast<size_t>(std::ranges::count_if(m_smokeComponentEntities, [&](SceneEntityId id) {
            return context.scene.registry().all_of<PointLightComponent>(context.scene.findEntity(id));
        }));
    };
    if (m_smokeFrame == firstFrame) {
        m_smokeComponentUndoSteps = m_history.size();
        m_smokeComponentEntities.clear();
        for (int index = 0; index < 2; ++index) {
            invokeOperator(context, "object.add_empty", {}, KeymapRegion::Global);
            if (const std::optional<SceneEntityId> added = m_selection.active()) {
                m_smokeComponentEntities.push_back(*added);
            }
        }
        m_selection.assign(
            std::vector<SceneEntityId>(m_smokeComponentEntities.rbegin(), m_smokeComponentEntities.rend()));
        constexpr std::string_view light = R"({"component":"point_light"})";
        for (const char* operatorId :
             {"object.component_add", "object.component_copy_to_selected", "object.component_reset"}) {
            invokeOperator(context, operatorId, light, KeymapRegion::Global);
        }
        m_smokeComponentUndoSteps = m_history.size() - m_smokeComponentUndoSteps;
        if (m_smokeComponentEntities.size() != 2 || pointLights() != 2) {
            fail("the point light was not copied to both selected entities");
        }
    } else if (m_smokeFrame == firstFrame + 1) {
        for (size_t step = 0; step < m_smokeComponentUndoSteps && m_history.canUndo(); ++step) {
            undo(context);
        }
        if (std::ranges::any_of(m_smokeComponentEntities,
                                [&](SceneEntityId id) { return context.scene.findEntity(id) != entt::null; })) {
            fail("undo did not remove the smoke entities");
        }
        m_selection.clear();
    }
    return m_smokeFrame > firstFrame + 1;
}

// H hides everything in the editor views (the frame rendered in between draws
// fewer items), then Alt+H reveals it again.
void EditorSession::updateSmokeHide(ApplicationContext& context) {
    const auto fail = [&](const std::string& error) {
        context.logger.log(LogLevel::Error, "editor", "Hide smoke: " + error);
        m_diagnostics.reportError("Hide smoke: " + error);
        context.control.requestSceneChange({std::numeric_limits<uint32_t>::max()});
    };
    if (m_smokeFrame == 7) {
        m_smokeRenderablesBeforeHide = context.rendering.stats().renderableCount;
        invokeOperator(context, "object.select_all", R"({"action":"select"})", KeymapRegion::Viewport);
        if (m_selection.empty()) {
            // An empty startup scene has nothing to hide.
            context.logger.log(
                LogLevel::Info, "editor",
                "Hide smoke skipped: " +
                    std::to_string(context.scene.registry().view<const SceneEntityIdComponent>().size()) +
                    " entities, none selectable");
            m_smokeHideSkipped = true;
            return;
        }
        if (!invokeOperator(context, "object.hide", {}, KeymapRegion::Viewport)) {
            fail("object.hide did not run");
        }
        return;
    }
    if (m_smokeHideSkipped) {
        return;
    }
    const uint32_t renderablesWhileHidden = context.rendering.stats().renderableCount;
    if (m_smokeRenderablesBeforeHide > 0 && renderablesWhileHidden >= m_smokeRenderablesBeforeHide) {
        fail("hidden objects were still rendered");
    }
    if (!invokeOperator(context, "object.unhide", {}, KeymapRegion::Viewport)) {
        fail("object.unhide did not run");
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
    context.logger.log(LogLevel::Error, "editor", error);
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
        return;
    }
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
        widgets::StatusPill(EditorIcon::Stop, "Edit", buttonTop, buttonBottom, ink);
        break;
    case EditorMode::Play:
        widgets::StatusPill(EditorIcon::Play, "Play", buttonActiveTop, buttonActiveBottom, activeInk);
        break;
    case EditorMode::Paused:
        widgets::StatusPill(EditorIcon::Pause, "Paused", buttonHoverTop, buttonHoverBottom, ink);
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
