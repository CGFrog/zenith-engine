// EditorSession's prefab windows: prefabs open in floating windows of their
// own (PrefabWindowSet) while the level stays open, editable, and playable
// underneath. Each window shows its own Hierarchy and Inspector on the
// window's document, runs operators on it, and saves to the prefab's file,
// refreshing the prefab's instances in every other open document. Windows
// open and close between frames' draws or by request, never while their
// panels draw.

#include "EditorSession.hpp"

#include "editor/application/PrefabInstanceRefresh.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/EditorWidgets.hpp"
#include "editor/viewport/ViewFraming.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace {
constexpr const char* prefabDiagnosticSource = "Prefabs";

std::string windowLabel(const PrefabWindowDocument& window) {
    return window.target.displayName + (window.document.dirty() ? "*" : "");
}

// The ImGui name of window's part (its host or one of its panels), unique to
// the window so no two windows or level areas share a dock slot.
std::string windowPartName(const PrefabWindowDocument& window, std::string_view title, std::string_view part) {
    return std::string(title) + "##prefab." + std::to_string(window.id) + "." + std::string(part);
}

// window's own ApplicationContext: the session's, with the window's scene, so
// panels that read the scene from it show the prefab.
ApplicationContext windowApplicationContext(ApplicationContext& context, PrefabWindowDocument& window) {
    return {.scene = window.scene,
            .assets = context.assets,
            .physics = context.physics,
            .audio = context.audio,
            .control = context.control,
            .rendering = context.rendering,
            .gameUi = context.gameUi,
            .logger = context.logger,
            .frameTimings = context.frameTimings};
}
} // namespace

// What a prefab window's panels can ask of the session: operators and keys
// act on the window's document; everything else is the session's own, with
// the session's ApplicationContext (never the window's, whose scene is the
// prefab's).
class EditorSession::PrefabWindowActions final : public EditorSessionActions {
  public:
    PrefabWindowActions(EditorSession& session, ApplicationContext& context, PrefabWindowDocument& window)
        : m_session(session)
        , m_context(context)
        , m_window(window) {}

    OperatorResult invokeOperator(ApplicationContext&, std::string_view operatorId, std::string_view properties,
                                  KeymapRegion region) override {
        return m_session.invokeOperatorOn(m_context, &m_window, operatorId, properties, region);
    }

    bool operatorAvailable(ApplicationContext&, std::string_view operatorId) override {
        return m_session.operatorAvailableOn(m_context, &m_window, operatorId);
    }

    std::string operatorShortcut(std::string_view operatorId, std::string_view properties) const override {
        return m_session.operatorShortcut(operatorId, properties);
    }

    void requestOpenScene(ApplicationContext&, std::filesystem::path path) override {
        m_session.requestOpenScene(m_context, std::move(path));
    }

    void projectFilesMoved(ApplicationContext&, std::span<const ProjectFileMove> moves) override {
        m_session.projectFilesMoved(m_context, moves);
    }

    void projectFilesRemoved(ApplicationContext&, std::span<const std::filesystem::path> removed) override {
        m_session.projectFilesRemoved(m_context, removed);
    }

    void handleObjectKeys(ApplicationContext&, KeymapRegion region) override {
        m_session.handleObjectKeysOn(m_context, &m_window, region);
    }

    bool modalTransformActive() const override {
        return m_session.modalTransformActive();
    }

    // The transform knows which document it edits (EditorSession::
    // ActiveTransform::window), so the session's context serves.
    void updateModalTransform(ApplicationContext&, ViewportId viewport) override {
        m_session.updateModalTransform(m_context, viewport);
    }

    uint32_t dispatchMouseChords(ApplicationContext&, KeymapRegion region, uint32_t allowedButtons) override {
        return m_session.dispatchMouseChordsOn(m_context, &m_window, region, allowedButtons);
    }

    bool mouseButtonBound(KeymapRegion region, int button) const override {
        return m_session.mouseButtonBound(region, button);
    }

