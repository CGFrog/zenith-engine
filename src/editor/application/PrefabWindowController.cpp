// The prefab windows: prefabs open in floating windows of their own
// (PrefabWindowSet) while the level stays open, editable, and playable
// underneath. Each window shows its own Hierarchy and Inspector on the
// window's document, runs operators on it, and saves to the prefab's file,
// refreshing the prefab's instances in every other open document. Windows
// open and close between frames' draws or by request, never while their
// panels draw.

#include "PrefabWindowController.hpp"

#include "editor/application/PrefabInstanceRefresh.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/areas/AreaRegistry.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/interaction/EntityEyedropper.hpp"
#include "editor/operators/Keymap.hpp"
#include "editor/panels/EditorPanel.hpp"
#include "editor/panels/ViewportPanel.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/EditorWidgets.hpp"
#include "editor/theme/ThemeChrome.hpp"
#include "editor/viewport/ViewFraming.hpp"
#include "engine/application/Application.hpp"

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
class PrefabWindowController::Actions final : public EditorSessionActions {
  public:
    Actions(PrefabWindowHost& host, ApplicationContext& context, PrefabWindowDocument& window)
        : m_host(host)
        , m_level(host.levelActions())
        , m_context(context)
        , m_window(window) {}

    OperatorResult invokeOperator(ApplicationContext&, std::string_view operatorId, std::string_view properties,
                                  KeymapRegion region) override {
        return m_host.invokeOperatorOn(m_context, &m_window, operatorId, properties, region);
    }

    bool operatorAvailable(ApplicationContext&, std::string_view operatorId) override {
        return m_host.operatorAvailableOn(m_context, &m_window, operatorId);
    }

    std::string operatorShortcut(std::string_view operatorId, std::string_view properties) const override {
        return m_level.operatorShortcut(operatorId, properties);
    }

    void requestOpenScene(ApplicationContext&, std::filesystem::path path) override {
        m_level.requestOpenScene(m_context, std::move(path));
    }

    void projectFilesMoved(ApplicationContext&, std::span<const ProjectFileMove> moves) override {
        m_level.projectFilesMoved(m_context, moves);
    }

    void projectFilesRemoved(ApplicationContext&, std::span<const std::filesystem::path> removed) override {
        m_level.projectFilesRemoved(m_context, removed);
    }

    void handleObjectKeys(ApplicationContext&, KeymapRegion region) override {
        m_host.handleObjectKeysOn(m_context, &m_window, region);
    }

    bool modalTransformActive() const override {
        return m_level.modalTransformActive();
    }

    // The transform knows which document it edits (EditorSession::
    // ActiveTransform::window), so the session's context serves.
    void updateModalTransform(ApplicationContext&, ViewportId viewport) override {
        m_level.updateModalTransform(m_context, viewport);
    }

    uint32_t dispatchMouseChords(ApplicationContext&, KeymapRegion region, uint32_t allowedButtons) override {
        return m_host.dispatchMouseChordsOn(m_context, &m_window, region, allowedButtons);
    }

    bool mouseButtonBound(KeymapRegion region, int button) const override {
        return m_level.mouseButtonBound(region, button);
    }

  private:
    PrefabWindowHost& m_host;
    EditorSessionActions& m_level;
    ApplicationContext& m_context;
    PrefabWindowDocument& m_window;
};

PrefabWindowController::PrefabWindowController(PrefabWindowServices services)
    : m_services(std::move(services))
    , m_recovery(m_services.recoveryDirectory) {}

PrefabWindowSet& PrefabWindowController::windows() {
    return m_windows;
}

const PrefabWindowSet& PrefabWindowController::windows() const {
    return m_windows;
}

void PrefabWindowController::clearFocus() {
    m_focused = 0;
}

ViewportPanel* PrefabWindowController::viewport(uint64_t window) const {
    const View* found = findView(window);
    return found != nullptr ? dynamic_cast<ViewportPanel*>(found->viewport.get()) : nullptr;
}

