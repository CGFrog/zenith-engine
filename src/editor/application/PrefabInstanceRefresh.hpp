#pragma once

#include "engine/scene/Prefab.hpp"

#include <cstddef>
#include <filesystem>
#include <string>

class SceneEditing;

// Updates every instance of reference in editing's scene to the prefab file's
// current content (PrefabAuthoring::refreshInstances, keeping each instance's
// own overrides against previous, or its persisted ones when previous is
// null) and records the change as one undo step named stepName, so later
// steps' diffs stay consistent with the scene. Nothing is recorded when the
// scene has no instance of the prefab or the refresh changed nothing.
//
// editing must edit a document's scene, not Play's runtime scene (whose
// changes are never recorded): to refresh Play's parked edit scene or a
// prefab window's scene, build a SceneEditing over that scene and its own
// history. refreshed, when given, receives how many
// instances were refreshed.
PrefabResult refreshPrefabInstances(const SceneEditing& editing, const std::filesystem::path& reference,
                                    const PrefabDocument* previous, std::string stepName, size_t* refreshed = nullptr);

// The undo step name for a refresh after an outside change:
// "Refresh <displayName> instances (changed on disk)".
std::string externalPrefabRefreshStepName(const std::string& displayName);
