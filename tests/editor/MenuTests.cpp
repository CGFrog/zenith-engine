#include "EditorTestFixture.hpp"
#include "HeadlessImGui.hpp"

#include "editor/menus/AddComponentMenu.hpp"
#include "editor/menus/DefaultMenus.hpp"
#include "editor/menus/MenuRegistry.hpp"
#include "editor/menus/PieMenu.hpp"
#include "editor/operators/DefaultKeymap.hpp"
#include "editor/operators/Keymap.hpp"
#include "editor/operators/ObjectOperators.hpp"
#include "editor/panels/InspectorPanel.hpp"
#include "engine/scene/ComponentTypes.hpp"

#include "support/Doctest.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

TEST_SUITE_BEGIN("Menu");

namespace {

// Components a game module might add: one in its own category and one with
// no category.
struct HealthComponent {
    float amount{100.0f};
};

struct MarkerComponent {
    bool flagged{false};
};

std::shared_ptr<const ComponentTypeRegistry> moduleComponentTypes() {
    auto types = std::make_shared<ComponentTypeRegistry>();
    CHECK_MESSAGE(registerBuiltInComponentTypes(*types).success, "built-in component types should register");
    CHECK_MESSAGE(types
                      ->add(ComponentTypeBuilder<HealthComponent>("health", "Health")
                                .category("Gameplay")
                                .fields(Fields<HealthComponent>().add("amount", &HealthComponent::amount))
                                .build())
                      .success,
                  "a module component type should register");
    CHECK_MESSAGE(types
                      ->add(ComponentTypeBuilder<MarkerComponent>("marker", "Marker")
                                .fields(Fields<MarkerComponent>().add("flagged", &MarkerComponent::flagged))
                                .build())
                      .success,
                  "an uncategorized component type should register");
    return types;
}

// The objects an editor session owns, with its menus, without a window or GPU.
// Polls and shortcuts are the real ones.
struct MenuFixture : EditorTestFixture {
    KeymapSessionActions actions{operators, runner, sessionContextSource()};

    MenuFixture() {
        CHECK_MESSAGE(registerObjectOperators(operators).success, "the object operators should register");
        CHECK_MESSAGE(registerMenuOperators(operators, menus).success, "the menu operators should register");
        CHECK_MESSAGE(registerDefaultMenus(menus.registry()).success, "the default menus should register");
        document.setComponentTypes(moduleComponentTypes());
        useActions(actions);
    }

    ResolvedMenu resolve(std::string menu, std::string category = {}) {
        std::optional<ResolvedMenu> resolved =
            menus.resolve({.menu = std::move(menu), .category = std::move(category)}, operatorContext());
        CHECK_MESSAGE(resolved.has_value(), "a registered menu should resolve");
        return resolved.value_or(ResolvedMenu{});
    }

    // One frame drawing the open popup menu at the top level, as the session
    // does after its areas.
    void drawMenus() {
        ImGui::NewFrame();
        EditorContext editor = context();
        menus.draw(editor);
        ImGui::Render();
    }

    void openFrames() {
        for (int frame = 0; frame < 3; ++frame) {
            drawMenus();
        }
    }

    // A frame drawing panel in a full-size window, then the open popup menu.
    void drawPanel(InspectorPanel& panel) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0.0f, 0.0f});
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin(panel.title());
        EditorContext editor = context();
        panel.draw(editor);
        ImGui::End();
        menus.draw(editor);
        ImGui::Render();
    }

    // A selected entity that has the Health and Point Light components.
    SceneEntityId addSubject() {
        const Entity entity = scene.createEntity("Subject");
        const ComponentTypeRegistry& types = componentTypesFor(document.options());
        for (const char* key : {"health", "point_light"}) {
            types.find(key)->addDefault(scene, entity);
        }
        const SceneEntityId id = scene.id(entity);
        selection.selectOnly(id);
        return id;
    }
};

// Presses and releases chord over two frames of draw.
void press(ImGuiKeyChord chord, const std::function<void()>& draw) {
    ImGuiIO& io = ImGui::GetIO();
    const ImGuiKey key = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
    io.AddKeyEvent(ImGuiMod_Shift, (chord & ImGuiMod_Shift) != 0);
    io.AddKeyEvent(key, true);
    draw();
    io.AddKeyEvent(key, false);
    io.AddKeyEvent(ImGuiMod_Shift, false);
    draw();
}

const MenuItem* findItem(const std::vector<MenuItem>& items, std::string_view label) {
    const auto found = std::ranges::find_if(items, [label](const MenuItem& item) { return item.label == label; });
    return found == items.end() ? nullptr : &*found;
}

std::vector<std::string> leafLabels(const ResolvedMenu& menu) {
    std::vector<std::string> labels;
    for (const MenuSearchResult& result : searchMenu(menu, "")) {
        labels.push_back(result.item->label);
    }
    return labels;
}

size_t entityCount(const Scene& scene) {
    return scene.registry().view<const SceneEntityIdComponent>().size();
}

