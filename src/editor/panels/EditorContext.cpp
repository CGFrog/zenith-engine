#include "EditorContext.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/application/EntityCommands.hpp"

void EditorContext::placeModel(const std::filesystem::path& path, Entity parent) {
    if (!editMode()) {
        diagnostics.reportError("Assets can only be placed in Edit mode");
        return;
    }
    EntityCommands::placeModel(editing, selection, path, parent);
}
