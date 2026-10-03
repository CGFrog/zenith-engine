#include "HeadlessImGui.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/document/CommandHistory.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/menus/AddComponentMenu.hpp"
#include "editor/menus/DefaultMenus.hpp"
#include "editor/menus/EditorMenus.hpp"
#include "editor/menus/MenuRegistry.hpp"
#include "editor/operators/DefaultKeymap.hpp"
#include "editor/operators/Keymap.hpp"
#include "editor/operators/ObjectOperators.hpp"
#include "editor/operators/OperatorRegistry.hpp"
#include "editor/operators/OperatorRunner.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/panels/InspectorPanel.hpp"
#include "editor/settings/ComponentInspectorLayout.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/audio/AudioSystem.hpp"
#include "engine/diagnostics/Logger.hpp"
#include "engine/physics/PhysicsWorld.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/ui/GameUi.hpp"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

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
    expect(registerBuiltInComponentTypes(*types).success, "built-in component types should register");
    expect(types
               ->add(ComponentTypeBuilder<HealthComponent>("health", "Health")
                         .category("Gameplay")
                         .fields(Fields<HealthComponent>().add("amount", &HealthComponent::amount))
                         .build())
               .success,
           "a module component type should register");
    expect(types
               ->add(ComponentTypeBuilder<MarkerComponent>("marker", "Marker")
                         .fields(Fields<MarkerComponent>().add("flagged", &MarkerComponent::flagged))
                         .build())
               .success,
           "an uncategorized component type should register");
    return types;
}

// The session workflows menus use: operators run through the runner, polls
// and shortcuts are the real ones.
class MenuSessionActions final : public EditorSessionActions {
  public:
    MenuSessionActions(const OperatorRegistry& registry, OperatorRunner& runner,
                       std::function<OperatorContext()> context)
        : m_registry(registry)
        , m_runner(runner)
        , m_context(std::move(context)) {}

    OperatorResult invokeOperator(ApplicationContext&, std::string_view operatorId, std::string_view properties,
                                  KeymapRegion) override {
        OperatorContext context = m_context();
        return m_runner.invokeWithJson(operatorId, context, properties);
    }

    bool operatorAvailable(ApplicationContext&, std::string_view operatorId) override {
        const Operator* op = m_registry.find(operatorId);
        return op != nullptr && op->canRun(m_context());
    }

    std::string operatorShortcut(std::string_view operatorId, std::string_view properties) const override {
        return m_keymap.shortcutLabel(operatorId, properties);
    }

    void requestOpenScene(ApplicationContext&, std::filesystem::path) override {}

    void projectFilesMoved(ApplicationContext&, std::span<const ProjectFileMove>) override {}

    void projectFilesRemoved(ApplicationContext&, std::span<const std::filesystem::path>) override {}

    void handleObjectKeys(ApplicationContext&, KeymapRegion) override {}

    bool modalTransformActive() const override {
        return false;
    }

    void updateModalTransform(ApplicationContext&, ViewportId) override {}

  private:
    const OperatorRegistry& m_registry;
    OperatorRunner& m_runner;
    std::function<OperatorContext()> m_context;
    Keymap m_keymap{defaultEditorKeymap()};
};

// The objects an editor session owns, with its menus, without a window or GPU.
struct MenuFixture {
    Scene scene;
    AssetManager assets;
    PhysicsWorld physics;
    AudioSystem audio;
    ApplicationControl control;
    RenderRuntime rendering;
    GameUiContext gameUi;
    Logger logger;
    ApplicationContext app{scene, assets, physics, audio, control, rendering, gameUi, logger};
    EditorDocument document{std::filesystem::temp_directory_path()};
    CommandHistory history;
    EditorDiagnostics diagnostics;
    Selection selection;
    EditorMode mode{EditorMode::Edit};
    EditorMenus menus;
    OperatorRegistry operators;
    OperatorRunner runner{operators, history};
    MenuSessionActions actions{operators, runner, [this] { return operatorContext(); }};

    MenuFixture() {
        expect(registerObjectOperators(operators).success, "the object operators should register");
        expect(registerMenuOperators(operators, menus).success, "the menu operators should register");
        expect(registerDefaultMenus(menus.registry()).success, "the default menus should register");
        document.setComponentTypes(moduleComponentTypes());
    }