  private:
    EditorSession& m_session;
    ApplicationContext& m_context;
    PrefabWindowDocument& m_window;
};

PrefabWindowDocument* EditorSession::focusedPrefabWindow() const {
    return m_focusedPrefabWindow == 0 ? nullptr : m_prefabWindows.find(m_focusedPrefabWindow);
}

bool EditorSession::prefabCloseAvailable() const {
    return focusedPrefabWindow() != nullptr;
}

bool EditorSession::prefabEditAvailable() const {
    // A window opens beside the level, so the level may be playing.
    return !m_fileDialog.pending() && !m_activeTransform;
}

void EditorSession::requestCloseFocusedPrefab(ApplicationContext&) {
    if (const PrefabWindowDocument* window = focusedPrefabWindow()) {
        requestClosePrefabWindow(window->id);
    }
}

// Opens the prefab in a window of its own, or brings its window forward when
// it is open; the level stays as it is.
OperatorResult EditorSession::editPrefab(ApplicationContext& context, const std::filesystem::path& reference) {
    PrefabWindowDocument* window = nullptr;
    const PrefabResult opened = m_prefabWindows.open(reference, m_document, context.assets, window);
    for (const std::string& diagnostic : opened.diagnostics) {
        m_diagnostics.add(EditorDiagnosticSeverity::Warning, prefabDiagnosticSource, diagnostic);
    }
    if (!opened) {
        return OperatorResult::failed("Could not edit the prefab: " + opened.error);
    }
    PrefabWindowView* view = prefabWindowView(window->id);
    if (view == nullptr) {
        addPrefabWindowView(context, *window);
        notePrefabFilesRead(window->scene, m_document.options().assetRoot, prefabWriteHooks());
        view = prefabWindowView(window->id);
    }
    // Opening a prefab that is already open brings its window to the front.
    view->focusRequest = true;
    m_focusedPrefabWindow = window->id;
    return OperatorResult::finished();
}

void EditorSession::addPrefabWindowView(ApplicationContext& context, PrefabWindowDocument& window) {
    auto view = std::make_unique<PrefabWindowView>();
    view->id = window.id;
    view->hierarchy = createPrefabWindowPanel("hierarchy");
    view->inspector = createPrefabWindowPanel("inspector");
    view->viewport = createPrefabWindowPanel("viewport");
    window.renderScene = context.rendering.createScene();
    window.assetLeases = context.assets.leaseSceneAssets(window.scene);
    if (auto* viewport = dynamic_cast<ViewportPanel*>(view->viewport.get())) {
        // Without a free render scene the viewport shows a placeholder until
        // bindPrefabWindowScenes finds one.
        viewport->showDocument(&window.document, window.renderScene);
        window.scene.updateWorldTransforms();
        std::vector<SceneEntityId> everything;
        for (const Entity entity : window.scene.registry().view<const TransformComponent>()) {
            everything.push_back(window.scene.id(entity));
        }
        frameEntities(viewport->camera(), window.scene, context.assets, everything);
    }
    m_prefabWindowViews.push_back(std::move(view));
}

void EditorSession::bindPrefabWindowScenes(ApplicationContext& context) {
    for (const std::unique_ptr<PrefabWindowDocument>& window : m_prefabWindows.windows()) {
        if (!window->renderScene) {
            // Opened while every render scene was taken: try again, since a
            // closed window frees one.
            window->renderScene = context.rendering.createScene();
            const PrefabWindowView* view = prefabWindowView(window->id);
            auto* viewport = view != nullptr ? dynamic_cast<ViewportPanel*>(view->viewport.get()) : nullptr;
            if (!window->renderScene || viewport == nullptr) {
                continue;
            }
            viewport->showDocument(&window->document, window->renderScene);
        }
        window->scene.updateWorldTransforms();
        context.rendering.setSceneContent(window->renderScene, &window->scene);
    }
}

std::shared_ptr<EditorPanel> EditorSession::createPrefabWindowPanel(std::string_view typeId) {
    const AreaType* type = m_areaTypes.find(typeId);
    if (type == nullptr || !type->create) {
        return nullptr;
    }
    return type->create();
}

