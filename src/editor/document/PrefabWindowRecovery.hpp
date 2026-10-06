#pragma once

// Autosaves of the prefab windows with unsaved changes, and recovering them.

#include "editor/document/EditorRecovery.hpp"
#include "engine/scene/SceneDocument.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

class AssetManager;
class Scene;

// One prefab window's unsaved document, as its autosave needs it.
struct PrefabWindowAutosave {
    const Scene& scene;
    const SceneDocumentOptions& options;
    PrefabEditRecoveryNote note;
};

// The autosaves of the prefab windows with unsaved changes: one
// PrefabEditRecovery per window in directory (for example
// <settings>/recovery-prefab-windows), named 1.prefab-edit, 2.prefab-edit, ...
// Each autosave replaces the whole set, so a window that was saved or closed
// since leaves nothing behind.
class PrefabWindowRecovery {
  public:
    explicit PrefabWindowRecovery(std::filesystem::path directory);

    // Writes windows (removing the previous set first); an empty span only
    // removes. Best effort: every window is attempted, and the first error is
    // returned.
    SceneDocumentResult autosave(std::span<const PrefabWindowAutosave> windows, const AssetManager& assets) const;
    // The autosaved windows found on disk, in order.
    std::vector<PrefabEditRecovery> available() const;
    // Removes every autosaved window.
    bool discard(std::string& error) const;

  private:
    std::filesystem::path m_directory;
};
