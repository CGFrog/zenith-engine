#include "FolderPicker.hpp"

#include "editor/project/ProjectFileOperations.hpp"
#include "editor/theme/EditorWidgets.hpp"

#include <imgui.h>

#include <utility>

FolderPicker::FolderPicker()
    : m_dialog(std::vector<FileDialogFilter>{}) {}

bool FolderPicker::draw(const char* id, std::string& path) {
    bool changed = false;
    if (const std::optional<FileDialogOutcome> outcome = m_dialog.poll()) {
        changed = apply(*outcome, path);
    }
    ImGui::PushID(id);
    ImGui::BeginDisabled(m_dialog.pending());
    if (widgets::Button("Browse...")) {
        m_error.clear();
        m_dialog.request(FileDialogKind::Folder, dialogStartFolder(pathFromUtf8Text(path)));
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Choose a folder");
    if (!m_error.empty()) {
        widgets::CopyableText(m_error, palette::error);
    }
    ImGui::PopID();
    return changed;
}

bool FolderPicker::apply(const FileDialogOutcome& outcome, std::string& path) {
    if (outcome.status == FileDialogStatus::Failed) {
        m_error = "Could not show the folder dialog: " + outcome.error;
        return false;
    }
    if (outcome.status != FileDialogStatus::Chosen) {
        return false;
    }
    std::string chosen = utf8PathText(outcome.path);
    if (chosen == path) {
        return false;
    }
    path = std::move(chosen);
    return true;
}

const std::string& FolderPicker::error() const {
    return m_error;
}

std::filesystem::path relativeIfInside(const std::filesystem::path& path, const std::filesystem::path& base) {
    const std::filesystem::path relative = path.lexically_relative(base);
    // lexically_relative is empty across roots and starts with ".." outside base.
    if (relative.empty() || *relative.begin() == "..") {
        return path;
    }
    return relative;
}
