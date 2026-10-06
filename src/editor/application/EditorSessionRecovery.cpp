// EditorSession's autosave, its emergency save after a crash, and the
// recovery prompt shown when the previous session left an autosave behind.
// Prefab windows with unsaved changes are saved beside the level
// (PrefabWindowController), and Recover opens them again.

#include "EditorSession.hpp"

#include "editor/theme/EditorWidgets.hpp"

#include <imgui.h>

#include <algorithm>
#include <memory>

void EditorSession::updateAutosave(ApplicationContext& context, float deltaSeconds) {
    // An interval of 0 turns autosave off (Edit > Preferences > Saving).
    const float intervalSeconds = m_settings.autosave().intervalSeconds;
    const bool levelUnsaved = m_document.dirty();
    const bool windowsUnsaved = m_prefabWindows.anyUnsaved();
    if (intervalSeconds <= 0.0f || !(levelUnsaved || windowsUnsaved) || m_playMode.mode() != EditorMode::Edit) {
        m_autosaveElapsed = 0.0f;
        return;
    }
    m_autosaveElapsed += deltaSeconds;
    if (m_autosaveElapsed >= intervalSeconds) {
        m_autosaveElapsed = 0.0f;
        if (windowsUnsaved) {
            m_prefabWindows.autosave(context, true);
        }
        if (!levelUnsaved) {
            return;
        }
        if (const SceneDocumentResult result = m_recovery.autosave(context.scene, context.assets, m_document.options());
            !result) {
            m_diagnostics.reportError("Autosave failed: " + result.error);
        }
    }
}

void EditorSession::registerCrashRecovery(ApplicationContext& context) {
    if (m_crashRecoveryRegistered || !m_crashRecoveryChanged) {
        return;
    }
    m_crashRecoveryChanged([this, &context] { saveRecoveryAfterCrash(context); });
    m_crashRecoveryRegistered = true;
}

void EditorSession::saveRecoveryAfterCrash(ApplicationContext& context) {
    m_prefabWindows.autosave(context, false);
    if (!m_document.dirty()) {
        return;
    }
    // Play and Simulate run on a copy; the document is the parked edit scene.
    const Scene* scene = m_playMode.mode() == EditorMode::Edit ? &context.scene : m_playMode.editScene();
    if (scene != nullptr) {
        m_recovery.autosave(*scene, context.assets, m_document.options());
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
    if (m_prefabWindows.recoveryAvailable()) {
        ImGui::TextWrapped("It includes unsaved prefab windows; Recover opens them again.");
    }
    if (widgets::Button("Recover")) {
        bool recovered = true;
        if (m_recovery.available()) {
            if (const SceneDocumentResult result =
                    m_recovery.recover(context.scene, context.assets, m_document.options());
                !result) {
                m_diagnostics.reportError("Recovery failed: " + result.error);
                recovered = false;
            } else {
                m_history.crossBoundary(CommandHistoryBoundary::OpenDocument);
                m_document.markDirty();
                notePrefabFilesRead(context.scene, m_document.options().assetRoot, prefabWriteHooks());
            }
        }
        if (recovered) {
            m_prefabWindows.recover(context);
        }
        if (recovered) {
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (widgets::Button("Discard Recovery")) {
        std::string error;
        if (!m_recovery.discard(error) || !m_prefabWindows.discardRecovery(error)) {
            m_diagnostics.reportError(error);
        } else {
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndPopup();
}
