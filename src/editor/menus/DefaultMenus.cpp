#include "DefaultMenus.hpp"

#include "editor/document/EditorDocument.hpp"
#include "editor/document/PrefabAuthoring.hpp"
#include "editor/menus/AddComponentMenu.hpp"
#include "editor/menus/EditorMenus.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <utility>
#include <vector>

namespace {
MenuEntry addEntry(std::string label, std::string operatorId, EditorIcon icon, std::vector<std::string> category = {},
                   std::string properties = {}) {
    return {.label = std::move(label),
            .operatorId = std::move(operatorId),
            .properties = std::move(properties),
            .submenu = {},
            .icon = icon,
            .category = std::move(category)};
}

// An Add entry creating an empty with one component (object.add_empty_with).
MenuEntry addWithComponent(std::string label, const char* component, EditorIcon icon,
                           std::vector<std::string> category) {
    return addEntry(std::move(label), "object.add_empty_with", icon, std::move(category),
                    nlohmann::json{{"component", component}}.dump());
}

// An Add > Physics entry creating a collider with a rigid body
// (object.add_collider).
MenuEntry colliderEntry(std::string label, const char* shape, const char* motion) {
    return addEntry(std::move(label), "object.add_collider", EditorIcon::Physics, {"Physics"},
                    nlohmann::json{{"shape", shape}, {"motion", motion}}.dump());
}
} // namespace

