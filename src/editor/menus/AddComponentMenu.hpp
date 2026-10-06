#pragma once

// An Add Component button that opens the Add Component menu at the mouse through wm.call_menu, showing only category's
// types when one is given.

#include "editor/menus/MenuRegistry.hpp"
#include "engine/scene/Scene.hpp"

#include <optional>
#include <string_view>
#include <vector>

struct EditorContext;

// An Add Component button that opens the Add Component menu at the mouse
// through wm.call_menu, showing only category's types when one is given. The
// menu adds to the active object, so the button is disabled unless entity is
// the active object.
void drawAddComponentButton(EditorContext& context, SceneEntityId entity,
                            std::optional<std::string_view> category = std::nullopt);

// The Add Component menu's dynamic section: every non-internal component type
// of the document's registry (so a game module's types too) that the active
// object does not have, under its category label, running
// object.component_add.
void addComponentEntries(const OperatorContext& context, std::vector<MenuEntry>& entries);

// The Add menu's dynamic section: Component > category > type for every
// non-internal component type, running object.add_empty_with.
void addEmptyWithComponentEntries(const OperatorContext& context, std::vector<MenuEntry>& entries);
