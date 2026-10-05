#pragma once

#include "editor/document/CommandHistory.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/document/PrefabEditDocument.hpp"
#include "editor/document/PrefabWriteCommand.hpp"
#include "editor/interaction/Selection.hpp"
#include "editor/panels/ViewportPanel.hpp"
#include "editor/project/AssetRelocation.hpp"
#include "engine/scene/Prefab.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

class AssetManager;
class EditorDiagnostics;

// The objects holding the document being edited: the session's own members.
// Operators, panels, and the undo keys refer to them by address, so switching
// documents exchanges their contents and never the objects themselves.
struct ActiveDocument {
    Scene& scene;
    EditorDocument& document;
    CommandHistory& history;
    Selection& selection;
};

// A document parked under a prefab edit: everything ActiveDocument holds,
// plus each viewport's view of it.
struct ParkedDocument {
    explicit ParkedDocument(EditorDocument document)
        : document(std::move(document)) {}

    Scene scene;
    EditorDocument document;
    CommandHistory history;
    Selection selection;
    std::vector<std::pair<ViewportId, ViewportPanel::DocumentView>> views;
};

// What the step pop records on the document it returns to: one
// "Edit <name>" PrefabWriteCommand (the file write and the refreshed
// instances), and a scene-only "Refresh <name> instances" step on each
// document parked further down.
struct DocumentStackPopResult {
    // The returned-to document's views, for the viewports to restore.
    std::vector<std::pair<ViewportId, ViewportPanel::DocumentView>> views;
    // Problems refreshing instances, as diagnostics; error when the step
    // could not be recorded.
    PrefabResult refresh{.success = true};
};

// The documents parked under prefab edits, from the scene up (the editor's
// prefab mode). The active document is always the session's ActiveDocument;
// each frame of the stack is one prefab edit: the prefab being (or, below the
// top, waiting to be) edited, and the document it was opened over. Opening a
// prefab from inside prefab mode (a nested instance) pushes another frame, and
// going back pops one, so the stack reads like the breadcrumbs:
// Level > room_07 > door_frame.
//
// Each document keeps its own undo history: edits in a prefab never enter the
// scene's history. A parked scene keeps any game module component storage it
// holds, so the module must not reload while the stack is not empty, and it
// must be unwound before the module unloads.
class DocumentStack {
  public:
    struct Frame {
        explicit Frame(EditorDocument document)
            : parent(std::move(document)) {}

        PrefabEditTarget prefab;
        // The document the prefab was opened over.
        ParkedDocument parent;
    };

    bool empty() const {
        return m_frames.empty();
    }

    size_t depth() const {
        return m_frames.size();
    }

    // The prefab being edited; null while a scene is.
    const PrefabEditTarget* activePrefab() const;

    // Bottom first: frames()[0]->parent is the scene document.
    std::span<const std::unique_ptr<Frame>> frames() const {
        return m_frames;
    }

    // The scene document while it is parked under prefab edits; null while it
    // is the active one.
    const ParkedDocument* parkedScene() const;
    ParkedDocument* parkedScene();
    // The document parked directly under the active prefab, or null.
    ParkedDocument* top();
    // Whether reference is open, active or parked.
    bool contains(const std::filesystem::path& reference) const;

    // Opens the prefab at reference (project-relative) over the active
    // document, which is parked: its scene, document state, history, and
    // selection move into a new frame (its views are the caller's to add,
    // through top()), and the active ones start empty, holding the prefab.
    // Fails, changing nothing, when the prefab is already open in the stack
    // or cannot be opened. The result's diagnostics list instantiation
    // problems.
    PrefabResult push(ActiveDocument active, AssetManager& assets, const std::filesystem::path& reference);
    // As push, with the prefab's scene already built (an autosave being
    // recovered): prefabScene's content becomes the active scene.
    void pushRecovered(ActiveDocument active, Scene& prefabScene, PrefabEditTarget target);

    // Writes the active prefab to its file (savePrefabDocument): links that
    // saving gives entities added since opening are recorded as a step
    // "Save <name>" on the active history, the history and document become
    // clean, and the content is kept for the step pop records.
    PrefabResult save(ActiveDocument active, AssetManager& assets, EditorDiagnostics& diagnostics,
                      const PrefabWriteHooks& hooks);

    // Leaves the active prefab, dropping its unsaved changes, and makes the
    // document under it active again. When the prefab was saved since it was
    // opened, refreshes the instances of it in that document and records one
    // undo step "Edit <name>" covering both the file (undo puts back what it
    // held before the first save) and the refreshed instances; the documents
    // parked further down refresh theirs as a step "Refresh <name> instances"
    // each. A file changed outside the editor since the last save gets only
    // the scene-only refresh. Does nothing when empty.
    DocumentStackPopResult pop(ActiveDocument active, AssetManager& assets, EditorDiagnostics& diagnostics,
                               const PrefabWriteHooks& hooks);
    // Drops every prefab edit with its unsaved changes, without recording
    // anything, and makes the scene document active again; returns its
    // views. For the session ending, before the game module unloads.
    std::vector<std::pair<ViewportId, ViewportPanel::DocumentView>> unwind(ActiveDocument active);

    // A prefab file changed outside the editor: refreshes its instances in
    // every parked document, each as one step stepName on its own history
    // (refreshPrefabInstances). refreshed receives how many instances.
    PrefabResult refreshParked(AssetManager& assets, EditorDiagnostics& diagnostics,
                               const std::filesystem::path& reference, const PrefabDocument* previous,
                               const std::string& stepName, size_t& refreshed);

    // Follows moved project files: an open prefab's file and reference, a
    // parked scene's path, and the asset references in parked scenes (the
    // active document is the caller's). assetRoot is the project's.
    void filesMoved(std::span<const ProjectFileMove> moves, AssetManager& assets, EditorDiagnostics& diagnostics);
    // Removed project files: a parked scene whose file went becomes untitled
    // with unsaved changes; an open prefab whose file went keeps its document
    // (saving writes the file again), with a warning.
    void filesRemoved(std::span<const std::filesystem::path> removed, EditorDiagnostics& diagnostics);

  private:
    std::vector<std::unique_ptr<Frame>> m_frames;
};