EditorSession::PrefabWindowView* EditorSession::prefabWindowView(uint64_t id) {
    for (const std::unique_ptr<PrefabWindowView>& view : m_prefabWindowViews) {
        if (view->id == id) {
            return view.get();
        }
    }
    return nullptr;
}

void EditorSession::requestClosePrefabWindow(uint64_t id) {
    const PrefabWindowDocument* window = m_prefabWindows.find(id);
    if (window == nullptr) {
        return;
    }
    if (window->document.dirty()) {
        m_prefabWindowPrompt = PrefabWindowPrompt{.id = id, .continuesPendingAction = false};
        m_openPrefabWindowPrompt = true;
        return;
    }
    m_prefabWindowCloses.push_back(id);
}

void EditorSession::applyPrefabWindowCloses(ApplicationContext& context) {
    if (m_prefabWindowCloses.empty()) {
        return;
    }
    for (const uint64_t id : std::exchange(m_prefabWindowCloses, {})) {
        if (m_activeTransform && m_activeTransform->window == id) {
            // Before the document it edits goes.
            finishModalTransform(context, false);
        }
        // The panels first: the viewport releases its render view.
        std::erase_if(m_prefabWindowViews,
                      [id](const std::unique_ptr<PrefabWindowView>& view) { return view->id == id; });
        if (const PrefabWindowDocument* window = m_prefabWindows.find(id)) {
            context.rendering.destroyScene(window->renderScene);
            if (m_eyedropper.owner() == &window->document) {
                m_eyedropper.cancel();
            }
        }
        m_prefabWindows.close(id);
        if (m_focusedPrefabWindow == id) {
            m_focusedPrefabWindow = 0;
        }
    }
    // A closed window's changes are gone; so is its autosave.
    autosavePrefabWindows(context, true);
}

void EditorSession::closeAllPrefabWindows(ApplicationContext& context) {
    if (m_activeTransform && m_activeTransform->window != 0) {
        finishModalTransform(context, false);
    }
    m_prefabWindowCloses.clear();
    m_prefabWindowPrompt.reset();
    m_prefabWindowViews.clear();
    for (const std::unique_ptr<PrefabWindowDocument>& window : m_prefabWindows.windows()) {
        context.rendering.destroyScene(window->renderScene);
        if (m_eyedropper.owner() == &window->document) {
            m_eyedropper.cancel();
        }
    }
    m_prefabWindows.closeAll();
    m_focusedPrefabWindow = 0;
}

bool EditorSession::savePrefabWindow(ApplicationContext& context, PrefabWindowDocument& window) {
    SavedPrefabDocument saved;
    const PrefabResult result = m_prefabWindows.save(window, context.assets, m_diagnostics, prefabWriteHooks(), saved);
    for (const std::string& diagnostic : result.diagnostics) {
        m_diagnostics.add(EditorDiagnosticSeverity::Warning, prefabDiagnosticSource, diagnostic);
    }
    if (!result) {
        m_diagnostics.reportError("Save failed: " + result.error);
        return false;
    }
    refreshAfterPrefabSave(context, window, saved);
    // The saved window no longer needs its autosave.
    autosavePrefabWindows(context, true);
    m_diagnostics.acknowledgeErrors();
    return true;
}

void EditorSession::refreshAfterPrefabSave(ApplicationContext& context, PrefabWindowDocument& window,
                                           const SavedPrefabDocument& saved) {
    if (saved.before == saved.after) {
        return;
    }
    PrefabFileChange change{.reference = window.target.reference, .displayName = window.target.displayName};
    if (PrefabDocument previous; saved.before && deserializePrefab(*saved.before, previous).success) {
        change.previous = std::move(previous);
    }
    if (const size_t refreshed = refreshPrefabInstancesElsewhere(context, change, &window, true); refreshed > 0) {
        m_diagnostics.add(EditorDiagnosticSeverity::Info, prefabDiagnosticSource,
                          "Saved " + change.displayName + "; refreshed " + std::to_string(refreshed) +
                              (refreshed == 1 ? " instance" : " instances") +
                              " in the open documents. Undo there puts them back.");
    }
}

