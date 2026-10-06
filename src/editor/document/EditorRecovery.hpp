#pragma once

#include "engine/scene/SceneDocument.hpp"

#include <filesystem>
#include <string>

class AssetManager;

class EditorRecovery {
  public:
    explicit EditorRecovery(std::filesystem::path path);

    SceneDocumentResult autosave(const Scene& scene, const AssetManager& assets,
                                 const SceneDocumentOptions& options) const;
    SceneDocumentResult recover(Scene& scene, AssetManager& assets, const SceneDocumentOptions& options) const;
    bool available() const;
    bool discard(std::string& error) const;
    const std::filesystem::path& path() const;

  private:
    std::filesystem::path m_path;
};

// What a prefab edit's autosave needs besides its scene: which prefab the
// scene is, and its root there.
struct PrefabEditRecoveryNote {
    // Project-relative.
    std::filesystem::path reference;
    SceneEntityId root;
    std::string displayName;
};

// The autosave of a prefab open as its own document (a prefab window): the
// edited scene as a scene document at path (for example
// <settings>/recovery-prefab-windows/1.prefab-edit), and a JSON note beside it
// (path with ".json" appended) naming the prefab, its root, and its display
// name. Kept apart from the scene's EditorRecovery
// (document/PrefabWindowRecovery keeps one per window).
class PrefabEditRecovery {
  public:
    explicit PrefabEditRecovery(std::filesystem::path path);

    SceneDocumentResult autosave(const Scene& scene, const AssetManager& assets, const SceneDocumentOptions& options,
                                 const PrefabEditRecoveryNote& note) const;
    // Reads the scene into an empty scene and the note; fails when either is
    // missing or unreadable.
    SceneDocumentResult recover(Scene& scene, AssetManager& assets, const SceneDocumentOptions& options,
                                PrefabEditRecoveryNote& note) const;
    bool available() const;
    // Removes both files.
    bool discard(std::string& error) const;

  private:
    std::filesystem::path notePath() const;

    EditorRecovery m_scene;
};