MenuEntry operatorEntry(std::string label, std::string operatorId, std::vector<std::string> category = {}) {
    return {.label = std::move(label),
            .operatorId = std::move(operatorId),
            .properties = {},
            .submenu = {},
            .icon = std::nullopt,
            .category = std::move(category)};
}

TEST_CASE("Menu: registration") {
    MenuFixture fixture;
    MenuRegistry& registry = fixture.menus.registry();
    CHECK_MESSAGE(!registry.addMenu(editorMenuOwner, {.id = addMenuId, .title = "Again"}).success,
                  "duplicate menu ids should be rejected");
    CHECK_MESSAGE(!registry.addMenu(editorMenuOwner, {.id = "", .title = "Nameless"}).success, "menus need an id");
    CHECK_MESSAGE(!registry.addEntry(editorMenuOwner, "MISSING_MT", operatorEntry("Cube", "object.add_cube")).success,
                  "entries need a registered menu");
    CHECK_MESSAGE(!registry.addEntry(editorMenuOwner, addMenuId, operatorEntry("", "object.add_cube")).success,
                  "entries need a label");
    CHECK_MESSAGE(!registry.addEntry(editorMenuOwner, addMenuId, operatorEntry("Nothing", "")).success,
                  "entries need an operator or a submenu");
    MenuEntry both = operatorEntry("Both", "object.add_cube");
    both.submenu = addComponentMenuId;
    CHECK_MESSAGE(!registry.addEntry(editorMenuOwner, addMenuId, both).success,
                  "entries cannot have both an operator and a submenu");
    MenuEntry missingSubmenu = operatorEntry("Missing", "");
    missingSubmenu.submenu = "MISSING_MT";
    CHECK_MESSAGE(!registry.addEntry(editorMenuOwner, addMenuId, missingSubmenu).success,
                  "submenu entries need a registered submenu");
    MenuEntry badProperties = operatorEntry("Bad", "object.select_all");
    badProperties.properties = "[1, 2]";
    CHECK_MESSAGE(!registry.addEntry(editorMenuOwner, addMenuId, badProperties).success,
                  "entry properties must be a JSON object");
    CHECK_MESSAGE(!registry.addSection(editorMenuOwner, addMenuId, {}).success, "sections need a fill function");
    CHECK_MESSAGE((registry.createOwner() != registry.createOwner() && registry.createOwner() != editorMenuOwner),
                  "owner tokens should be unique");
}

TEST_CASE("Menu: owners and sections") {
    MenuFixture fixture;
    MenuRegistry& registry = fixture.menus.registry();
    const size_t editorLeaves = leafLabels(fixture.resolve(addMenuId)).size();

    // A module adds to the editor's Add menu and defines a menu of its own
    // with a dynamic section.
    const MenuOwner module = registry.createOwner();
    int fills = 0;
    CHECK_MESSAGE(registry.addEntry(module, addMenuId, operatorEntry("Spinner", "object.add_empty", {"Game"})).success,
                  "a module should add entries to an editor menu");
    CHECK_MESSAGE(registry.addMenu(module, {.id = "GAME_MT_tools", .title = "Tools"}).success,
                  "a module should define a menu");
    CHECK_MESSAGE(registry
                      .addSection(module, "GAME_MT_tools",
                                  [&fills](const OperatorContext&, std::vector<MenuEntry>& entries) {
                                      ++fills;
                                      entries.push_back(operatorEntry("Reset", "object.select_all", {"Game"}));
                                      entries.push_back(operatorEntry("", "object.select_all"));
                                  })
                      .success,
                  "a module should add a dynamic section");
    // The editor links the module's menu from its own.
    MenuEntry link = operatorEntry("Tools", "");
    link.submenu = "GAME_MT_tools";
    CHECK_MESSAGE(registry.addEntry(editorMenuOwner, addMenuId, link).success, "a submenu entry should register");

    const ResolvedMenu add = fixture.resolve(addMenuId);
    const MenuItem* game = findItem(add.items, "Game");
    CHECK_MESSAGE((game != nullptr && game->children.size() == 1 && game->children.front().label == "Spinner"),
                  "module entries should appear in their category");
    const MenuItem* tools = findItem(add.items, "Tools");
    CHECK_MESSAGE((tools != nullptr && tools->isGroup() && tools->children.size() == 1),
                  "a submenu entry should expand to the submenu, leaving out malformed dynamic entries");
    CHECK_MESSAGE(fills == 1, "a dynamic section should fill once per resolve");
    fixture.resolve(addMenuId);
    CHECK_MESSAGE(fills == 2, "a dynamic section should fill again each time the menu is shown");

    // Categories with the same name share one submenu.
    const ResolvedMenu add2 = fixture.resolve(addMenuId);
    CHECK_MESSAGE(std::ranges::count_if(add2.items, [](const MenuItem& item) { return item.label == "Light"; }) == 1,
                  "categories should be merged");

    // Unloading removes everything the module added; the editor's link to its
    // menu stays and shows again when the module registers the menu again.
    CHECK_MESSAGE(registry.removeOwner(module) == 3, "removing an owner should remove its menu, entry, and section");
    const ResolvedMenu unloaded = fixture.resolve(addMenuId);
    CHECK_MESSAGE((findItem(unloaded.items, "Game") == nullptr && findItem(unloaded.items, "Tools") == nullptr),
                  "an unloaded module's entries and missing submenus should not be shown");
    CHECK_MESSAGE(leafLabels(unloaded).size() == editorLeaves, "the editor's own entries should stay");
    CHECK_MESSAGE(!fixture.menus.resolve({.menu = "GAME_MT_tools", .category = {}}, fixture.operatorContext()),
                  "a removed menu should no longer resolve");
    const MenuOwner reloaded = registry.createOwner();
    registry.addMenu(reloaded, {.id = "GAME_MT_tools", .title = "Tools"});
    registry.addEntry(reloaded, "GAME_MT_tools", operatorEntry("Reset", "object.select_all"));
    const ResolvedMenu reloadedAdd = fixture.resolve(addMenuId);
    const MenuItem* relinked = findItem(reloadedAdd.items, "Tools");
    CHECK_MESSAGE((relinked != nullptr && relinked->children.size() == 1),
                  "another owner's entries should show again when the menu is registered again");

    // A menu that contains itself stops nesting instead of recursing forever.
    registry.addMenu(editorMenuOwner, {.id = "LOOP_MT", .title = "Loop"});
    registry.addEntry(editorMenuOwner, "LOOP_MT", operatorEntry("Leaf", "object.add_empty"));
    MenuEntry loop = operatorEntry("Again", "");
    loop.submenu = "LOOP_MT";
    registry.addEntry(editorMenuOwner, "LOOP_MT", loop);
    CHECK_MESSAGE(leafLabels(fixture.resolve("LOOP_MT")).size() == 9, "a submenu cycle should stop at the depth limit");
}

