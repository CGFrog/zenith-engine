#pragma once

#include "editor/menus/MenuRegistry.hpp"

// Registers the editor's own menus with editorMenuOwner: the Add menu
// (VIEW3D_MT_add: the add-object entries, then Component > category > type
// creating an empty with that component) and the Add Component menu
// (OBJECT_MT_add_component, grouped by category).
MenuResult registerDefaultMenus(MenuRegistry& registry);
