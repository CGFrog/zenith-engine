#pragma once

#include "editor/application/SceneEditing.hpp"
#include "editor/interaction/Selection.hpp"

#include <glm/vec3.hpp>

#include <filesystem>
#include <span>
#include <string>

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
// Pastes a copy of the selection beside the originals and selects the copy
// without recording undo; before receives the snapshot taken first. Returns
// false when nothing was duplicated.
bool duplicateSelectionUncommitted(const SceneEditing& editing, Selection& selection, std::string& before);
void duplicateSelection(const SceneEditing& editing, Selection& selection);
// Loads the model and instantiates it under a new entity named after the
// file, parented when parent is valid, with its origin at the world-space
// location (meters; the editor passes the 3D cursor), then selects it.
void placeModel(const SceneEditing& editing, Selection& selection, const std::filesystem::path& path, Entity parent,
                const glm::vec3& location);
} // namespace EntityCommands