TEST_CASE("Menu: search") {
    MenuFixture fixture;
    const ResolvedMenu add = fixture.resolve(addMenuId);
    const std::vector<MenuSearchResult> sun = searchMenu(add, "sun");
    CHECK_MESSAGE(
        (!sun.empty() && sun.front().item->operatorId == "object.add_sun" && sun.front().path == "Light > Sun"),
        "typing sun should find Light > Sun first");
    const std::vector<MenuSearchResult> health = searchMenu(add, "comp health");
    CHECK_MESSAGE((!health.empty() && health.front().path == "Component > Gameplay > Health"),
                  "searches should match the category path");
    CHECK_MESSAGE(searchMenu(add, "xyzzy").empty(), "unrelated text should match nothing");
    CHECK_MESSAGE(searchMenu(add, "").size() == leafLabels(add).size(), "an empty query should list every leaf");
}

TEST_CASE("Menu: add menu contents") {
    MenuFixture fixture;
    const ResolvedMenu add = fixture.resolve(addMenuId);
    CHECK_MESSAGE((add.title == "Add" && add.layout == MenuLayout::Submenus), "the Add menu should use submenus");
    for (const char* label : {"Mesh", "Light", "Environment", "Fog", "Empty", "Component"}) {
        CHECK_MESSAGE(findItem(add.items, label) != nullptr, "the Add menu should offer the add-object entries");
    }
    const MenuItem* light = findItem(add.items, "Light");
    for (const char* label : {"Sun", "Point Light", "Spot Light"}) {
        CHECK_MESSAGE((light != nullptr && findItem(light->children, label) != nullptr),
                      "Add > Light offers every light");
    }
    for (const auto& [category, label] :
         {std::pair{"Light Probe", "Reflection Probe"}, std::pair{"Light Probe", "Irradiance Volume"},
          std::pair{"Audio", "Audio Emitter"}, std::pair{"Audio", "Audio Listener"}, std::pair{"Effects", "Decal"},
          std::pair{"Effects", "Particle Emitter"}}) {
        const MenuItem* group = findItem(add.items, category);
        CHECK_MESSAGE((group != nullptr && findItem(group->children, label) != nullptr),
                      "the Add menu groups probes, audio, and effects");
    }
    CHECK_MESSAGE(findItem(add.items, "Camera") != nullptr, "the Add menu offers a camera");
    const MenuItem* component = findItem(add.items, "Component");
    const MenuItem* gameplay = component != nullptr ? findItem(component->children, "Gameplay") : nullptr;
    const MenuItem* other = component != nullptr ? findItem(component->children, "Other") : nullptr;
    CHECK_MESSAGE((gameplay != nullptr && findItem(gameplay->children, "Health") != nullptr),
                  "module component types should be offered by category");
    CHECK_MESSAGE((other != nullptr && findItem(other->children, "Marker") != nullptr),
                  "uncategorized module types should be under Other");
    const std::vector<std::string> leaves = leafLabels(add);
    for (const ComponentType& type : componentTypesFor(fixture.document.options()).types()) {
        if (type.internal()) {
            CHECK_MESSAGE(std::ranges::find(leaves, type.displayName()) == leaves.end(),
                          "internal component types should not be offered");
        }
    }
}