bool PrefabWindowController::panelsDocked(uint64_t window) const {
    const View* found = findView(window);
    const PrefabWindowDocument* document = m_windows.find(window);
    if (found == nullptr || document == nullptr || found->dockspaceId == 0) {
        return false;
    }
    const std::pair<const std::shared_ptr<EditorPanel>&, std::string_view> panels[] = {
        {found->hierarchy, "hierarchy"}, {found->viewport, "viewport"}, {found->inspector, "inspector"}};
    for (const auto& [panel, part] : panels) {
        if (!panel) {
            continue;
        }
        const ImGuiWindow* panelWindow =
            ImGui::FindWindowByName(windowPartName(*document, panel->title(), part).c_str());
        if (panelWindow == nullptr || panelWindow->DockNode == nullptr ||
            ImGui::DockNodeGetRootNode(panelWindow->DockNode)->ID != found->dockspaceId) {
            return false;
        }
    }
    return true;
}

PrefabWindowDocument* PrefabWindowController::focused() const {
    return m_focused == 0 ? nullptr : m_windows.find(m_focused);
}

bool PrefabWindowController::prefabCloseAvailable() const {
    return focused() != nullptr;
}

bool PrefabWindowController::prefabEditAvailable() const {
    // A window opens beside the level, so the level may be playing.
    return !m_services.host.prefabWindowToolsBusy();
}

void PrefabWindowController::requestCloseFocusedPrefab(ApplicationContext&) {
    if (const PrefabWindowDocument* window = focused()) {
        requestClose(window->id);
    }
}

// Opens the prefab in a window of its own, or brings its window forward when
// it is open; the level stays as it is.
OperatorResult PrefabWindowController::editPrefab(ApplicationContext& context, const std::filesystem::path& reference) {
    PrefabWindowDocument* window = nullptr;
    const PrefabResult opened = m_windows.open(reference, m_services.document, context.assets, window);
    for (const std::string& diagnostic : opened.diagnostics) {
        m_services.diagnostics.add(EditorDiagnosticSeverity::Warning, prefabDiagnosticSource, diagnostic);
    }
    if (!opened) {
        return OperatorResult::failed("Could not edit the prefab: " + opened.error);
    }
    View* view = findView(window->id);
    if (view == nullptr) {
        addView(context, *window);
        notePrefabFilesRead(window->scene, m_services.document.options().assetRoot, m_services.host.prefabWriteHooks());
        view = findView(window->id);
    }
    // Opening a prefab that is already open brings its window to the front.
    view->focusRequest = true;
    m_focused = window->id;
    return OperatorResult::finished();
}

void PrefabWindowController::addView(ApplicationContext& context, PrefabWindowDocument& window) {
    auto view = std::make_unique<View>();
    view->id = window.id;
    view->hierarchy = createPanel("hierarchy");
    view->inspector = createPanel("inspector");
    view->viewport = createPanel("viewport");
    window.renderScene = context.rendering.createScene();
    window.assetLeases = context.assets.leaseSceneAssets(window.scene);
    if (auto* viewport = dynamic_cast<ViewportPanel*>(view->viewport.get())) {
        // Without a free render scene the viewport shows a placeholder until
        // bindScenes finds one.
        viewport->showDocument(&window.document, window.renderScene);
        window.scene.updateWorldTransforms();
        std::vector<SceneEntityId> everything;
        for (const Entity entity : window.scene.registry().view<const TransformComponent>()) {
            everything.push_back(window.scene.id(entity));
        }
        frameEntities(viewport->camera(), window.scene, context.assets, everything);
    }
    m_views.push_back(std::move(view));
}

