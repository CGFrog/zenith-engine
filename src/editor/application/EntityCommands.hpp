#pragma once

#include "editor/application/SceneEditing.hpp"
#include "editor/interaction/Selection.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Undoable entity operations shared by panels, menus, and the object keymap.
// They need no window or GPU, so tests can drive them directly.
namespace EntityCommands {
// Creates an entity named "Entity", parented when parent is valid, and
// returns its id; the id is empty when the command failed.
SceneEntityId createEntity(const SceneEditing& editing, Entity parent);
// Destroys each listed entity that still exists as one undo step. Deleting a
// parent also deletes its children.
void deleteEntities(const SceneEditing& editing, std::span<const SceneEntityId> entities, std::string commandName);
// Deletes the selection and clears it.
void deleteSelection(const SceneEditing& editing, Selection& selection);
// Pastes a copy of the selection beside the originals (pasteBesideSources:
// each copy keeps its original's parent and transform) and selects the copy
// without recording undo. Returns false, after reporting why, when nothing
// was duplicated. It creates entities and adds them to the originals'
// parents, so the snapshot to record it from lists the selection (an Undo
// operator declares selectionScope).
bool duplicateSelectionUnrecorded(const SceneEditing& editing, Selection& selection);
// As duplicateSelectionUnrecorded; before receives the snapshot taken first,
// which records the duplicate (and a later move of the copies) when
// committed.
bool duplicateSelectionUncommitted(const SceneEditing& editing, Selection& selection,
                                   SceneEditing::EntitySnapshot& before);
void duplicateSelection(const SceneEditing& editing, Selection& selection);
// How a copy pasted beside its source is placed under the source's parent.
enum class PastedTransform : uint8_t {
    // The source's local transform: a duplicate lands on its original.
    KeepSourceLocal,
    // The world transform the fragment gave the copy (SceneFragment::replicate).
    KeepWorld,
};

// Pastes fragment (SceneFragment::copy of sources, in its roots order) so
// each copy of a root joins that root's parent, placed as transform says;
// a copy of a root without a parent (or whose parent cannot take it) stays
// at the top level on its fragment world transform. A fragment replicated N
// times (SceneFragment::replicate) gives N copies of each root, cycling
// through sources. pasted receives the copied roots.
SceneDocumentResult pasteBesideSources(const SceneEditing& editing, std::string_view fragment,
                                       std::span<const SceneEntityId> sources, PastedTransform transform,
                                       std::vector<SceneEntityId>& pasted);
} // namespace EntityCommands