TEST_CASE("Menu: add component entries") {
    MenuFixture fixture;
    CHECK_MESSAGE(fixture.resolve(addComponentMenuId).items.empty(), "with no active object nothing can be added");
    fixture.addSubject();
    const ResolvedMenu menu = fixture.resolve(addComponentMenuId);
    CHECK_MESSAGE(menu.layout == MenuLayout::Sections, "Add Component should list categories as sections");
    const std::vector<std::string> leaves = leafLabels(menu);
    CHECK_MESSAGE(std::ranges::find(leaves, "Marker") != leaves.end(), "module types should be offered");
    CHECK_MESSAGE((std::ranges::find(leaves, "Health") == leaves.end() &&
                   std::ranges::find(leaves, "Point Light") == leaves.end()),
                  "components the object has should not be offered");
    CHECK_MESSAGE(std::ranges::find(leaves, "Mesh Renderer") == leaves.end(), "internal types should not be offered");
    const ResolvedMenu lights = fixture.resolve(addComponentMenuId, "Lights");
    CHECK_MESSAGE((!lights.items.empty() && std::ranges::all_of(lights.items,
                                                                [](const MenuItem& item) {
                                                                    return !item.isGroup() &&
                                                                           item.operatorId == "object.component_add";
                                                                })),
                  "a category request should list only that category's types, without a heading");
    CHECK_MESSAGE(fixture.resolve(addComponentMenuId, "Nothing").items.empty(), "an unknown category should be empty");
}

// Physics is reachable from both menus: Add > Physics creates colliders with
// a rigid body, and Add Component lists the physics component types.
TEST_CASE("Menu: physics entries") {
    MenuFixture fixture;
    const ResolvedMenu add = fixture.resolve(addMenuId);
    const MenuItem* physics = findItem(add.items, "Physics");
    for (const char* label : {"Static Box", "Static Sphere", "Static Capsule", "Dynamic Box", "Dynamic Sphere",
                              "Dynamic Capsule", "Rigid Body", "Character Controller", "Character"}) {
        CHECK_MESSAGE((physics != nullptr && findItem(physics->children, label) != nullptr),
                      "Add > Physics offers colliders, a rigid body, and characters");
    }
    const MenuItem* dynamicSphere = physics != nullptr ? findItem(physics->children, "Dynamic Sphere") : nullptr;
    CHECK_MESSAGE((dynamicSphere != nullptr &&
                   static_cast<bool>(fixture.invoke(dynamicSphere->operatorId, dynamicSphere->properties))),
                  "a collider entry should add an object");
    const std::optional<SceneEntityId> sphereId = fixture.selection.active();
    const Entity sphere = sphereId ? fixture.scene.findEntity(*sphereId) : entt::null;
    CHECK_MESSAGE(
        (sphere != entt::null && fixture.scene.registry().all_of<ColliderComponent, RigidBodyComponent>(sphere) &&
         std::holds_alternative<SphereCollider>(fixture.scene.registry().get<ColliderComponent>(sphere).shape) &&
         fixture.scene.registry().get<RigidBodyComponent>(sphere).motionType == PhysicsMotionType::Dynamic &&
         fixture.scene.registry().get<RigidBodyComponent>(sphere).layer == PhysicsLayer::Dynamic),
        "a Dynamic Sphere is a sphere collider on a dynamic body");
    const MenuItem* staticCapsule = physics != nullptr ? findItem(physics->children, "Static Capsule") : nullptr;
    CHECK_MESSAGE((staticCapsule != nullptr &&
                   static_cast<bool>(fixture.invoke(staticCapsule->operatorId, staticCapsule->properties))),
                  "a static collider entry should add an object");
    const Entity capsule = fixture.scene.findEntity(fixture.selection.active().value_or(SceneEntityId{}));
    CHECK_MESSAGE(
        (capsule != entt::null &&
         std::holds_alternative<CapsuleCollider>(fixture.scene.registry().get<ColliderComponent>(capsule).shape) &&
         fixture.scene.registry().get<RigidBodyComponent>(capsule).motionType == PhysicsMotionType::Static),
        "a Static Capsule is a capsule collider on a static body");

    fixture.addSubject();
    const ResolvedMenu components = fixture.resolve(addComponentMenuId, "Physics");
    const std::vector<std::string> leaves = leafLabels(components);
    for (const char* label : {"Collider", "Rigid Body", "Character Controller"}) {
        CHECK_MESSAGE(std::ranges::find(leaves, label) != leaves.end(),
                      "Add Component lists the physics component types");
    }
}