size_t EditorSession::refreshPrefabInstancesElsewhere(ApplicationContext& context, const PrefabFileChange& change,
                                                      const PrefabWindowDocument* originWindow, bool refreshLevel) {
    const PrefabDocument* previous = change.previous ? &*change.previous : nullptr;
    const std::string stepName = "Refresh " + change.displayName + " instances";
    PrefabResult result{.success = true};
    const auto combine = [&result](const PrefabResult& part) {
        result.diagnostics.insert(result.diagnostics.end(), part.diagnostics.begin(), part.diagnostics.end());
        if (!part && result) {
            result.success = false;
            result.error = part.error;
        }
    };
    size_t inLevel = 0;
    if (refreshLevel) {
        // During Play its edit scene; the running scene keeps its copies
        // until Stop.
        Scene* editScene = m_playMode.editScene();
        Scene& levelScene = editScene != nullptr ? *editScene : context.scene;
        const SceneEditing levelEditing(levelScene, context.assets, m_document, m_history, m_diagnostics);
        combine(refreshPrefabInstances(levelEditing, change.reference, previous, stepName, &inLevel));
        if (editScene == nullptr) {
            m_selection.removeMissing(levelScene);
        }
    }
    size_t inWindows = 0;
    combine(m_prefabWindows.refreshInstances(context.assets, m_diagnostics, change.reference, previous, stepName,
                                             originWindow, inWindows));
    for (const std::string& diagnostic : result.diagnostics) {
        m_diagnostics.add(EditorDiagnosticSeverity::Warning, prefabDiagnosticSource, diagnostic);
    }
    if (!result) {
        m_diagnostics.reportError("Could not refresh " + change.displayName + " instances: " + result.error);
    }
    return inLevel + inWindows;
}

OperatorContext EditorSession::prefabWindowOperatorContext(ApplicationContext& context, KeymapRegion region,
                                                           PrefabWindowDocument& window) {
    // A prefab document is never Play's runtime scene: it records history
    // and stays editable while the level plays.
    return {.editing = {window.scene, context.assets, window.document, window.history, m_diagnostics},
            .selection = window.selection,
            .mode = EditorMode::Edit,
            .region = region,
            .app = &context};
}

EditorContext EditorSession::prefabWindowEditorContext(ApplicationContext& windowContext, ApplicationContext& context,
                                                       PrefabWindowDocument& window, PrefabWindowActions& actions) {
    return {.app = windowContext,
            .editing = {window.scene, context.assets, window.document, window.history, m_diagnostics},
            .selection = window.selection,
            .diagnostics = m_diagnostics,
            .mode = EditorMode::Edit,
            .actions = actions,
            .eyedropper = &m_eyedropper,
            .menus = m_menus,
            .simulating = false,
            .componentEditors = &m_componentEditors,
            .navigationPreview = nullptr,
            .navigationSettings = &m_navigationSettings,
            .gameServices = nullptr,
            .scripts = this,
            .watches = &window.watches,
            .projectNames = &m_projectNames,
            .prefabEdit = OpenPrefabSummary{.displayName = window.target.displayName, .root = window.target.root},
            .projectAssets = &m_projectAssets,
            .fileDrops = &m_fileDrops,
            .definitionSchemas =
                m_moduleHost.module() != nullptr ? m_moduleHost.module()->definitionSchemas().get() : nullptr};
}

std::vector<std::pair<PrefabWindowDocument*, EditorSession::PrefabWindowView*>> EditorSession::prefabWindowSnapshot() {
    // Snapshot: an operator a window's panel runs may open another window.
    std::vector<std::pair<PrefabWindowDocument*, PrefabWindowView*>> windows;
    for (const std::unique_ptr<PrefabWindowView>& view : m_prefabWindowViews) {
        if (PrefabWindowDocument* window = m_prefabWindows.find(view->id)) {
            windows.emplace_back(window, view.get());
        }
    }
    return windows;
}

