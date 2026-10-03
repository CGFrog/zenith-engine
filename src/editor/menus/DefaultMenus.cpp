#include "DefaultMenus.hpp"

#include "editor/menus/AddComponentMenu.hpp"
#include "editor/menus/EditorMenus.hpp"

#include <utility>
#include <vector>

namespace {
MenuEntry addEntry(std::string label, std::string operatorId, EditorIcon icon, std::vector<std::string> category = {}) {
    return {.label = std::move(label),
            .operatorId = std::move(operatorId),
            .properties = {},
            .submenu = {},
            .icon = icon,
            .category = std::move(category)};
}
} // namespace

MenuResult registerDefaultMenus(MenuRegistry& registry) {
    if (MenuResult result = registry.addMenu(editorMenuOwner, {.id = addMenuId, .title = "Add"}); !result) {
        return result;
    }
    if (MenuResult result = registry.addMenu(
            editorMenuOwner, {.id = addComponentMenuId, .title = "Add Component", .layout = MenuLayout::Sections});
        !result) {
        return result;
    }
    std::vector<MenuEntry> objects;
    objects.push_back(addEntry("Cube", "object.add_cube", EditorIcon::Mesh, {"Mesh"}));
    objects.push_back(addEntry("Sun", "object.add_sun", EditorIcon::Sun, {"Light"}));
    objects.push_back(addEntry("Environment", "object.add_environment", EditorIcon::Environment));
    objects.push_back(addEntry("Fog", "object.add_fog", EditorIcon::Fog));
    objects.push_back(addEntry("Empty", "object.add_empty", EditorIcon::Empty));
    for (MenuEntry& entry : objects) {
        if (MenuResult result = registry.addEntry(editorMenuOwner, addMenuId, std::move(entry)); !result) {
            return result;
        }
    }
    if (MenuResult result = registry.addSection(editorMenuOwner, addMenuId, addEmptyWithComponentEntries); !result) {
        return result;
    }
    return registry.addSection(editorMenuOwner, addComponentMenuId, addComponentEntries);
}