    SceneEditing editing() {
        return {scene, assets, document, history, diagnostics};
    }

    OperatorContext operatorContext() {
        return {.editing = editing(), .selection = selection, .mode = mode};
    }

    EditorContext context() {
        return {.app = app,
                .editing = editing(),
                .selection = selection,
                .diagnostics = diagnostics,
                .mode = mode,
                .actions = actions,
                .menus = menus};
    }

    OperatorResult invoke(std::string_view id, std::string_view properties = {}) {
        OperatorContext context = operatorContext();
        return runner.invokeWithJson(id, context, properties);
    }

    ResolvedMenu resolve(std::string menu, std::string category = {}) {
        std::optional<ResolvedMenu> resolved =
            menus.resolve({.menu = std::move(menu), .category = std::move(category)}, operatorContext());
        expect(resolved.has_value(), "a registered menu should resolve");
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

void testRegistration() {
    MenuFixture fixture;
    MenuRegistry& registry = fixture.menus.registry();
    expect(!registry.addMenu(editorMenuOwner, {.id = addMenuId, .title = "Again"}).success,
           "duplicate menu ids should be rejected");
    expect(!registry.addMenu(editorMenuOwner, {.id = "", .title = "Nameless"}).success, "menus need an id");
    expect(!registry.addEntry(editorMenuOwner, "MISSING_MT", operatorEntry("Cube", "object.add_cube")).success,
           "entries need a registered menu");
    expect(!registry.addEntry(editorMenuOwner, addMenuId, operatorEntry("", "object.add_cube")).success,
           "entries need a label");
    expect(!registry.addEntry(editorMenuOwner, addMenuId, operatorEntry("Nothing", "")).success,
           "entries need an operator or a submenu");
    MenuEntry both = operatorEntry("Both", "object.add_cube");
    both.submenu = addComponentMenuId;
    expect(!registry.addEntry(editorMenuOwner, addMenuId, both).success,
           "entries cannot have both an operator and a submenu");
    MenuEntry missingSubmenu = operatorEntry("Missing", "");
    missingSubmenu.submenu = "MISSING_MT";
    expect(!registry.addEntry(editorMenuOwner, addMenuId, missingSubmenu).success,
           "submenu entries need a registered submenu");
    MenuEntry badProperties = operatorEntry("Bad", "object.select_all");
    badProperties.properties = "[1, 2]";
    expect(!registry.addEntry(editorMenuOwner, addMenuId, badProperties).success,
           "entry properties must be a JSON object");
    expect(!registry.addSection(editorMenuOwner, addMenuId, {}).success, "sections need a fill function");
    expect(registry.createOwner() != registry.createOwner() && registry.createOwner() != editorMenuOwner,
           "owner tokens should be unique");
}

void testOwnersAndSections() {
    MenuFixture fixture;
    MenuRegistry& registry = fixture.menus.registry();
    const size_t editorLeaves = leafLabels(fixture.resolve(addMenuId)).size();

    // A module adds to the editor's Add menu and defines a menu of its own
    // with a dynamic section.
    const MenuOwner module = registry.createOwner();
    int fills = 0;
    expect(registry.addEntry(module, addMenuId, operatorEntry("Spinner", "object.add_empty", {"Game"})).success,
           "a module should add entries to an editor menu");
    expect(registry.addMenu(module, {.id = "GAME_MT_tools", .title = "Tools"}).success,
           "a module should define a menu");
    expect(registry
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
    expect(registry.addEntry(editorMenuOwner, addMenuId, link).success, "a submenu entry should register");

    const ResolvedMenu add = fixture.resolve(addMenuId);
    const MenuItem* game = findItem(add.items, "Game");
    expect(game != nullptr && game->children.size() == 1 && game->children.front().label == "Spinner",
           "module entries should appear in their category");
    const MenuItem* tools = findItem(add.items, "Tools");
    expect(tools != nullptr && tools->isGroup() && tools->children.size() == 1,
           "a submenu entry should expand to the submenu, leaving out malformed dynamic entries");
    expect(fills == 1, "a dynamic section should fill once per resolve");
    fixture.resolve(addMenuId);
    expect(fills == 2, "a dynamic section should fill again each time the menu is shown");

    // Categories with the same name share one submenu.
    const ResolvedMenu add2 = fixture.resolve(addMenuId);
    expect(std::ranges::count_if(add2.items, [](const MenuItem& item) { return item.label == "Light"; }) == 1,
           "categories should be merged");

    // Unloading removes everything the module added; the editor's link to its
    // menu stays and shows again when the module registers the menu again.
    expect(registry.removeOwner(module) == 3, "removing an owner should remove its menu, entry, and section");
    const ResolvedMenu unloaded = fixture.resolve(addMenuId);
    expect(findItem(unloaded.items, "Game") == nullptr && findItem(unloaded.items, "Tools") == nullptr,
           "an unloaded module's entries and missing submenus should not be shown");
    expect(leafLabels(unloaded).size() == editorLeaves, "the editor's own entries should stay");
    expect(!fixture.menus.resolve({.menu = "GAME_MT_tools", .category = {}}, fixture.operatorContext()),
           "a removed menu should no longer resolve");
    const MenuOwner reloaded = registry.createOwner();
    registry.addMenu(reloaded, {.id = "GAME_MT_tools", .title = "Tools"});
    registry.addEntry(reloaded, "GAME_MT_tools", operatorEntry("Reset", "object.select_all"));
    const ResolvedMenu reloadedAdd = fixture.resolve(addMenuId);
    const MenuItem* relinked = findItem(reloadedAdd.items, "Tools");
    expect(relinked != nullptr && relinked->children.size() == 1,
           "another owner's entries should show again when the menu is registered again");

    // A menu that contains itself stops nesting instead of recursing forever.
    registry.addMenu(editorMenuOwner, {.id = "LOOP_MT", .title = "Loop"});
    registry.addEntry(editorMenuOwner, "LOOP_MT", operatorEntry("Leaf", "object.add_empty"));
    MenuEntry loop = operatorEntry("Again", "");
    loop.submenu = "LOOP_MT";
    registry.addEntry(editorMenuOwner, "LOOP_MT", loop);
    expect(leafLabels(fixture.resolve("LOOP_MT")).size() == 9, "a submenu cycle should stop at the depth limit");
}

void testSearch() {
    MenuFixture fixture;
    const ResolvedMenu add = fixture.resolve(addMenuId);
    const std::vector<MenuSearchResult> sun = searchMenu(add, "sun");
    expect(!sun.empty() && sun.front().item->operatorId == "object.add_sun" && sun.front().path == "Light > Sun",
           "typing sun should find Light > Sun first");
    const std::vector<MenuSearchResult> health = searchMenu(add, "comp health");
    expect(!health.empty() && health.front().path == "Component > Gameplay > Health",
           "searches should match the category path");
    expect(searchMenu(add, "xyzzy").empty(), "unrelated text should match nothing");
    expect(searchMenu(add, "").size() == leafLabels(add).size(), "an empty query should list every leaf");
}

void testAddMenuContents() {
    MenuFixture fixture;
    const ResolvedMenu add = fixture.resolve(addMenuId);
    expect(add.title == "Add" && add.layout == MenuLayout::Submenus, "the Add menu should use submenus");
    for (const char* label : {"Mesh", "Light", "Environment", "Fog", "Empty", "Component"}) {
        expect(findItem(add.items, label) != nullptr, "the Add menu should offer the add-object entries");
    }
    const MenuItem* component = findItem(add.items, "Component");
    const MenuItem* gameplay = component != nullptr ? findItem(component->children, "Gameplay") : nullptr;
    const MenuItem* other = component != nullptr ? findItem(component->children, "Other") : nullptr;
    expect(gameplay != nullptr && findItem(gameplay->children, "Health") != nullptr,
           "module component types should be offered by category");
    expect(other != nullptr && findItem(other->children, "Marker") != nullptr,
           "uncategorized module types should be under Other");
    const std::vector<std::string> leaves = leafLabels(add);
    for (const ComponentType& type : componentTypesFor(fixture.document.options()).types()) {
        if (type.internal()) {
            expect(std::ranges::find(leaves, type.displayName()) == leaves.end(),
                   "internal component types should not be offered");
        }
    }
}

void testAddComponentEntries() {
    MenuFixture fixture;
    expect(fixture.resolve(addComponentMenuId).items.empty(), "with no active object nothing can be added");
    fixture.addSubject();
    const ResolvedMenu menu = fixture.resolve(addComponentMenuId);
    expect(menu.layout == MenuLayout::Sections, "Add Component should list categories as sections");
    const std::vector<std::string> leaves = leafLabels(menu);
    expect(std::ranges::find(leaves, "Marker") != leaves.end(), "module types should be offered");
    expect(std::ranges::find(leaves, "Health") == leaves.end() &&
               std::ranges::find(leaves, "Point Light") == leaves.end(),
           "components the object has should not be offered");
    expect(std::ranges::find(leaves, "Mesh Renderer") == leaves.end(), "internal types should not be offered");
    const ResolvedMenu lights = fixture.resolve(addComponentMenuId, "Lights");
    expect(!lights.items.empty() && std::ranges::all_of(lights.items,
                                                        [](const MenuItem& item) {
                                                            return !item.isGroup() &&
                                                                   item.operatorId == "object.component_add";
                                                        }),
           "a category request should list only that category's types, without a heading");
    expect(fixture.resolve(addComponentMenuId, "Nothing").items.empty(), "an unknown category should be empty");
}

void testAddEmptyWith() {
    MenuFixture fixture;
    expect(static_cast<bool>(fixture.invoke("object.add_empty_with", R"({"component":"point_light"})")),
           "adding an empty with a component should finish");
    const Entity created = fixture.scene.findEntity(fixture.selection.active().value_or(SceneEntityId{}));
    expect(created != entt::null && fixture.selection.size() == 1, "the new entity should be selected");
    if (created != entt::null) {
        expect(fixture.scene.registry().get<NameComponent>(created).name == "Point Light",
               "the entity should be named after its component");
        expect(fixture.scene.registry().all_of<PointLightComponent>(created), "the entity should have the component");
    }
    expect(fixture.history.size() == 1, "adding should record one undo step");
    fixture.invoke("object.add_empty_with", R"({"component":"health"})");
    expect(entityCount(fixture.scene) == 2, "module component types should be addable");
    expect(fixture.invoke("object.add_empty_with", R"({"component":"model"})").status == OperatorStatus::Failed,
           "internal component types should be refused");
    expect(fixture.invoke("object.add_empty_with", R"({"component":"nope"})").status == OperatorStatus::Failed,
           "unknown component types should be refused");
    expect(entityCount(fixture.scene) == 2, "refused adds should not create entities");
    fixture.diagnostics.acknowledgeErrors();
    fixture.editing().undo();
    fixture.editing().undo();
    expect(entityCount(fixture.scene) == 0, "undo should remove the added entities");
}

void testCallMenuOperators() {
    MenuFixture fixture;
    expect(fixture.invoke("wm.call_menu", R"({"menu":"MISSING_MT"})").status == OperatorStatus::Failed &&
               !fixture.menus.isOpen(),
           "calling an unknown menu should fail");
    fixture.diagnostics.acknowledgeErrors();
    expect(static_cast<bool>(fixture.invoke("object.add_menu")) && fixture.menus.isOpen() &&
               fixture.menus.request().menu == addMenuId,
           "object.add_menu should open the Add menu");
    fixture.menus.close();
    fixture.mode = EditorMode::Play;
    expect(fixture.invoke("object.add_menu").status == OperatorStatus::Cancelled,
           "the Add menu should not open in Play mode");
    expect(fixture.history.size() == 0, "opening menus should not record undo steps");
}

void testAddMenuPopup() {
    MenuFixture fixture;
    HeadlessImGuiContext imgui;
    // Shift A in the viewport runs whatever the keymap binds, as the session does.
    Keymap keymap(defaultEditorKeymap());
    const KeymapBinding* binding = keymap.resolve(KeymapRegion::Viewport, ImGuiMod_Shift | ImGuiKey_A, 0.0, 0.3);
    expect(binding != nullptr, "Shift A should be bound in the viewport");
    if (binding == nullptr) {
        return;
    }
    fixture.invoke(binding->operatorId, binding->properties);
    fixture.openFrames();
    expect(fixture.menus.isOpen() && ImGui::GetIO().WantTextInput, "Shift A should open the Add menu, search focused");

    HeadlessImGuiContext::typeNextFrame("sun");
    fixture.drawMenus();
    press(ImGuiKey_Enter, [&] { fixture.drawMenus(); });
    expect(fixture.scene.registry().view<const DirectionalLightComponent>().size() == 1 && fixture.history.size() == 1,
           "typing sun and Enter should add a sun as one undo step");
    expect(!fixture.menus.isOpen(), "choosing an entry should close the menu");

    // Module component types are offered too.
    fixture.invoke("wm.call_menu", R"({"menu":"VIEW3D_MT_add"})");
    fixture.openFrames();
    HeadlessImGuiContext::typeNextFrame("health");
    fixture.drawMenus();
    press(ImGuiKey_Enter, [&] { fixture.drawMenus(); });
    const Entity created = fixture.scene.findEntity(fixture.selection.active().value_or(SceneEntityId{}));
    expect(created != entt::null && fixture.scene.registry().all_of<HealthComponent>(created),
           "a module component type should be addable from the Add menu");

    fixture.invoke("object.add_menu");
    fixture.openFrames();
    press(ImGuiKey_Escape, [&] { fixture.drawMenus(); });
    fixture.drawMenus();
    expect(!fixture.menus.isOpen() && fixture.history.size() == 2, "Escape should close without running anything");
}

void testAddComponentPopup() {
    MenuFixture fixture;
    const SceneEntityId subject = fixture.addSubject();
    ComponentInspectorLayout componentLayout;
    InspectorPanel inspector(componentLayout);
    HeadlessImGuiContext imgui;
    fixture.drawPanel(inspector);
    const ImGuiWindow* window = HeadlessImGuiContext::findWindow(inspector.title());
    expect(window != nullptr, "the Inspector should draw into its window");
    if (window == nullptr) {
        return;
    }
    HeadlessImGuiContext::activateNextFrame(HeadlessImGuiContext::itemId(*window, "Add Component"));
    fixture.drawPanel(inspector);
    for (int frame = 0; frame < 3; ++frame) {
        fixture.drawPanel(inspector);
    }
    expect(fixture.menus.isOpen() && fixture.menus.request().menu == addComponentMenuId && ImGui::GetIO().WantTextInput,
           "Add Component should open its menu with the search focused");
    HeadlessImGuiContext::typeNextFrame("mark");
    fixture.drawPanel(inspector);
    press(ImGuiKey_Enter, [&] { fixture.drawPanel(inspector); });
    const Entity entity = fixture.scene.findEntity(subject);
    expect(fixture.scene.registry().all_of<MarkerComponent>(entity) && fixture.history.size() == 1,
           "searching and Enter should add the module component as one undo step");
    expect(!fixture.menus.isOpen(), "adding should close the menu");

    // A component the entity has is not offered: searching for it adds nothing.
    fixture.invoke("wm.call_menu", R"({"menu":"OBJECT_MT_add_component"})");
    for (int frame = 0; frame < 3; ++frame) {
        fixture.drawPanel(inspector);
    }
    HeadlessImGuiContext::typeNextFrame("health");
    fixture.drawPanel(inspector);
    press(ImGuiKey_Enter, [&] { fixture.drawPanel(inspector); });
    expect(fixture.history.size() == 1 && !fixture.diagnostics.hasUnacknowledgedError(),
           "a component the entity has should not be offered");
    press(ImGuiKey_Escape, [&] { fixture.drawPanel(inspector); });
}
} // namespace

int main() {
    testRegistration();
    testOwnersAndSections();
    testSearch();
    testAddMenuContents();
    testAddComponentEntries();
    testAddEmptyWith();
    testCallMenuOperators();
    testAddMenuPopup();
    testAddComponentPopup();
    if (failures != 0) {
        std::cerr << failures << " menu test(s) failed\n";
        return 1;
    }
    return 0;
}