void EditorSession::updatePrefabWindows(ApplicationContext& context, const InputState& input, float deltaSeconds) {
    for (const auto& [window, view] : prefabWindowSnapshot()) {
        ApplicationContext windowContext = windowApplicationContext(context, *window);
        PrefabWindowActions actions(*this, context, *window);
        EditorContext editor = prefabWindowEditorContext(windowContext, context, *window, actions);
        for (const std::shared_ptr<EditorPanel>& panel : {view->hierarchy, view->viewport, view->inspector}) {
            if (panel) {
                panel->update(editor, input, deltaSeconds);
            }
        }
    }
}

void EditorSession::leasePrefabWindowAssets(ApplicationContext& context) {
    for (const std::unique_ptr<PrefabWindowDocument>& window : m_prefabWindows.windows()) {
        window->assetLeases = context.assets.leaseSceneAssets(window->scene);
    }
}

void EditorSession::withFocusedEditorContext(ApplicationContext& context,
                                             const std::function<void(EditorContext&)>& draw) {
    PrefabWindowDocument* window = focusedPrefabWindow();
    if (window == nullptr) {
        EditorContext editor = editorContext(context);
        draw(editor);
        return;
    }
    ApplicationContext windowContext = windowApplicationContext(context, *window);
    PrefabWindowActions actions(*this, context, *window);
    EditorContext editor = prefabWindowEditorContext(windowContext, context, *window, actions);
    draw(editor);
}

void EditorSession::reconcilePrefabWindows(ApplicationContext& context, const PrefabFileChange& change) {
    if (m_prefabWindows.empty()) {
        return;
    }
    if (m_prefabWindows.find(change.reference) != nullptr) {
        m_diagnostics.add(EditorDiagnosticSeverity::Warning, prefabDiagnosticSource,
                          change.displayName +
                              " changed on disk while it is open in a window. Save there writes that window's "
                              "version over the change; closing without saving keeps the file's.");
    }
    size_t refreshed = 0;
    const PrefabResult result = m_prefabWindows.refreshInstances(
        context.assets, m_diagnostics, change.reference, change.previous ? &*change.previous : nullptr,
        externalPrefabRefreshStepName(change.displayName), nullptr, refreshed);
    for (const std::string& diagnostic : result.diagnostics) {
        m_diagnostics.add(EditorDiagnosticSeverity::Warning, prefabDiagnosticSource, diagnostic);
    }
    if (!result) {
        m_diagnostics.reportError("Could not refresh " + change.displayName +
                                  " instances in the prefab windows: " + result.error);
    }
}

void EditorSession::drawPrefabWindows(ApplicationContext& context) {
    for (const auto& [window, view] : prefabWindowSnapshot()) {
        drawPrefabWindow(context, *window, *view);
    }
    drawPrefabWindowPrompt(context);
}