void addPrefabEntries(const OperatorContext& context, const std::vector<std::filesystem::path>& prefabs,
                      std::vector<MenuEntry>& entries, PrefabDisplayNames* names) {
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
        std::filesystem::path path = reference;
        path.replace_extension();
        // Display names are UTF-8, so they are joined as text, not as paths.
        std::string label = path.generic_string();
        if (names != nullptr) {
            const std::string folder = reference.parent_path().generic_string();
            label = (folder.empty() ? std::string() : folder + "/") + names->get(root / reference);
        }
        entries.push_back({.label = std::move(label),
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
    objects.push_back(addWithComponent("Point Light", "point_light", EditorIcon::PointLight, {"Light"}));
    objects.push_back(addWithComponent("Spot Light", "spot_light", EditorIcon::SpotLight, {"Light"}));
    objects.push_back(
        addWithComponent("Reflection Probe", "reflection_probe", EditorIcon::ReflectionProbe, {"Light Probe"}));
    objects.push_back(
        addWithComponent("Irradiance Volume", "irradiance_volume", EditorIcon::IrradianceVolume, {"Light Probe"}));
    objects.push_back(addEntry("Camera", "object.add_camera", EditorIcon::Camera));
    objects.push_back(addWithComponent("Audio Emitter", "audio_emitter", EditorIcon::AudioEmitter, {"Audio"}));
    objects.push_back(addWithComponent("Audio Listener", "audio_listener", EditorIcon::AudioListener, {"Audio"}));
    objects.push_back(addWithComponent("Decal", "decal", EditorIcon::Decal, {"Effects"}));
    objects.push_back(addWithComponent("Particle Emitter", "particle_emitter", EditorIcon::Particles, {"Effects"}));
    objects.push_back(addEntry("Environment", "object.add_environment", EditorIcon::Environment));
    objects.push_back(addEntry("Fog", "object.add_fog", EditorIcon::Fog));
    objects.push_back(addEntry("Empty", "object.add_empty", EditorIcon::Empty));
    // Physics: colliders come with a rigid body, since a collider alone is
    // not simulated; Rigid Body and Character Controller add just theirs, to
    // be combined with a collider in the Inspector.
    objects.push_back(colliderEntry("Static Box", "box", "static"));
    objects.push_back(colliderEntry("Static Sphere", "sphere", "static"));
    objects.push_back(colliderEntry("Static Capsule", "capsule", "static"));
    objects.push_back(colliderEntry("Dynamic Box", "box", "dynamic"));
    objects.push_back(colliderEntry("Dynamic Sphere", "sphere", "dynamic"));
    objects.push_back(colliderEntry("Dynamic Capsule", "capsule", "dynamic"));
    objects.push_back(addWithComponent("Rigid Body", "rigid_body", EditorIcon::Physics, {"Physics"}));
    // The Character is a prefab, written on first use (prefab.add_character),
    // so every character in a project shares one definition.
    objects.push_back(addEntry("Character", "prefab.add_character", EditorIcon::Prefab, {"Physics"}));
    objects.push_back(
        addWithComponent("Character Controller", "character_controller", EditorIcon::Physics, {"Physics"}));
    for (MenuEntry& entry : objects) {
        if (MenuResult result = registry.addEntry(editorMenuOwner, addMenuId, std::move(entry)); !result) {
            return result;
        }
    }
    if (sources.projectPrefabs) {
        MenuSectionFill prefabs = [list = std::move(sources.projectPrefabs),
                                   names = std::make_shared<PrefabDisplayNames>()](const OperatorContext& context,
                                                                                   std::vector<MenuEntry>& entries) {
            addPrefabEntries(context, list(), entries, names.get());
        };
        if (MenuResult result = registry.addSection(editorMenuOwner, addMenuId, std::move(prefabs)); !result) {
            return result;
        }
    }
    if (MenuResult result = registry.addSection(editorMenuOwner, addMenuId, addEmptyWithComponentEntries); !result) {
        return result;
    }
    if (MenuResult result = registry.addSection(editorMenuOwner, addComponentMenuId, addComponentEntries); !result) {
        return result;
    }
    if (MenuResult result = registerSelectGroupedMenu(registry); !result) {
        return result;
    }
    return registerPieMenus(registry);
}

MenuResult registerSelectGroupedMenu(MenuRegistry& registry) {
    if (MenuResult result = registry.addMenu(editorMenuOwner, {.id = selectGroupedMenuId, .title = "Select Grouped"});
        !result) {
        return result;
    }
    constexpr std::array<std::pair<const char*, const char*>, 8> types{{{"Children", "children"},
                                                                        {"Immediate Children", "immediate_children"},
                                                                        {"Prefab", "prefab"},
                                                                        {"Model", "model"},
                                                                        {"Mesh", "mesh"},
                                                                        {"Tag", "tag"},
                                                                        {"Collection", "collection"},
                                                                        {"Components", "components"}}};
    for (const auto& [label, type] : types) {
        if (MenuResult result = registry.addEntry(editorMenuOwner, selectGroupedMenuId,
                                                  {.label = label,
                                                   .operatorId = "object.select_grouped",
                                                   .properties = nlohmann::json{{"type", type}}.dump(),
                                                   .submenu = {},
                                                   .icon = std::nullopt,
                                                   .category = {}});
            !result) {
            return result;
        }
    }
    return {.success = true};
}

MenuResult registerPieMenus(MenuRegistry& registry) {
    // Entries fill the pie in Blender's order: left, right, bottom, top, then
    // top-left, top-right, bottom-left, bottom-right.
    struct PieEntry {
        const char* label;
        const char* operatorId;
        const char* properties;
    };

    struct Pie {
        const char* id;
        const char* title;
        std::vector<PieEntry> entries;
    };

    // transform.pivot_set comes with the pivot point tools; until it is
    // registered its entries show disabled.
    const std::vector<Pie> pies{
        {shadingPieMenuId,
         "Shading",
         {{"Wireframe", "view3d.shading_set", R"({"type":"wireframe"})"},
          {"Solid", "view3d.shading_set", R"({"type":"solid"})"},
          {"Material Preview", "view3d.shading_set", R"({"type":"material_preview"})"},
          {"Rendered", "view3d.shading_set", R"({"type":"rendered"})"}}},
        {viewPieMenuId,
         "View",
         {{"Left", "view3d.view_axis", R"({"type":"left"})"},
          {"Right", "view3d.view_axis", R"({"type":"right"})"},
          {"Bottom", "view3d.view_axis", R"({"type":"bottom"})"},
          {"Top", "view3d.view_axis", R"({"type":"top"})"},
          {"Front", "view3d.view_axis", R"({"type":"front"})"},
          {"Back", "view3d.view_axis", R"({"type":"back"})"},
          {"Camera", "view3d.view_camera", ""},
          {"Frame Selected", "view3d.frame_selected", ""}}},
        {pivotPieMenuId,
         "Pivot Point",
         {{"3D Cursor", "transform.pivot_set", R"({"pivot":"cursor"})"},
          {"Individual Origins", "transform.pivot_set", R"({"pivot":"individual_origins"})"},
          {"Median Point", "transform.pivot_set", R"({"pivot":"median_point"})"},
          {"Active Element", "transform.pivot_set", R"({"pivot":"active_element"})"}}},
    };
    for (const Pie& pie : pies) {
        if (MenuResult result = registry.addMenu(editorMenuOwner, {.id = pie.id, .title = pie.title}); !result) {
            return result;
        }
        for (const PieEntry& entry : pie.entries) {
            if (MenuResult result = registry.addEntry(editorMenuOwner, pie.id,
                                                      {.label = entry.label,
                                                       .operatorId = entry.operatorId,
                                                       .properties = entry.properties,
                                                       .submenu = {},
                                                       .icon = std::nullopt,
                                                       .category = {}});
                !result) {
                return result;
            }
        }
    }
    return {.success = true};
}
