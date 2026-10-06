// EditorSession's autosave, its emergency save after a crash, and the
// recovery prompt shown when the previous session left an autosave behind.
// In prefab mode the parked scene and the open prefab are saved apart
// (m_recovery and m_prefabRecovery), and Recover reopens the prefab edit.
// Prefab windows with unsaved changes are saved beside them
// (m_prefabWindowRecovery), and Recover opens them again.

#include "EditorSession.hpp"

#include "editor/theme/EditorWidgets.hpp"

#include <imgui.h>

#include <algorithm>
#include <memory>

void EditorSession::updateAutosave(ApplicationContext& context, float deltaSeconds) {
    // An interval of 0 turns autosave off (Edit > Preferences > Saving).
    const float intervalSeconds = m_settings.autosave().intervalSeconds;
    const ParkedDocument* parkedScene = m_documents.parkedScene();
    const bool levelUnsaved = m_document.dirty() || (parkedScene != nullptr && parkedScene->document.dirty());
    const bool windowsUnsaved =
        std::ranges::any_of(m_prefabWindows.windows(), [](const std::unique_ptr<PrefabWindowDocument>& window) {
            return window->document.dirty();
        });
    if (intervalSeconds <= 0.0f || !(levelUnsaved || windowsUnsaved) || m_playMode.mode() != EditorMode::Edit) {
        m_autosaveElapsed = 0.0f;
        return;
    }
    m_autosaveElapsed += deltaSeconds;
    if (m_autosaveElapsed >= intervalSeconds) {
        m_autosaveElapsed = 0.0f;
        if (windowsUnsaved) {
            autosavePrefabWindows(context, true);
        }
        if (!levelUnsaved) {
            return;
        }
        if (!m_documents.empty()) {
            if (const std::string error = autosavePrefabEdit(context); !error.empty()) {
                m_diagnostics.reportError("Autosave failed: " + error);
            }
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

std::string EditorSession::autosavePrefabEdit(ApplicationContext& context) {
    std::string error;
    if (const ParkedDocument* scene = m_documents.parkedScene(); scene != nullptr && scene->document.dirty()) {
        if (const SceneDocumentResult result =
                m_recovery.autosave(scene->scene, context.assets, scene->document.options());
            !result) {
            error = result.error;
        }
    }
    const PrefabEditTarget* prefab = m_documents.activePrefab();
    if (prefab != nullptr && m_document.dirty()) {
        if (const SceneDocumentResult result = m_prefabRecovery.autosave(
                context.scene, context.assets, m_document.options(),
                {.reference = prefab->reference, .root = prefab->root, .displayName = prefab->displayName});
            !result && error.empty()) {
            error = result.error;
        }
    }
    return error;
}

void EditorSession::saveRecoveryAfterCrash(ApplicationContext& context) {
    autosavePrefabWindows(context, false);
    if (!m_documents.empty()) {
        // Prefab mode never runs Play, so the active scene is the prefab.
        autosavePrefabEdit(context);
        return;
    }
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
    if (m_prefabRecovery.available()) {
        ImGui::TextWrapped("It includes an unsaved prefab edit; Recover opens the prefab again.");
    }
    if (!m_prefabWindowRecovery.available().empty()) {
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
        if (recovered && m_prefabRecovery.available()) {
            recoverPrefabEdit(context);
        }
        if (recovered) {
            recoverPrefabWindows(context);
        }
        if (recovered) {
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (widgets::Button("Discard Recovery")) {
        std::string error;
        if (!m_recovery.discard(error) || !m_prefabRecovery.discard(error) || !m_prefabWindowRecovery.discard(error)) {
            m_diagnostics.reportError(error);
        } else {
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndPopup();
}

void EditorSession::recoverPrefabEdit(ApplicationContext& context) {
    Scene recovered;
    PrefabEditRecoveryNote note;
    if (const SceneDocumentResult result =
            m_prefabRecovery.recover(recovered, context.assets, m_document.options(), note);
        !result) {
        m_diagnostics.reportError("Could not recover the prefab edit: " + result.error);
        return;
    }
    std::filesystem::path file;
    if (const PrefabResult resolved = PrefabSystem(context.assets, m_document.options()).resolve(note.reference, file);
        !resolved || recovered.findEntity(note.root) == entt::null) {
        m_diagnostics.reportError("Could not recover the prefab edit of " + note.reference.generic_string() + ": " +
                                  (resolved ? "its root is missing" : resolved.error));
        return;
    }
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(file, error);
    context.physics.clear();
    context.audio.stopSceneVoices();
    m_documents.pushRecovered(activeDocument(context), recovered,
                              {.reference = note.reference,
                               .file = (error ? file : absolute).lexically_normal(),
                               .displayName = note.displayName,
                               .root = note.root});
    m_document.markDirty();
    m_operatorRunner.forgetLast();
    ParkedDocument& parked = *m_documents.top();
    for (ViewportPanel* viewport : m_viewports.openViewports()) {
        parked.views.emplace_back(viewport->id(), viewport->takeDocumentView());
    }
    notePrefabFilesRead(context.scene, m_document.options().assetRoot, prefabWriteHooks());
}
