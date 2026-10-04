// EditorSession's autosave and the recovery prompt shown when the previous
// session left an autosave behind.

#include "EditorSession.hpp"

#include "editor/theme/EditorWidgets.hpp"

#include <imgui.h>

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