void EditorSession::drawPrefabWindow(ApplicationContext& context, PrefabWindowDocument& window,
                                     PrefabWindowView& view) {
    const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    if (view.focusRequest) {
        ImGui::SetNextWindowFocus();
        ImGui::SetNextWindowCollapsed(false);
        view.focusRequest = false;
    }
    // Opens floating, centred over the editor, and keeps where the user moves it.
    ImGui::SetNextWindowPos({mainViewport->WorkPos.x + mainViewport->WorkSize.x * 0.5f,
                             mainViewport->WorkPos.y + mainViewport->WorkSize.y * 0.5f},
                            ImGuiCond_Appearing, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({mainViewport->WorkSize.x * 0.6f, mainViewport->WorkSize.y * 0.6f}, ImGuiCond_Appearing);
    const std::string hostName = "Prefab: " + windowLabel(window) + "###prefab." + std::to_string(window.id);
    bool open = true;
    const bool visible =
        ImGui::Begin(hostName.c_str(), &open,
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        m_focusedPrefabWindow = window.id;
    }
    const ImGuiID dockspaceId = ImGui::GetID("##PrefabDockspace");
    if (visible) {
        m_themeChrome.paintAreaWindow();
        drawPrefabWindowToolbar(context, window);
        if (!view.layoutBuilt) {
            buildPrefabWindowLayout(window, dockspaceId);
            view.layoutBuilt = true;
        }
        ImGui::DockSpace(dockspaceId, {0.0f, 0.0f}, ImGuiDockNodeFlags_NoWindowMenuButton);
    }
    ImGui::End();
    if (!open) {
        requestClosePrefabWindow(window.id);
    }
    ApplicationContext windowContext = windowApplicationContext(context, window);
    PrefabWindowActions actions(*this, context, window);
    EditorContext editor = prefabWindowEditorContext(windowContext, context, window, actions);
    if (!visible) {
        for (const std::shared_ptr<EditorPanel>& panel : {view.hierarchy, view.viewport, view.inspector}) {
            if (panel) {
                panel->hidden(editor);
            }
        }
        return;
    }
    const auto drawPanel = [&](const std::shared_ptr<EditorPanel>& panel, std::string_view part) {
        if (!panel) {
            return;
        }
        const std::string name = windowPartName(window, panel->title(), part);
        // The theme paints the pane in place of the window background.
        ImGui::PushStyleColor(ImGuiCol_WindowBg, toImVec4(palette::transparent));
        const bool panelVisible =
            ImGui::Begin(name.c_str(), nullptr,
                         panel->windowFlags() | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
        ImGui::PopStyleColor();
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
            m_focusedPrefabWindow = window.id;
        }
        if (panelVisible) {
            m_themeChrome.paintAreaWindow();
            panel->draw(editor);
        } else {
            panel->hidden(editor);
        }
        ImGui::End();
    };
    drawPanel(view.hierarchy, "hierarchy");
    drawPanel(view.viewport, "viewport");
    drawPanel(view.inspector, "inspector");
}

void EditorSession::buildPrefabWindowLayout(const PrefabWindowDocument& window, ImGuiID dockspaceId) {
    const ImVec2 size = ImGui::GetContentRegionAvail();
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, {std::max(size.x, 1.0f), std::max(size.y, 1.0f)});
    ImGuiID center = dockspaceId;
    const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.25f, nullptr, &center);
    const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.35f, nullptr, &center);
    ImGui::DockBuilderDockWindow(windowPartName(window, "Hierarchy", "hierarchy").c_str(), left);
    ImGui::DockBuilderDockWindow(windowPartName(window, "Viewport", "viewport").c_str(), center);
    ImGui::DockBuilderDockWindow(windowPartName(window, "Inspector", "inspector").c_str(), right);
    ImGui::DockBuilderFinish(dockspaceId);
}

void EditorSession::drawPrefabWindowToolbar(ApplicationContext& context, PrefabWindowDocument& window) {
    ImGui::BeginDisabled(!m_prefabWindows.find(window.id) || m_fileDialog.pending() || m_activeTransform.has_value());
    if (widgets::Button("Save##prefab_window_save")) {
        savePrefabWindow(context, window);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Write the prefab to its file and refresh its instances in the open documents  %s",
                          m_keymap.shortcutLabel("file.save", {}).c_str());
    ImGui::SameLine();
    if (widgets::Button("Close##prefab_window_close")) {
        requestClosePrefabWindow(window.id);
    }
    ImGui::SetItemTooltip("Close this prefab, asking about unsaved changes");
    ImGui::SameLine();
    widgets::CaptionText(window.target.reference.generic_string().c_str(), palette::inkMuted);
}

