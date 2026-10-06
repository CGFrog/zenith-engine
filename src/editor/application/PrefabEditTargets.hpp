#pragma once

// What the prefab windows do with a prefab open as a document of its own (PrefabEditTarget): saving it, and following
// its file when project files move or go.

#include "editor/application/DocumentBinding.hpp"
#include "editor/document/PrefabEditDocument.hpp"
#include "editor/project/AssetRelocation.hpp"

#include <filesystem>
#include <span>

class AssetManager;
class EditorDiagnostics;

// What the prefab windows do with a prefab open as a document of its own
// (PrefabEditTarget): saving it, and following its file when project files
// move or go.

// Writes the prefab document's root back to target's file
// (savePrefabDocument): links that saving gives entities added since opening
// are recorded as one step "Save <name>" on the document's history, the
// document and history become clean. saved receives this save's before and
// after content.
PrefabResult saveOpenPrefab(DocumentBinding document, PrefabEditTarget& target, AssetManager& assets,
                            EditorDiagnostics& diagnostics, const PrefabWriteHooks& hooks, SavedPrefabDocument& saved);

// Follows target's file when moves names it: the file and the
// project-relative reference change, reported in Info. assetRoot is the
// project's.
void followMovedPrefab(PrefabEditTarget& target, const std::filesystem::path& assetRoot,
                       std::span<const ProjectFileMove> moves, EditorDiagnostics& diagnostics);

// Warns when target's file is among removed; the document stays open and
// saving it writes the file again.
void warnRemovedPrefab(const PrefabEditTarget& target, std::span<const std::filesystem::path> removed,
                       EditorDiagnostics& diagnostics);
