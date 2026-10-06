// EditorSession's prefab mode: a prefab opened as a document of its own over
// the parked scene (DocumentStack), saving it, going back, refreshing parked
// documents when a prefab file changes on disk, and the breadcrumbs that show
// where the user is. The unsaved-changes prompts that leaving waits on are in
// EditorFileDialogs.cpp; autosave and recovery in EditorRecovery.cpp.

#include "EditorSession.hpp"

#include "editor/application/PrefabInstanceRefresh.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/EditorWidgets.hpp"
#include "editor/viewport/ViewFraming.hpp"

#include <imgui.h>

#include <utility>
#include <vector>

namespace {
constexpr const char* prefabDiagnosticSource = "Prefabs";

std::string sameReference(const std::filesystem::path& reference) {
    return reference.lexically_normal().generic_string();
}
} // namespace

bool EditorSession::prefabEditActive() const {
    return !m_documents.empty();
}

bool EditorSession::prefabCloseAvailable() const {
    return !m_documents.empty() || focusedPrefabWindow() != nullptr;
}

bool EditorSession::prefabEditAvailable(const OperatorContext& context) const {
    if (prefabWindowsEnabled()) {
        // A window opens beside the level, so the level may be playing.
        return !m_fileDialog.pending() && !m_activeTransform;
    }
    return documentCommandsAvailable(context);
}

std::optional<OpenPrefabSummary> EditorSession::activePrefabSummary() const {
    const PrefabEditTarget* prefab = m_documents.activePrefab();
    if (prefab == nullptr) {
        return std::nullopt;
    }
    return OpenPrefabSummary{.displayName = prefab->displayName, .root = prefab->root};
}

DocumentBinding EditorSession::activeDocument(ApplicationContext& context) {
    return {.scene = context.scene, .document = m_document, .history = m_history, .selection = m_selection};
}

OperatorResult EditorSession::editPrefab(ApplicationContext& context, const std::filesystem::path& reference) {
    if (prefabWindowsEnabled()) {
        return openPrefabWindow(context, reference);
    }
    if (m_playMode.mode() != EditorMode::Edit) {
        return OperatorResult::failed("Stop Play before editing a prefab");
    }
    // Physics bodies and audio voices belong to the scene being parked.
    context.physics.clear();
    context.audio.stopSceneVoices();
    const PrefabResult opened = m_documents.push(activeDocument(context), context.assets, reference);
    for (const std::string& diagnostic : opened.diagnostics) {
        m_diagnostics.add(EditorDiagnosticSeverity::Warning, prefabDiagnosticSource, diagnostic);
    }
    if (!opened) {
        return OperatorResult::failed("Could not edit the prefab: " + opened.error);
    }
    // Adjust Last Operation and the eyedropper refer to the parked document.
    m_operatorRunner.forgetLast();
    m_eyedropper.cancel();
    ParkedDocument& parked = *m_documents.top();
    std::vector<SceneEntityId> everything;
    for (const Entity entity : context.scene.registry().view<const TransformComponent>()) {
        everything.push_back(context.scene.id(entity));
    }
    for (ViewportPanel* viewport : levelViewports()) {
        parked.views.emplace_back(viewport->id(), viewport->takeDocumentView());
        frameEntities(viewport->camera(), context.scene, context.assets, everything);
    }
    notePrefabFilesRead(context.scene, m_document.options().assetRoot, prefabWriteHooks());
    m_autosaveElapsed = 0.0f;
    enterPrefabWorkspace();
    return OperatorResult::finished();
}

void EditorSession::leavePrefabEdit(ApplicationContext& context) {
    if (m_documents.empty()) {
        return;
    }
    context.physics.clear();
    context.audio.stopSceneVoices();
    DocumentStackPopResult popped =
        m_documents.pop(activeDocument(context), context.assets, m_diagnostics, prefabWriteHooks());
    for (const std::string& diagnostic : popped.refresh.diagnostics) {
        m_diagnostics.add(EditorDiagnosticSeverity::Warning, prefabDiagnosticSource, diagnostic);
    }
    if (!popped.refresh) {
        m_diagnostics.reportError(popped.refresh.error);
    }
    m_operatorRunner.forgetLast();
    m_eyedropper.cancel();
    for (auto& [id, view] : popped.views) {
        if (ViewportPanel* viewport = m_viewports.find(id)) {
            viewport->restoreDocumentView(std::move(view));
        }
    }
    m_selection.removeMissing(context.scene);
    // The autosave named the prefab just left; the next one writes whatever
    // is open then.
    std::string recoveryError;
    if (!m_prefabRecovery.discard(recoveryError)) {
        m_diagnostics.reportError(recoveryError);
    }
    m_autosaveElapsed = 0.0f;
    if (m_documents.empty()) {
        leavePrefabWorkspace();
    }
}