void EditorSession::drawPrefabWindowPrompt(ApplicationContext& context) {
    constexpr const char* popupName = "Unsaved Prefab";
    if (m_openPrefabWindowPrompt) {
        ImGui::OpenPopup(popupName);
        m_openPrefabWindowPrompt = false;
    }
    if (!ImGui::BeginPopupModal(popupName, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    PrefabWindowDocument* window = m_prefabWindowPrompt ? m_prefabWindows.find(m_prefabWindowPrompt->id) : nullptr;
    if (window == nullptr) {
        m_prefabWindowPrompt.reset();
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const PrefabWindowPrompt prompt = *m_prefabWindowPrompt;
    ImGui::Text("Save changes to %s before closing it?", window->target.displayName.c_str());
    const auto answered = [&](bool close) {
        m_prefabWindowPrompt.reset();
        ImGui::CloseCurrentPopup();
        if (close) {
            m_prefabWindowCloses.push_back(prompt.id);
        }
        if (prompt.continuesPendingAction) {
            if (close) {
                continuePendingAction(context);
            } else {
                m_pendingAction = PendingAction::None;
            }
        }
    };
    if (widgets::Button("Save")) {
        answered(savePrefabWindow(context, *window));
    }
    ImGui::SameLine();
    if (widgets::Button("Discard")) {
        // Closed now rather than between frames, so continuing the pending
        // action sees it gone.
        window->document.setDirty(false);
        answered(true);
    }
    ImGui::SameLine();
    if (widgets::Button("Cancel")) {
        answered(false);
    }
    ImGui::EndPopup();
}

bool EditorSession::askAboutPrefabWindows() {
    for (const std::unique_ptr<PrefabWindowDocument>& window : m_prefabWindows.windows()) {
        if (window->document.dirty() &&
            std::ranges::find(m_prefabWindowCloses, window->id) == m_prefabWindowCloses.end()) {
            m_prefabWindowPrompt = PrefabWindowPrompt{.id = window->id, .continuesPendingAction = true};
            m_openPrefabWindowPrompt = true;
            return true;
        }
    }
    return false;
}

void EditorSession::autosavePrefabWindows(ApplicationContext& context, bool reportErrors) {
    std::vector<PrefabWindowAutosave> unsaved;
    for (const std::unique_ptr<PrefabWindowDocument>& window : m_prefabWindows.windows()) {
        if (window->document.dirty()) {
            unsaved.push_back({.scene = window->scene,
                               .options = window->document.options(),
                               .note = {.reference = window->target.reference,
                                        .root = window->target.root,
                                        .displayName = window->target.displayName}});
        }
    }
    if (const SceneDocumentResult result = m_prefabWindowRecovery.autosave(unsaved, context.assets);
        !result && reportErrors) {
        m_diagnostics.reportError("Autosave failed: " + result.error);
    }
}

void EditorSession::recoverPrefabWindows(ApplicationContext& context) {
    for (const PrefabEditRecovery& recovery : m_prefabWindowRecovery.available()) {
        Scene recovered;
        PrefabEditRecoveryNote note;
        if (const SceneDocumentResult result = recovery.recover(recovered, context.assets, m_document.options(), note);
            !result) {
            m_diagnostics.reportError("Could not recover a prefab window: " + result.error);
            continue;
        }
        std::filesystem::path file;
        if (const PrefabResult resolved =
                PrefabSystem(context.assets, m_document.options()).resolve(note.reference, file);
            !resolved || recovered.findEntity(note.root) == entt::null) {
            m_diagnostics.reportError("Could not recover the prefab window of " + note.reference.generic_string() +
                                      ": " + (resolved ? "its root is missing" : resolved.error));
            continue;
        }
        if (m_prefabWindows.find(note.reference) != nullptr) {
            continue;
        }
        std::error_code error;
        const std::filesystem::path absolute = std::filesystem::absolute(file, error);
        PrefabWindowDocument& window = m_prefabWindows.adopt(recovered,
                                                             {.reference = note.reference,
                                                              .file = (error ? file : absolute).lexically_normal(),
                                                              .displayName = note.displayName,
                                                              .root = note.root},
                                                             m_document);
        addPrefabWindowView(context, window);
        notePrefabFilesRead(window.scene, m_document.options().assetRoot, prefabWriteHooks());
    }
}
