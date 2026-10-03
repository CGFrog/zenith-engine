#include "DefaultMenus.hpp"

#include "editor/document/EditorDocument.hpp"
#include "editor/menus/AddComponentMenu.hpp"
#include "editor/menus/EditorMenus.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
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

void addPrefabEntries(const OperatorContext& context, const std::vector<std::filesystem::path>& prefabs,
                      std::vector<MenuEntry>& entries) {
    const std::filesystem::path root =
        std::filesystem::absolute(context.editing.document().options().assetRoot).lexically_normal();
    std::vector<std::filesystem::path> references;
    for (const std::filesystem::path& prefab : prefabs) {
        const std::filesystem::path relative = prefab.lexically_normal().lexically_relative(root);
        if (!relative.empty() && *relative.begin() != "..") {
            references.push_back(relative);
        }
    }
    std::ranges::sort(references);
    for (const std::filesystem::path& reference : references) {
        std::filesystem::path label = reference;
        label.replace_extension();
        entries.push_back({.label = label.generic_string(),
                           .operatorId = "prefab.instantiate",
                           .properties = nlohmann::json{{"asset", reference.generic_string()}}.dump(),
                           .submenu = {},
                           .icon = EditorIcon::Components,
                           .category = {"Prefab"}});
    }
}

MenuResult registerDefaultMenus(MenuRegistry& registry, DefaultMenuSources sources) {
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
    if (sources.projectPrefabs) {
        MenuSectionFill prefabs = [list = std::move(sources.projectPrefabs)](const OperatorContext& context,
                                                                             std::vector<MenuEntry>& entries) {
            addPrefabEntries(context, list(), entries);
        };
        if (MenuResult result = registry.addSection(editorMenuOwner, addMenuId, std::move(prefabs)); !result) {
            return result;
        }
    }
    if (MenuResult result = registry.addSection(editorMenuOwner, addMenuId, addEmptyWithComponentEntries); !result) {
        return result;
    }
    return registry.addSection(editorMenuOwner, addComponentMenuId, addComponentEntries);
}