bool EditorSession::savePrefabEdit(ApplicationContext& context) {
    const PrefabResult saved =
        m_documents.save(activeDocument(context), context.assets, m_diagnostics, prefabWriteHooks());
    for (const std::string& diagnostic : saved.diagnostics) {
        m_diagnostics.add(EditorDiagnosticSeverity::Warning, prefabDiagnosticSource, diagnostic);
    }
    if (!saved) {
        m_diagnostics.reportError("Save failed: " + saved.error);
        return false;
    }
    std::string recoveryError;
    if (!m_prefabRecovery.discard(recoveryError)) {
        m_diagnostics.reportError(recoveryError);
    }
    m_diagnostics.acknowledgeErrors();
    return true;
}

std::string EditorSession::activeDocumentName() const {
    const PrefabEditTarget* prefab = m_documents.activePrefab();
    if (prefab == nullptr) {
        return m_document.displayName();
    }
    return prefab->displayName + (m_document.dirty() ? "*" : "");
}

void EditorSession::reconcileParkedDocuments(ApplicationContext& context, const PrefabFileChange& change) {
    if (m_documents.empty()) {
        return;
    }
    const PrefabEditTarget* open = m_documents.activePrefab();
    if (sameReference(open->reference) == sameReference(change.reference)) {
        m_diagnostics.add(EditorDiagnosticSeverity::Warning, prefabDiagnosticSource,
                          change.displayName +
                              " changed on disk while it is open here. Save writes this version over that change; "
                              "Back without saving keeps the file's.");
    }
    size_t refreshed = 0;
    const PrefabResult result = m_documents.refreshParked(context.assets, m_diagnostics, change.reference,
                                                          change.previous ? &*change.previous : nullptr,
                                                          externalPrefabRefreshStepName(change.displayName), refreshed);
    for (const std::string& diagnostic : result.diagnostics) {
        m_diagnostics.add(EditorDiagnosticSeverity::Warning, prefabDiagnosticSource, diagnostic);
    }
    if (!result) {
        m_diagnostics.reportError("Could not refresh " + change.displayName + " instances: " + result.error);
    } else if (refreshed > 0) {
        m_diagnostics.add(EditorDiagnosticSeverity::Info, prefabDiagnosticSource,
                          change.displayName + " changed on disk; refreshed " + std::to_string(refreshed) +
                              (refreshed == 1 ? " instance" : " instances") +
                              " in the documents under the open prefab; Undo there puts them back.");
    }
}

void EditorSession::drawPrefabBreadcrumbs(ApplicationContext& context) {
    const std::span<const std::unique_ptr<DocumentStack::Frame>> frames = m_documents.frames();
    if (frames.empty()) {
        return;
    }
    ImGui::Separator();
    widgets::CaptionText(frames.front()->parent.document.displayName().c_str(), palette::inkMuted);
    for (size_t index = 0; index < frames.size(); ++index) {
        const PrefabEditTarget& prefab = frames[index]->prefab;
        const bool active = index + 1 == frames.size();
        // A parked prefab's document waits in the frame above it.
        const bool dirty = active ? m_document.dirty() : frames[index + 1]->parent.document.dirty();
        const std::string label = prefab.displayName + (dirty ? "*" : "");
        ImGui::SameLine();
        widgets::CaptionText(">", palette::inkMuted);
        ImGui::SameLine();
        widgets::CaptionText(label.c_str(), active ? palette::prefabOverrideInk : palette::inkMuted);
        ImGui::SetItemTooltip("%s", prefab.reference.generic_string().c_str());
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!operatorAvailable(context, "file.save"));
    if (widgets::Button("Save##prefab_save")) {
        invokeOperator(context, "file.save", {}, KeymapRegion::Global);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Write the prefab to its file  %s", m_keymap.shortcutLabel("file.save", {}).c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(!operatorAvailable(context, "prefab.exit_edit"));
    if (widgets::Button("Back##prefab_back")) {
        invokeOperator(context, "prefab.exit_edit", {}, KeymapRegion::Global);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Return to the document under this prefab");
}