void PrefabWindowController::bindScenes(ApplicationContext& context) {
    for (const std::unique_ptr<PrefabWindowDocument>& window : m_windows.windows()) {
        if (!window->renderScene) {
            // Opened while every render scene was taken: try again, since a
            // closed window frees one.
            window->renderScene = context.rendering.createScene();
            const View* view = findView(window->id);
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

std::shared_ptr<EditorPanel> PrefabWindowController::createPanel(std::string_view typeId) const {
    const AreaType* type = m_services.areaTypes.find(typeId);
    if (type == nullptr || !type->create) {
        return nullptr;
    }
    return type->create();
}

PrefabWindowController::View* PrefabWindowController::findView(uint64_t id) const {
    for (const std::unique_ptr<View>& view : m_views) {
        if (view->id == id) {
            return view.get();
        }
    }
    return nullptr;
}

void PrefabWindowController::requestClose(uint64_t id) {
    const PrefabWindowDocument* window = m_windows.find(id);
    if (window == nullptr) {
        return;
    }
    if (window->document.dirty()) {
        m_prompt = Prompt{.id = id, .continuesPendingAction = false};
        m_openPrompt = true;
        return;
    }
    m_closes.push_back(id);
}

void PrefabWindowController::releaseWindow(ApplicationContext& context, const PrefabWindowDocument& window) {
    context.rendering.destroyScene(window.renderScene);
    if (m_services.eyedropper.owner() == &window.document) {
        m_services.eyedropper.cancel();
    }
}

void PrefabWindowController::applyCloses(ApplicationContext& context) {
    if (m_closes.empty()) {
        return;
    }
    for (const uint64_t id : std::exchange(m_closes, {})) {
        // Before the document it edits goes.
        m_services.host.cancelModalTransformIn(context, id);
        // The panels next: the viewport releases its render view.
        std::erase_if(m_views, [id](const std::unique_ptr<View>& view) { return view->id == id; });
        if (const PrefabWindowDocument* window = m_windows.find(id)) {
            releaseWindow(context, *window);
        }
        m_windows.close(id);
        if (m_focused == id) {
            m_focused = 0;
        }
    }
    // A closed window's changes are gone; so is its autosave.
    autosave(context, true);
}

void PrefabWindowController::closeAll(ApplicationContext& context) {
    for (const std::unique_ptr<PrefabWindowDocument>& window : m_windows.windows()) {
        m_services.host.cancelModalTransformIn(context, window->id);
    }
    m_closes.clear();
    m_prompt.reset();
    m_views.clear();
    for (const std::unique_ptr<PrefabWindowDocument>& window : m_windows.windows()) {
        releaseWindow(context, *window);
    }
    m_windows.closeAll();
    m_focused = 0;
}

bool PrefabWindowController::save(ApplicationContext& context, PrefabWindowDocument& window) {
    SavedPrefabDocument saved;
    const PrefabResult result =
        m_windows.save(window, context.assets, m_services.diagnostics, m_services.host.prefabWriteHooks(), saved);
    for (const std::string& diagnostic : result.diagnostics) {
        m_services.diagnostics.add(EditorDiagnosticSeverity::Warning, prefabDiagnosticSource, diagnostic);
    }
    if (!result) {
        m_services.diagnostics.reportError("Save failed: " + result.error);
        return false;
    }
    refreshAfterSave(context, window, saved);
    // The saved window no longer needs its autosave.
    autosave(context, true);
    m_services.diagnostics.acknowledgeErrors();
    return true;
}

void PrefabWindowController::refreshAfterSave(ApplicationContext& context, PrefabWindowDocument& window,
                                              const SavedPrefabDocument& saved) {
    if (saved.before == saved.after) {
        return;
    }
    PrefabFileChange change{.reference = window.target.reference, .displayName = window.target.displayName};
    if (PrefabDocument previous; saved.before && deserializePrefab(*saved.before, previous).success) {
        change.previous = std::move(previous);
    }
    if (const size_t refreshed = m_services.host.refreshPrefabInstancesElsewhere(context, change, &window, true);
        refreshed > 0) {
        m_services.diagnostics.add(EditorDiagnosticSeverity::Info, prefabDiagnosticSource,
                                   "Saved " + change.displayName + "; refreshed " + std::to_string(refreshed) +
                                       (refreshed == 1 ? " instance" : " instances") +
                                       " in the open documents. Undo there puts them back.");
    }
}

std::vector<std::pair<PrefabWindowDocument*, PrefabWindowController::View*>> PrefabWindowController::snapshot() const {
    // Snapshot: an operator a window's panel runs may open another window.
    std::vector<std::pair<PrefabWindowDocument*, View*>> windows;
    for (const std::unique_ptr<View>& view : m_views) {
        if (PrefabWindowDocument* window = m_windows.find(view->id)) {
            windows.emplace_back(window, view.get());
        }
    }
    return windows;
}

void PrefabWindowController::update(ApplicationContext& context, const InputState& input, float deltaSeconds) {
    for (const auto& [window, view] : snapshot()) {
        withEditorContext(context, *window, [&](EditorContext& editor) {
            for (const std::shared_ptr<EditorPanel>& panel : {view->hierarchy, view->viewport, view->inspector}) {
                if (panel) {
                    panel->update(editor, input, deltaSeconds);
                }
            }
        });
    }
    // Keeps the assets each window's scene uses loaded.
    for (const std::unique_ptr<PrefabWindowDocument>& window : m_windows.windows()) {
        window->assetLeases = context.assets.leaseSceneAssets(window->scene);
    }
}

void PrefabWindowController::withEditorContext(ApplicationContext& context, PrefabWindowDocument& window,
                                               const std::function<void(EditorContext&)>& draw) {
    ApplicationContext windowContext = windowApplicationContext(context, window);
    Actions actions(m_services.host, context, window);
    EditorContext editor = m_services.host.prefabWindowEditorContext(windowContext, context, window, actions);
    draw(editor);
}

void PrefabWindowController::reconcile(ApplicationContext& context, const PrefabFileChange& change) {
    if (m_windows.empty()) {
        return;
    }
    if (m_windows.find(change.reference) != nullptr) {
        m_services.diagnostics.add(EditorDiagnosticSeverity::Warning, prefabDiagnosticSource,
                                   change.displayName +
                                       " changed on disk while it is open in a window. Save there writes that window's "
                                       "version over the change; closing without saving keeps the file's.");
    }
    size_t refreshed = 0;
    const PrefabResult result = m_windows.refreshInstances(
        context.assets, m_services.diagnostics, change.reference, change.previous ? &*change.previous : nullptr,
        externalPrefabRefreshStepName(change.displayName), nullptr, refreshed);
    for (const std::string& diagnostic : result.diagnostics) {
        m_services.diagnostics.add(EditorDiagnosticSeverity::Warning, prefabDiagnosticSource, diagnostic);
    }
    if (!result) {
        m_services.diagnostics.reportError("Could not refresh " + change.displayName +
                                           " instances in the prefab windows: " + result.error);
    }
}

void PrefabWindowController::draw(ApplicationContext& context) {
    for (const auto& [window, view] : snapshot()) {
        drawWindow(context, *window, *view);
    }
    drawPrompt(context);
}

void PrefabWindowController::drawWindow(ApplicationContext& context, PrefabWindowDocument& window, View& view) {
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
        m_focused = window.id;
    }
    const ImGuiID dockspaceId = ImGui::GetID("##PrefabDockspace");
    view.dockspaceId = dockspaceId;
    if (visible) {
        m_services.themeChrome.paintAreaWindow();
        drawToolbar(context, window);
        if (!view.layoutBuilt) {
            buildLayout(window, dockspaceId);
            view.layoutBuilt = true;
        }
        ImGui::DockSpace(dockspaceId, {0.0f, 0.0f}, ImGuiDockNodeFlags_NoWindowMenuButton);
    }
    ImGui::End();
    if (!open) {
        requestClose(window.id);
    }
    ApplicationContext windowContext = windowApplicationContext(context, window);
    Actions actions(m_services.host, context, window);
    EditorContext editor = m_services.host.prefabWindowEditorContext(windowContext, context, window, actions);
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
            m_focused = window.id;
        }
        if (panelVisible) {
            m_services.themeChrome.paintAreaWindow();
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

void PrefabWindowController::buildLayout(const PrefabWindowDocument& window, ImGuiID dockspaceId) {
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

void PrefabWindowController::drawToolbar(ApplicationContext& context, PrefabWindowDocument& window) {
    ImGui::BeginDisabled(!m_windows.find(window.id) || m_services.host.prefabWindowToolsBusy());
    if (widgets::Button("Save##prefab_window_save")) {
        save(context, window);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Write the prefab to its file and refresh its instances in the open documents  %s",
                          m_services.keymap.shortcutLabel("file.save", {}).c_str());
    ImGui::SameLine();
    if (widgets::Button("Close##prefab_window_close")) {
        requestClose(window.id);
    }
    ImGui::SetItemTooltip("Close this prefab, asking about unsaved changes");
    ImGui::SameLine();
    widgets::CaptionText(window.target.reference.generic_string().c_str(), palette::inkMuted);
}

void PrefabWindowController::drawPrompt(ApplicationContext& context) {
    constexpr const char* popupName = "Unsaved Prefab";
    if (m_openPrompt) {
        ImGui::OpenPopup(popupName);
        m_openPrompt = false;
    }
    if (!ImGui::BeginPopupModal(popupName, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    PrefabWindowDocument* window = m_prompt ? m_windows.find(m_prompt->id) : nullptr;
    if (window == nullptr) {
        m_prompt.reset();
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const Prompt prompt = *m_prompt;
    ImGui::Text("Save changes to %s before closing it?", window->target.displayName.c_str());
    const auto answered = [&](bool close) {
        m_prompt.reset();
        ImGui::CloseCurrentPopup();
        if (close) {
            m_closes.push_back(prompt.id);
        }
        if (prompt.continuesPendingAction) {
            if (close) {
                m_services.host.continuePendingAction(context);
            } else {
                m_services.host.cancelPendingAction();
            }
        }
    };
    if (widgets::Button("Save")) {
        answered(save(context, *window));
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

bool PrefabWindowController::askAboutUnsaved() {
    for (const std::unique_ptr<PrefabWindowDocument>& window : m_windows.windows()) {
        if (window->document.dirty() && std::ranges::find(m_closes, window->id) == m_closes.end()) {
            m_prompt = Prompt{.id = window->id, .continuesPendingAction = true};
            m_openPrompt = true;
            return true;
        }
    }
    return false;
}

void PrefabWindowController::autosave(ApplicationContext& context, bool reportErrors) {
    std::vector<PrefabWindowAutosave> unsaved;
    for (const std::unique_ptr<PrefabWindowDocument>& window : m_windows.windows()) {
        if (window->document.dirty()) {
            unsaved.push_back({.scene = window->scene,
                               .options = window->document.options(),
                               .note = {.reference = window->target.reference,
                                        .root = window->target.root,
                                        .displayName = window->target.displayName}});
        }
    }
    if (const SceneDocumentResult result = m_recovery.autosave(unsaved, context.assets); !result && reportErrors) {
        m_services.diagnostics.reportError("Autosave failed: " + result.error);
    }
}

void PrefabWindowController::recover(ApplicationContext& context) {
    for (const PrefabEditRecovery& recovery : m_recovery.available()) {
        Scene recovered;
        PrefabEditRecoveryNote note;
        if (const SceneDocumentResult result =
                recovery.recover(recovered, context.assets, m_services.document.options(), note);
            !result) {
            m_services.diagnostics.reportError("Could not recover a prefab window: " + result.error);
            continue;
        }
        std::filesystem::path file;
        if (const PrefabResult resolved =
                PrefabSystem(context.assets, m_services.document.options()).resolve(note.reference, file);
            !resolved || recovered.findEntity(note.root) == entt::null) {
            m_services.diagnostics.reportError("Could not recover the prefab window of " +
                                               note.reference.generic_string() + ": " +
                                               (resolved ? "its root is missing" : resolved.error));
            continue;
        }
        if (m_windows.find(note.reference) != nullptr) {
            continue;
        }
        std::error_code error;
        const std::filesystem::path absolute = std::filesystem::absolute(file, error);
        PrefabWindowDocument& window = m_windows.adopt(recovered,
                                                       {.reference = note.reference,
                                                        .file = (error ? file : absolute).lexically_normal(),
                                                        .displayName = note.displayName,
                                                        .root = note.root},
                                                       m_services.document);
        addView(context, window);
        notePrefabFilesRead(window.scene, m_services.document.options().assetRoot, m_services.host.prefabWriteHooks());
    }
}

bool PrefabWindowController::anyUnsaved() const {
    return std::ranges::any_of(m_windows.windows(), [](const std::unique_ptr<PrefabWindowDocument>& window) {
        return window->document.dirty();
    });
}

bool PrefabWindowController::recoveryAvailable() const {
    return !m_recovery.available().empty();
}

bool PrefabWindowController::discardRecovery(std::string& error) {
    return m_recovery.discard(error);
}
