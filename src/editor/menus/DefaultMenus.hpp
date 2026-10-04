#pragma once

#include "editor/menus/MenuRegistry.hpp"

#include <filesystem>
#include <functional>
#include <vector>

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
// (OBJECT_MT_add_component, grouped by category), plus the pies
// (registerPieMenus).
MenuResult registerDefaultMenus(MenuRegistry& registry, DefaultMenuSources sources = {});

// Registers the viewport pies with editorMenuOwner, for wm.call_pie: Shading
// (VIEW3D_MT_shading_pie: view3d.shading_set {type}), View
// (VIEW3D_MT_view_pie: view3d.view_axis {type}, view3d.view_camera,
// view3d.frame_selected), and Pivot Point (VIEW3D_MT_pivot_pie:
// transform.pivot_set {pivot}).
MenuResult registerPieMenus(MenuRegistry& registry);

// Appends one prefab.instantiate entry per prefab under the "Prefab" category,
// labelled with its project-relative path without the extension and sorted.
// prefabs are absolute paths; those outside the context's asset root are
// skipped.
void addPrefabEntries(const OperatorContext& context, const std::vector<std::filesystem::path>& prefabs,
                      std::vector<MenuEntry>& entries);