// Add menu entries create what they name; a second camera never takes the
// primary flag from the first.
TEST_CASE("Menu: add menu objects") {
    MenuFixture fixture;
    const ResolvedMenu add = fixture.resolve(addMenuId);
    // The object entries, not the Component group's one-per-type entries.
    std::vector<const MenuItem*> entries;
    for (const MenuItem& item : add.items) {
        if (item.label == "Component") {
            continue;
        }
        entries.push_back(&item);
        for (const MenuItem& child : item.children) {
            entries.push_back(&child);
        }
    }
    for (const MenuItem* item : entries) {
        if (item->operatorId == "object.add_empty_with" || item->operatorId == "object.add_camera") {
            CHECK_MESSAGE((static_cast<bool>(fixture.invoke(item->operatorId, item->properties)) &&
                           fixture.selection.size() == 1),
                          "every component entry of the Add menu adds a selected object");
        }
    }
    const Entity spot = fixture.scene.findEntity(fixture.selection.active().value_or(SceneEntityId{}));
    CHECK_MESSAGE((spot != entt::null), "the last added object should be selected");
    std::vector<bool> primaries;
    for (const auto [entity, camera] : fixture.scene.registry().view<const CameraComponent>().each()) {
        primaries.push_back(camera.primary);
    }
    CHECK_MESSAGE((primaries.size() == 1 && primaries.front()), "the first camera added is primary");
    CHECK_MESSAGE((static_cast<bool>(fixture.invoke("object.add_camera"))), "a second camera should be added");
    size_t primaryCount = 0;
    for (const auto [entity, camera] : fixture.scene.registry().view<const CameraComponent>().each()) {
        primaryCount += camera.primary ? 1 : 0;
    }
    CHECK_MESSAGE(primaryCount == 1, "adding a camera keeps the existing primary camera");
    for (const char* key : {"spot_light", "audio_emitter", "audio_listener", "decal", "particle_emitter",
                            "reflection_probe", "irradiance_volume", "point_light"}) {
        const ComponentType* type = componentTypesFor(fixture.document.options()).find(key);
        bool found = false;
        for (const Entity entity : fixture.scene.registry().view<const SceneEntityIdComponent>()) {
            found = found || (type != nullptr && type->has(fixture.scene, entity));
        }
        CHECK_MESSAGE(found, "each Add menu component entry creates its component");
    }
    fixture.mode = EditorMode::Play;
    CHECK_MESSAGE(
        (fixture.invoke("object.add_camera").status == OperatorStatus::Cancelled &&
         fixture.invoke("object.add_empty_with", R"({"component":"point_light"})").status == OperatorStatus::Cancelled),
        "the Add menu's operators follow the menu and refuse in Play");
    fixture.mode = EditorMode::Edit;

    // The Prefab section lists the project's prefabs when a source is set.
    MenuRegistry prefabMenus;
    const std::filesystem::path prefab = fixture.document.options().assetRoot / "props" / "crate.prefab";
    CHECK_MESSAGE(
        registerDefaultMenus(prefabMenus, {.projectPrefabs = [prefab] { return std::vector{prefab}; }}).success,
        "the default menus should register with a prefab source");
    const std::optional<ResolvedMenu> withPrefabs = prefabMenus.resolve(addMenuId, fixture.operatorContext());
    const MenuItem* prefabs = withPrefabs ? findItem(withPrefabs->items, "Prefab") : nullptr;
    CHECK_MESSAGE((prefabs != nullptr && !prefabs->children.empty() &&
                   prefabs->children.front().operatorId == "prefab.instantiate"),
                  "Add > Prefab lists the project's prefabs");
}

TEST_CASE("Menu: add empty with") {
    MenuFixture fixture;
    CHECK_MESSAGE((static_cast<bool>(fixture.invoke("object.add_empty_with", R"({"component":"point_light"})"))),
                  "adding an empty with a component should finish");
    const Entity created = fixture.scene.findEntity(fixture.selection.active().value_or(SceneEntityId{}));
    CHECK_MESSAGE((created != entt::null && fixture.selection.size() == 1), "the new entity should be selected");
    if (created != entt::null) {
        CHECK_MESSAGE((fixture.scene.registry().get<NameComponent>(created).name == "Point Light"),
                      "the entity should be named after its component");
        CHECK_MESSAGE((fixture.scene.registry().all_of<PointLightComponent>(created)),
                      "the entity should have the component");
    }
    CHECK_MESSAGE(fixture.history.size() == 1, "adding should record one undo step");
    fixture.invoke("object.add_empty_with", R"({"component":"health"})");
    CHECK_MESSAGE(entityCount(fixture.scene) == 2, "module component types should be addable");
    CHECK_MESSAGE(fixture.invoke("object.add_empty_with", R"({"component":"model"})").status == OperatorStatus::Failed,
                  "internal component types should be refused");
    CHECK_MESSAGE(fixture.invoke("object.add_empty_with", R"({"component":"nope"})").status == OperatorStatus::Failed,
                  "unknown component types should be refused");
    CHECK_MESSAGE(entityCount(fixture.scene) == 2, "refused adds should not create entities");
    fixture.diagnostics.acknowledgeErrors();
    fixture.editing().undo();
    fixture.editing().undo();
    CHECK_MESSAGE(entityCount(fixture.scene) == 0, "undo should remove the added entities");
}

TEST_CASE("Menu: call menu operators") {
    MenuFixture fixture;
    CHECK_MESSAGE((fixture.invoke("wm.call_menu", R"({"menu":"MISSING_MT"})").status == OperatorStatus::Failed &&
                   !fixture.menus.isOpen()),
                  "calling an unknown menu should fail");
    fixture.diagnostics.acknowledgeErrors();
    CHECK_MESSAGE((static_cast<bool>(fixture.invoke("object.add_menu")) && fixture.menus.isOpen() &&
                   fixture.menus.request().menu == addMenuId),
                  "object.add_menu should open the Add menu");
    fixture.menus.close();
    fixture.mode = EditorMode::Play;
    CHECK_MESSAGE(fixture.invoke("object.add_menu").status == OperatorStatus::Cancelled,
                  "the Add menu should not open in Play mode");
    CHECK_MESSAGE(fixture.history.size() == 0, "opening menus should not record undo steps");
}

