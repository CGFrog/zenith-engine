// EditorSession's document workflow: native and typed-path file dialogs, the
// unsaved-changes prompt, and the actions (New, Open, Close, leaving for the
// Project Hub) that wait on it.

#include "EditorSession.hpp"

#include "editor/document/DefaultScene.hpp"
#include "editor/theme/EditorWidgets.hpp"

#include <imgui.h>

#include <algorithm>
#include <utility>

namespace {
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

void EditorSession::requestSaveAs() {
    requestFileDialog(FileDialogKind::Save);
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
    const SceneDocumentResult result = m_document.saveAs(path, context.scene, context.assets);
    if (!result) {
        m_diagnostics.reportError(result.error);
        return false;
    }
    reportSaveWarnings(result);
    m_history.crossBoundary(CommandHistoryBoundary::SaveDocument);
    onDocumentPathCommitted();
    return true;
}

void EditorSession::reportSaveWarnings(const SceneDocumentResult& result) {
    for (const std::string& warning : result.warnings) {
        m_diagnostics.add(EditorDiagnosticSeverity::Warning, "Editor", warning);
    }
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
    const SceneDocumentResult result = m_document.save(context.scene, context.assets);
    if (!result) {
        m_diagnostics.reportError("Save failed: " + result.error);
        return false;
    }
    reportSaveWarnings(result);
    m_history.crossBoundary(CommandHistoryBoundary::SaveDocument);
    std::string recoveryError;
    if (!m_recovery.discard(recoveryError)) {
        m_diagnostics.reportError(recoveryError);
    }
    m_diagnostics.acknowledgeErrors();
    return true;
}
