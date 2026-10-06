#include "PrefabEditTargets.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/application/SceneEditing.hpp"
#include "editor/document/CommandHistory.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/project/AssetCatalog.hpp"
#include "engine/assets/Assets.hpp"

#include <optional>
#include <utility>

namespace {
std::filesystem::path normalReference(const std::filesystem::path& reference) {
    return std::filesystem::path(reference.lexically_normal().generic_string());
}
} // namespace

PrefabResult saveOpenPrefab(DocumentBinding document, PrefabEditTarget& target, AssetManager& assets,
                            EditorDiagnostics& diagnostics, const PrefabWriteHooks& hooks, SavedPrefabDocument& saved) {
    const SceneEditing editing(document.scene, assets, document.document, document.history, diagnostics);
    std::string before;
    if (!editing.capture(before)) {
        return {.error = "Could not record the save of " + target.displayName};
    }
    PrefabSystem prefabs(assets, document.document.options());
    PrefabResult result = savePrefabDocument(document.scene, target.root, target.reference, prefabs, saved, hooks);
    if (!result) {
        return result;
    }
    // Entities added since opening now carry the ids the file gave them.
    std::string after;
    if (editing.capture(after)) {
        editing.commit("Save " + target.displayName, {}, before, after);
    }
    document.history.crossBoundary(CommandHistoryBoundary::SaveDocument);
    document.document.markSaved();
    return result;
}

void followMovedPrefab(PrefabEditTarget& target, const std::filesystem::path& assetRoot,
                       std::span<const ProjectFileMove> moves, EditorDiagnostics& diagnostics) {
    std::optional<std::filesystem::path> moved = relocatedProjectPath(normalizedCatalogPath(target.file), moves);
    if (!moved) {
        return;
    }
    std::error_code error;
    const std::filesystem::path root = std::filesystem::absolute(assetRoot, error).lexically_normal();
    target.file = std::move(*moved);
    target.reference = normalReference(target.file.lexically_relative(root));
    diagnostics.add(EditorDiagnosticSeverity::Info, "Prefabs",
                    "The open prefab " + target.displayName + " now lives at '" + target.reference.generic_string() +
                        "'.");
}

void warnRemovedPrefab(const PrefabEditTarget& target, std::span<const std::filesystem::path> removed,
                       EditorDiagnostics& diagnostics) {
    if (isRemovedProjectPath(normalizedCatalogPath(target.file), removed)) {
        diagnostics.add(EditorDiagnosticSeverity::Warning, "Prefabs",
                        "The open prefab " + target.displayName +
                            "'s file was removed; save it to write the file again.");
    }
}