TEST_CASE("Menu: add menu popup") {
    MenuFixture fixture;
    HeadlessImGuiContext imgui;
    // Shift A in the viewport runs whatever the keymap binds, as the session does.
    Keymap keymap(defaultEditorKeymap());
    const KeymapBinding* binding = keymap.resolve(KeymapRegion::Viewport, ImGuiMod_Shift | ImGuiKey_A, 0.0, 0.3);
    CHECK_MESSAGE(binding != nullptr, "Shift A should be bound in the viewport");
    if (binding == nullptr) {
        return;
    }
    fixture.invoke(binding->operatorId, binding->properties);
    fixture.openFrames();
    CHECK_MESSAGE((fixture.menus.isOpen() && ImGui::GetIO().WantTextInput),
                  "Shift A should open the Add menu, search focused");

    HeadlessImGuiContext::typeNextFrame("sun");
    fixture.drawMenus();
    press(ImGuiKey_Enter, [&] { fixture.drawMenus(); });
    CHECK_MESSAGE(
        (fixture.scene.registry().view<const DirectionalLightComponent>().size() == 1 && fixture.history.size() == 1),
        "typing sun and Enter should add a sun as one undo step");
    CHECK_MESSAGE(!fixture.menus.isOpen(), "choosing an entry should close the menu");

    // Module component types are offered too.
    fixture.invoke("wm.call_menu", R"({"menu":"VIEW3D_MT_add"})");
    fixture.openFrames();
    HeadlessImGuiContext::typeNextFrame("health");
    fixture.drawMenus();
    press(ImGuiKey_Enter, [&] { fixture.drawMenus(); });
    const Entity created = fixture.scene.findEntity(fixture.selection.active().value_or(SceneEntityId{}));
    CHECK_MESSAGE((created != entt::null && fixture.scene.registry().all_of<HealthComponent>(created)),
                  "a module component type should be addable from the Add menu");

    fixture.invoke("object.add_menu");
    fixture.openFrames();
    press(ImGuiKey_Escape, [&] { fixture.drawMenus(); });
    fixture.drawMenus();
    CHECK_MESSAGE((!fixture.menus.isOpen() && fixture.history.size() == 2),
                  "Escape should close without running anything");
}

TEST_CASE("Menu: add component popup") {
    MenuFixture fixture;
    const SceneEntityId subject = fixture.addSubject();
    ComponentInspectorLayout componentLayout;
    InspectorPanel inspector(componentLayout);
    HeadlessImGuiContext imgui;
    fixture.drawPanel(inspector);
    const ImGuiWindow* window = HeadlessImGuiContext::findWindow(inspector.title());
    CHECK_MESSAGE(window != nullptr, "the Inspector should draw into its window");
    if (window == nullptr) {
        return;
    }
    HeadlessImGuiContext::activateNextFrame(HeadlessImGuiContext::itemId(*window, "Add Component"));
    fixture.drawPanel(inspector);
    for (int frame = 0; frame < 3; ++frame) {
        fixture.drawPanel(inspector);
    }
    CHECK_MESSAGE(
        (fixture.menus.isOpen() && fixture.menus.request().menu == addComponentMenuId && ImGui::GetIO().WantTextInput),
        "Add Component should open its menu with the search focused");
    HeadlessImGuiContext::typeNextFrame("mark");
    fixture.drawPanel(inspector);
    press(ImGuiKey_Enter, [&] { fixture.drawPanel(inspector); });
    const Entity entity = fixture.scene.findEntity(subject);
    CHECK_MESSAGE((fixture.scene.registry().all_of<MarkerComponent>(entity) && fixture.history.size() == 1),
                  "searching and Enter should add the module component as one undo step");
    CHECK_MESSAGE(!fixture.menus.isOpen(), "adding should close the menu");

    // A component the entity has is not offered: searching for it adds nothing.
    fixture.invoke("wm.call_menu", R"({"menu":"OBJECT_MT_add_component"})");
    for (int frame = 0; frame < 3; ++frame) {
        fixture.drawPanel(inspector);
    }
    HeadlessImGuiContext::typeNextFrame("health");
    fixture.drawPanel(inspector);
    press(ImGuiKey_Enter, [&] { fixture.drawPanel(inspector); });
    CHECK_MESSAGE((fixture.history.size() == 1 && !fixture.diagnostics.hasUnacknowledgedError()),
                  "a component the entity has should not be offered");
    press(ImGuiKey_Escape, [&] { fixture.drawPanel(inspector); });
}

