#pragma once

#include "editor/application/DocumentBinding.hpp"
#include "editor/document/CommandHistory.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/document/PrefabEditDocument.hpp"
#include "editor/interaction/Selection.hpp"
#include "editor/project/AssetRelocation.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/graphics/RenderIds.hpp"
#include "engine/scene/Prefab.hpp"
#include "engine/scene/Scene.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

class AssetManager;
class EditorDiagnostics;

// A prefab open in a window of its own beside the level: the prefab
// instantiated into a scene of its own (openPrefabDocument), with its own
// document state, undo history, and selection, so edits there never enter the
// level's history and the level stays open and editable underneath.
struct PrefabWindowDocument {
    PrefabWindowDocument(uint64_t id, EditorDocument document)
        : id(id)
        , document(std::move(document)) {}

    DocumentBinding binding() {
        return {scene, document, history, selection};
    }

    // Unique within the session and never reused, so a window's ImGui ids
    // stay its own.
    uint64_t id;
    PrefabEditTarget target;
    Scene scene;
    EditorDocument document;
    CommandHistory history;
    Selection selection;
    // Keeps the assets the scene uses loaded: only the engine's own scene is
    // leased by the application, and nothing else holds a prefab window's.
    // The session renews it every frame.
    AssetLeaseSet assetLeases;
    // The render scene the window's viewport draws the scene as
    // (RenderRuntime::createScene); the session creates and destroys it.
    RenderSceneId renderScene;
};

// The prefabs open in windows of their own (the editor's prefab windows), at
// most one window per prefab. Windows are held by address, so a binding or a
// panel context built from one stays valid while others open; closing one
// destroys it, so the session closes windows between frames, never while
// one draws.
//
// Each document copies the level document's options when it opens, so it
// reads the game module's component types and holds module component storage
// in its scene: the module must not reload while a window is open, and every
// window must close before the module unloads.
class PrefabWindowSet {
  public:
    bool empty() const {
        return m_windows.empty();
    }

    size_t size() const {
        return m_windows.size();
    }

    // In the order they opened.
    std::span<const std::unique_ptr<PrefabWindowDocument>> windows() const {
        return m_windows;
    }

    // The window editing reference (project-relative), or null.
    PrefabWindowDocument* find(const std::filesystem::path& reference) const;
    PrefabWindowDocument* find(uint64_t id) const;
    // The window whose document is document, or null (the level's, for
    // example).
    PrefabWindowDocument* owner(const EditorDocument& document) const;

    // Opens the prefab at reference (project-relative) in a new window whose
    // document reads with level's options, and points window at it. A prefab
    // already open points window at its window and opens nothing. Fails,
    // changing nothing, when the prefab cannot be opened; the result's
    // diagnostics list instantiation problems.
    PrefabResult open(const std::filesystem::path& reference, const EditorDocument& level, AssetManager& assets,
                      PrefabWindowDocument*& window);
    // As open, with the prefab's scene already built (an autosave being
    // recovered): prefabScene's content becomes the new window's scene, and
    // the document starts with unsaved changes.
    PrefabWindowDocument& adopt(Scene& prefabScene, PrefabEditTarget target, const EditorDocument& level);

    // Drops the window with its unsaved changes; false when there is none.
    bool close(uint64_t id);
    // Drops every window with its unsaved changes, for the session ending or
    // the game module unloading.
    void closeAll();

    // Writes window's prefab to its file (saveOpenPrefab); saved receives the
    // file's content before and after.
    PrefabResult save(PrefabWindowDocument& window, AssetManager& assets, EditorDiagnostics& diagnostics,
                      const PrefabWriteHooks& hooks, SavedPrefabDocument& saved);

    // A prefab file changed (saved from a window, or outside the editor):
    // refreshes its instances in every window but the one editing that prefab
    // (whose root is the prefab itself, not an instance), each as one step
    // stepName on its own history (refreshPrefabInstances). refreshed
    // receives how many instances.
    PrefabResult refreshInstances(AssetManager& assets, EditorDiagnostics& diagnostics,
                                  const std::filesystem::path& reference, const PrefabDocument* previous,
                                  const std::string& stepName, size_t& refreshed);

    // Follows moved project files: each window's prefab file and reference,
    // and the asset references in each window's scene (a window whose
    // references moved gets unsaved changes). assetRoot is the project's.
    void filesMoved(std::span<const ProjectFileMove> moves, const std::filesystem::path& assetRoot,
                    AssetManager& assets, EditorDiagnostics& diagnostics);
    // Removed project files: a window whose prefab file went keeps its
    // document (saving writes the file again), with a warning.
    void filesRemoved(std::span<const std::filesystem::path> removed, EditorDiagnostics& diagnostics);

  private:
    PrefabWindowDocument& add(Scene& prefabScene, PrefabEditTarget target, const EditorDocument& level);

    std::vector<std::unique_ptr<PrefabWindowDocument>> m_windows;
    uint64_t m_nextId{1};
};
