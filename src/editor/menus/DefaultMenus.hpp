#pragma once

#include "editor/menus/MenuRegistry.hpp"

#include <filesystem>
#include <functional>
#include <vector>

class PrefabDisplayNames;

// Project content the default menus list. Unset sources list nothing.
struct DefaultMenuSources {
    // Absolute paths of the project's prefab files (the asset catalog's
    // prefabs), for Add > Prefab. Paths outside the document's asset root
    // are not listed.
    std::function<std::vector<std::filesystem::path>()> projectPrefabs;
};

// Registers the editor's own menus with editorMenuOwner: the Add menu
// (VIEW3D_MT_add: the add-object entries, then Prefab > one entry per project
// prefab running prefab.instantiate, then Component > category > type
// creating an empty with that component) and the Add Component menu
// (OBJECT_MT_add_component, grouped by category).
MenuResult registerDefaultMenus(MenuRegistry& registry, DefaultMenuSources sources = {});

// Appends one prefab.instantiate entry per prefab under the "Prefab" category,
// sorted by project-relative path and labelled with the path's folder and the
// prefab's display name (read through names; the file name without the
// extension when names is null). prefabs are absolute paths; those outside
// the context's asset root are skipped.
void addPrefabEntries(const OperatorContext& context, const std::vector<std::filesystem::path>& prefabs,
                      std::vector<MenuEntry>& entries, PrefabDisplayNames* names = nullptr);