TEST_CASE("Menu: pie geometry") {
    constexpr float deadZone = 10.0f;
    CHECK_MESSAGE((pieItemAt({-100.0f, 0.0f}, 4, deadZone) == 0u && pieItemAt({100.0f, 0.0f}, 4, deadZone) == 1u &&
                   pieItemAt({0.0f, 100.0f}, 4, deadZone) == 2u && pieItemAt({0.0f, -100.0f}, 4, deadZone) == 3u),
                  "pie items should fill left, right, bottom, top");
    CHECK_MESSAGE((pieItemAt({70.0f, -70.0f}, 8, deadZone) == 5u && pieItemAt({-70.0f, 70.0f}, 8, deadZone) == 6u),
                  "the diagonals should hold items five to eight");
    CHECK_MESSAGE(pieItemAt({30.0f, -100.0f}, 2, deadZone) == 1u,
                  "with two items the right half should pick the right one");
    CHECK_MESSAGE((!pieItemAt({4.0f, 3.0f}, 4, deadZone) && !pieItemAt({100.0f, 0.0f}, 0, deadZone)),
                  "the dead zone and an empty pie should pick nothing");

    MenuFixture fixture;
    const std::vector<MenuItem> shading = pieItems(fixture.resolve(shadingPieMenuId));
    CHECK_MESSAGE((shading.size() == 4 && shading[0].label == "Wireframe" &&
                   shading[0].operatorId == "view3d.shading_set" && shading[0].properties == R"({"type":"wireframe"})"),
                  "the Shading pie should put Wireframe on the left");
    CHECK_MESSAGE(pieItems(fixture.resolve(viewPieMenuId)).size() == maximumPieItems,
                  "the View pie should fill all eight directions");
    const std::vector<MenuItem> pivot = pieItems(fixture.resolve(pivotPieMenuId));
    CHECK_MESSAGE(
        (!pivot.empty() &&
         std::ranges::all_of(pivot, [](const MenuItem& item) { return item.operatorId == "transform.pivot_set"; })),
        "the Pivot pie should run transform.pivot_set even before it is registered");
    Keymap keymap(defaultEditorKeymap());
    const KeymapBinding* z = keymap.resolve(KeymapRegion::Viewport, ImGuiKey_Z, 0.0, 0.3);
    CHECK_MESSAGE(
        (z != nullptr && z->operatorId == "wm.call_pie" && z->properties == R"({"menu":"VIEW3D_MT_shading_pie"})"),
        "Z should call the Shading pie in the viewport");
}

// A fixture with a test pie whose entries record which one ran; the fourth
// (top) entry's operator never polls.
struct PieFixture : MenuFixture {
    struct RecordProperties {
        std::string name;
    };

    std::vector<std::string> picked;
    HeadlessImGuiContext imgui;
    ImVec2 center{640.0f, 360.0f};

    PieFixture() {
        CHECK_MESSAGE(operators
                          .add({.id = "test.record",
                                .label = "Record",
                                .description = "Records its name",
                                .flags = OperatorFlags::None,
                                .properties = OperatorProperties::make(
                                    RecordProperties{},
                                    std::move(Fields<RecordProperties>().add("name", &RecordProperties::name))),
                                .poll = {},
                                .execute =
                                    [this](OperatorContext&, OperatorProperties& properties) {
                                        picked.push_back(properties.get<RecordProperties>()->name);
                                        return OperatorResult::finished();
                                    }})
                          .success,
                      "the recording operator should register");
        CHECK_MESSAGE(
            operators
                .add({.id = "test.never",
                      .label = "Never",
                      .description = "Never available",
                      .flags = OperatorFlags::None,
                      .properties = {},
                      .poll = [](const OperatorContext&) { return false; },
                      .execute = [](OperatorContext&, OperatorProperties&) { return OperatorResult::finished(); }})
                .success,
            "the unavailable operator should register");
        MenuRegistry& registry = menus.registry();
        CHECK_MESSAGE(registry.addMenu(editorMenuOwner, {.id = "TEST_MT_pie", .title = "Test"}).success,
                      "the test pie should register");
        for (const char* name : {"west", "east", "south"}) {
            MenuEntry entry = operatorEntry(name, "test.record");
            entry.properties = std::string(R"({"name":")") + name + "\"}";
            registry.addEntry(editorMenuOwner, "TEST_MT_pie", std::move(entry));
        }
        registry.addEntry(editorMenuOwner, "TEST_MT_pie", operatorEntry("north", "test.never"));
        menus.setPieMenusEnabled(true);
        moveMouse({0.0f, 0.0f});
    }

    void moveMouse(ImVec2 offset) {
        ImGui::GetIO().AddMousePosEvent(center.x + offset.x, center.y + offset.y);
        drawMenus();
    }

    // Opens the pie with key held down (ImGuiKey_None for none).
    void openPie(ImGuiKey key) {
        if (key != ImGuiKey_None) {
            ImGui::GetIO().AddKeyEvent(key, true);
            drawMenus();
        }
        CHECK_MESSAGE((static_cast<bool>(invoke("wm.call_pie", R"({"menu":"TEST_MT_pie"})")) && menus.pieOpen()),
                      "wm.call_pie should open the pie");
        drawMenus();
    }

    void release(ImGuiKey key) {
        ImGui::GetIO().AddKeyEvent(key, false);
        drawMenus();
    }

    void click(ImGuiMouseButton button) {
        ImGui::GetIO().AddMouseButtonEvent(button, true);
        drawMenus();
        ImGui::GetIO().AddMouseButtonEvent(button, false);
        drawMenus();
    }
};

TEST_CASE("Menu: pie hold release") {
    PieFixture fixture;
    fixture.openPie(ImGuiKey_Z);
    CHECK_MESSAGE(fixture.menus.pie().followsKey(), "a pie opened by a held key should follow it");
    CHECK_MESSAGE(std::abs(fixture.menus.pie().center().x - fixture.center.x) < 1.0f,
                  "the pie should open around the mouse");
    fixture.moveMouse({PieMenu::radius(), 4.0f});
    CHECK_MESSAGE(fixture.menus.pie().highlighted() == 1u, "pointing right should highlight the right item");
    fixture.release(ImGuiKey_Z);
    CHECK_MESSAGE((fixture.picked == std::vector<std::string>{"east"} && !fixture.menus.pieOpen()),
                  "letting go of the key should run the item the mouse points at and close the pie");

    // Letting go over an unavailable item, or after holding in the middle,
    // closes without running anything.
    fixture.moveMouse({0.0f, 0.0f});
    fixture.openPie(ImGuiKey_Z);
    fixture.moveMouse({0.0f, -PieMenu::radius()});
    fixture.release(ImGuiKey_Z);
    CHECK_MESSAGE((fixture.picked.size() == 1 && !fixture.menus.pieOpen()), "an unavailable item should not run");
    fixture.moveMouse({0.0f, 0.0f});
    fixture.openPie(ImGuiKey_Z);
    for (int frame = 0; frame < 30; ++frame) {
        fixture.drawMenus();
    }
    fixture.release(ImGuiKey_Z);
    CHECK_MESSAGE((fixture.picked.size() == 1 && !fixture.menus.pieOpen()),
                  "a long hold let go in the middle should close the pie");
}

TEST_CASE("Menu: pie tap then click") {
    PieFixture fixture;
    fixture.openPie(ImGuiKey_Z);
    fixture.release(ImGuiKey_Z);
    CHECK_MESSAGE((fixture.menus.pieOpen() && !fixture.menus.pie().followsKey() && fixture.picked.empty()),
                  "a quick tap should leave the pie open for a click");
    fixture.moveMouse({-4.0f, PieMenu::radius()});
    fixture.click(ImGuiMouseButton_Left);
    CHECK_MESSAGE((fixture.picked == std::vector<std::string>{"south"} && !fixture.menus.pieOpen()),
                  "a click should run the item the mouse points at");

    // Without a held key (opened from a menu) the pie waits for a click; a
    // click in the middle dismisses it.
    fixture.moveMouse({0.0f, 0.0f});
    fixture.openPie(ImGuiKey_None);
    CHECK_MESSAGE((fixture.menus.pieOpen() && !fixture.menus.pie().followsKey()),
                  "a pie opened without a key should wait");
    fixture.click(ImGuiMouseButton_Left);
    CHECK_MESSAGE((fixture.picked.size() == 1 && !fixture.menus.pieOpen()),
                  "a click in the middle should dismiss the pie");
}

TEST_CASE("Menu: pie cancel") {
    PieFixture fixture;
    fixture.openPie(ImGuiKey_None);
    fixture.moveMouse({-PieMenu::radius(), 0.0f});
    press(ImGuiKey_Escape, [&] { fixture.drawMenus(); });
    CHECK_MESSAGE((!fixture.menus.pieOpen() && fixture.picked.empty()), "Escape should cancel the pie");

    fixture.moveMouse({0.0f, 0.0f});
    fixture.openPie(ImGuiKey_Z);
    fixture.moveMouse({-PieMenu::radius(), 0.0f});
    fixture.click(ImGuiMouseButton_Right);
    fixture.release(ImGuiKey_Z);
    CHECK_MESSAGE((!fixture.menus.pieOpen() && fixture.picked.empty()), "a right click should cancel a held pie");
}

TEST_CASE("Menu: pies off fall back") {
    PieFixture fixture;
    fixture.menus.setPieMenusEnabled(false);
    CHECK_MESSAGE((static_cast<bool>(fixture.invoke("wm.call_pie", R"({"menu":"TEST_MT_pie"})")) &&
                   fixture.menus.isOpen() && !fixture.menus.pieOpen() && fixture.menus.request().menu == "TEST_MT_pie"),
                  "with pies off, wm.call_pie should open the searchable menu");
    fixture.openFrames();
    HeadlessImGuiContext::typeNextFrame("east");
    fixture.drawMenus();
    press(ImGuiKey_Enter, [&] { fixture.drawMenus(); });
    CHECK_MESSAGE((fixture.picked == std::vector<std::string>{"east"}), "the fallback menu should be searchable");
    CHECK_MESSAGE(fixture.invoke("wm.call_pie", R"({"menu":"MISSING_MT"})").status == OperatorStatus::Failed,
                  "calling an unknown pie should fail");
    fixture.diagnostics.acknowledgeErrors();
}
} // namespace

TEST_SUITE_END();
