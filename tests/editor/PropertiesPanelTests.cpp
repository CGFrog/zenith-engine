#include "HeadlessImGui.hpp"
#include "engine/diagnostics/Logger.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/document/CommandHistory.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/interaction/ComponentEditors.hpp"
#include "editor/interaction/EntityPropertiesView.hpp"
#include "editor/interaction/ReflectedFieldEditor.hpp"
#include "editor/menus/EditorMenus.hpp"
#include "editor/operators/ComponentOperators.hpp"
#include "editor/operators/ObjectOperators.hpp"
#include "editor/operators/OperatorRegistry.hpp"
#include "editor/operators/OperatorRunner.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/panels/GraphicsPanel.hpp"
#include "editor/panels/InspectorPanel.hpp"
#include "editor/panels/PropertiesPanel.hpp"
#include "editor/settings/ComponentInspectorLayout.hpp"
#include "editor/settings/RenderSettingsController.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/audio/AudioSystem.hpp"
#include "engine/physics/PhysicsWorld.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/ui/GameUi.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

// The session workflows a panel triggers, reduced to running operators.
class OperatorActions final : public EditorSessionActions {
  public:
    OperatorActions(OperatorRunner& runner, std::function<OperatorContext()> context)
        : m_runner(runner)
        , m_context(std::move(context)) {}

    OperatorResult invokeOperator(ApplicationContext&, std::string_view operatorId, std::string_view properties,
                                  KeymapRegion) override {
        OperatorContext context = m_context();
        return m_runner.invokeWithJson(operatorId, context, properties);
    }

    bool operatorAvailable(ApplicationContext&, std::string_view) override {
        return true;
    }

    std::string operatorShortcut(std::string_view, std::string_view) const override {
        return {};
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
    OperatorRunner& m_runner;
    std::function<OperatorContext()> m_context;
};

// The objects an editor session owns, without a window or GPU.
struct PanelFixture {
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
    MemoryClipboard clipboard;
    ComponentInspectorLayout componentLayout;
    OperatorRegistry operators;
    OperatorRunner runner{operators, history};
    OperatorActions actions{runner, [this] { return operatorContext(); }};
    EditorMenus menus;
    RenderSettingsController renderSettings{EditorRenderSettings(
        ProjectConfig{}, std::filesystem::temp_directory_path() / "zenith-properties-panel-render.json")};

    PanelFixture() {
        expect(registerObjectOperators(operators).success, "the object operators should register");
        expect(registerComponentOperators(operators, {.clipboard = clipboard, .layout = componentLayout}).success,
               "the component operators should register");
        document.setComponentTypes(moduleComponentTypes());
    }

    SceneEditing editing() {
        return {scene, assets, document, history, diagnostics};
    }

    OperatorContext operatorContext() {
        return {.editing = editing(), .selection = selection, .mode = EditorMode::Edit};
    }

    EditorContext context() {
        return {.app = app,
                .editing = editing(),
                .selection = selection,
                .diagnostics = diagnostics,
                .mode = EditorMode::Edit,
                .actions = actions,
                .menus = menus};
    }

    // An entity with components from several built-in categories and both
    // module types, selected.
    SceneEntityId addSubject() {
        const Entity entity = scene.createEntity("Subject");
        const ComponentTypeRegistry& types = componentTypesFor(document.options());
        for (const char* key : {"point_light", "volumetric_fog", "collider", "health", "marker"}) {
            types.find(key)->addDefault(scene, entity);
        }
        const SceneEntityId id = scene.id(entity);
        selection.selectOnly(id);
        return id;
    }

    // Draws panel for one frame inside its own window, as the session does.
    template <typename Panel> void draw(Panel& panel) {
        HeadlessImGuiFrame frame(panel.title());
        EditorContext editor = context();
        panel.draw(editor);
    }

    // Draws panel for one frame and returns the text it rendered (labels,
    // values, and checkbox marks such as "[~]"), through ImGui's text log.
    template <typename Panel> std::string drawLogged(Panel& panel) {
        HeadlessImGuiFrame frame(panel.title());
        ImGui::LogToBuffer();
        EditorContext editor = context();
        panel.draw(editor);
        std::string text = ImGui::GetCurrentContext()->LogBuffer.c_str();
        ImGui::LogFinish();
        return text;
    }
};

std::vector<std::string> labels(const std::vector<PropertiesTab>& tabs) {
    std::vector<std::string> result;
    for (const PropertiesTab& tab : tabs) {
        result.push_back(tab.label());
    }
    return result;
}

void testCategories() {
    PanelFixture fixture;
    const SceneEntityId subject = fixture.addSubject();
    const std::vector<std::string> categories =
        entityComponentCategories(componentTypesFor(fixture.document.options()), fixture.scene, subject);
    for (const char* expected : {"Lights", "Environment", "Physics", "Gameplay", "Other"}) {
        expect(std::ranges::count(categories, expected) == 1, "each category the entity has should appear once");
    }
    expect(categories.size() == 5, "only categories of components the entity has should appear");
    expect(entityComponentCategories(componentTypesFor(fixture.document.options()), fixture.scene, {}).empty(),
           "a missing entity should have no categories");
}

void testTabs() {
    PanelFixture fixture;
    PropertiesPanel panel(fixture.renderSettings, fixture.componentLayout);
    {
        const std::vector<std::string> empty = labels(panel.tabs(fixture.context()));
        expect(empty == std::vector<std::string>({"Object", "World", "Render", "Scene"}),
               "with nothing selected only the Object and scene-wide tabs should be offered");
    }
    fixture.addSubject();
    const std::vector<PropertiesTab> tabs = panel.tabs(fixture.context());
    const std::vector<std::string> names = labels(tabs);
    expect(names.size() == 9 && names.front() == "Object" && names[names.size() - 3] == "World" &&
               names[names.size() - 2] == "Render" && names.back() == "Scene",
           "entity tabs should come first, then World, Render, and Scene");
    expect(std::ranges::find(names, "Gameplay") < std::ranges::find(names, "Other"),
           "module categories should follow registry order");

    // Every tab draws without input and without changing the scene.
    HeadlessImGuiContext imgui;
    for (const PropertiesTab& tab : tabs) {
        panel.selectTab(tab);
        expect(panel.activeTab(fixture.context()) == tab, "an available tab should become active when selected");
        fixture.draw(panel);
        fixture.draw(panel);
    }
    expect(fixture.history.size() == 0 && !fixture.diagnostics.hasUnacknowledgedError(),
           "drawing every tab should not record edits or report errors");

    // A remembered category tab falls back to Object while it is unavailable
    // and returns once an entity with the category is selected again.
    panel.selectTab({.kind = PropertiesTabKind::Components, .category = "Gameplay"});
    const SceneEntityId subject = *fixture.selection.active();
    fixture.selection.clear();
    expect(panel.activeTab(fixture.context()).kind == PropertiesTabKind::Object,
           "an unavailable tab should fall back to Object");
    fixture.draw(panel);
    fixture.selection.selectOnly(subject);
    expect(panel.activeTab(fixture.context()).category == "Gameplay",
           "the remembered tab should return when it is available again");

    // Clicking a tab in the strip selects it.
    fixture.draw(panel);
    const ImGuiWindow* strip = HeadlessImGuiContext::findWindow("##PropertiesTabs");
    expect(strip != nullptr, "the tab strip should be a child window");
    if (strip != nullptr) {
        const auto world = std::ranges::find(names, "World");
        // Selectable ids are pushed by index (PushID(int) hashes the integer).
        ImGuiID id = strip->ID;
        const int indexValue = static_cast<int>(world - names.begin());
        id = ImHashData(&indexValue, sizeof(indexValue), id);
        id = ImHashStr("World", 0, id);
        HeadlessImGuiContext::activateNextFrame(id);
        fixture.draw(panel);
        expect(panel.activeTab(fixture.context()).kind == PropertiesTabKind::World,
               "activating a strip entry should select its tab");
    }
}

// Activates the item, types text into it, and draws until the edit lands.
template <typename Panel> void typeInto(PanelFixture& fixture, Panel& panel, ImGuiID id, const char* text) {
    HeadlessImGuiContext::activateNextFrame(id);
    fixture.draw(panel);
    HeadlessImGuiContext::typeNextFrame(text);
    fixture.draw(panel);
}

void testEditsRecordUndo() {
    PanelFixture fixture;
    const SceneEntityId subject = fixture.addSubject();
    PropertiesPanel panel(fixture.renderSettings, fixture.componentLayout);
    HeadlessImGuiContext imgui;

    // Object tab: renaming records one undo step.
    panel.selectTab({.kind = PropertiesTabKind::Object});
    fixture.draw(panel);
    const ImGuiWindow* content = HeadlessImGuiContext::findWindow("##PropertiesContent");
    expect(content != nullptr, "the tab contents should be a child window");
    if (content == nullptr) {
        return;
    }
    typeInto(fixture, panel, HeadlessImGuiContext::itemId(*content, "Name"), "Hero");
    const auto name = [&] {
        return fixture.scene.registry().get<NameComponent>(fixture.scene.findEntity(subject)).name;
    };
    expect(name() == "Hero", "typing in the Properties name field should rename the entity");
    expect(fixture.history.size() == 1, "the rename should record one undo step");

    // A module category tab: editing a reflected field records one step that
    // undoes through SceneEditing.
    panel.selectTab({.kind = PropertiesTabKind::Components, .category = "Gameplay"});
    fixture.draw(panel);
    content = HeadlessImGuiContext::findWindow("##PropertiesContent");
    const auto health = [&] {
        return fixture.scene.registry().get<HealthComponent>(fixture.scene.findEntity(subject)).amount;
    };
    typeInto(fixture, panel, HeadlessImGuiContext::itemId(*content, "health", "amount", "Amount"), "42");
    expect(health() == 42.0f, "typing in a module component field should edit the component");
    expect(fixture.history.size() == 2, "the component edit should record one more undo step");
    fixture.editing().undo();
    expect(health() == 100.0f && name() == "Hero", "undo should revert only the component edit");
    fixture.editing().undo();
    expect(name() == "Subject", "undo should revert the rename");

    // The Inspector shares the view: the same edit records the same way.
    InspectorPanel inspector(fixture.componentLayout);
    fixture.draw(inspector);
    const ImGuiWindow* inspectorWindow = HeadlessImGuiContext::findWindow(inspector.title());
    expect(inspectorWindow != nullptr, "the Inspector should draw into its window");
    if (inspectorWindow != nullptr) {
        typeInto(fixture, inspector, HeadlessImGuiContext::itemId(*inspectorWindow, "Name"), "Villain");
        expect(name() == "Villain" && fixture.history.canUndo(), "the Inspector rename should record undo");
    }
}

void testWorldTab() {
    PanelFixture fixture;
    PropertiesPanel panel(fixture.renderSettings, fixture.componentLayout);
    HeadlessImGuiContext imgui;
    panel.selectTab({.kind = PropertiesTabKind::World});
    fixture.draw(panel);
    const ImGuiWindow* content = HeadlessImGuiContext::findWindow("##PropertiesContent");
    expect(content != nullptr, "the World tab should draw");
    if (content == nullptr) {
        return;
    }
    HeadlessImGuiContext::activateNextFrame(HeadlessImGuiContext::itemId(*content, "Add Fog"));
    fixture.draw(panel);
    const auto fogs = fixture.scene.registry().view<const VolumetricFogComponent>();
    expect(fogs.size() == 1 && fixture.history.size() == 1, "Add Fog should add fog as one undo step");
    expect(fixture.selection.size() == 1, "the added fog should be selected");
    fixture.editing().undo();
    expect(fixture.scene.registry().view<const VolumetricFogComponent>().empty(), "undo should remove the fog");

    HeadlessImGuiContext::activateNextFrame(HeadlessImGuiContext::itemId(*content, "Add Environment"));
    fixture.draw(panel);
    expect(fixture.scene.registry().view<const EnvironmentLightComponent>().size() == 1,
           "Add Environment should run the add operator");
    // The primary environment is now drawn and editable without selecting it.
    fixture.selection.clear();
    fixture.draw(panel);
    fixture.draw(panel);

    // A sun that is no longer primary stays in the tab instead of being
    // offered again as missing.
    HeadlessImGuiContext::activateNextFrame(HeadlessImGuiContext::itemId(*content, "Add Sun"));
    fixture.draw(panel);
    const auto suns = [&] { return fixture.scene.registry().view<DirectionalLightComponent>(); };
    expect(suns().size() == 1, "Add Sun should run the add operator");
    for (const Entity sun : suns()) {
        suns().get<DirectionalLightComponent>(sun).primary = false;
    }
    fixture.draw(panel);
    HeadlessImGuiContext::activateNextFrame(HeadlessImGuiContext::itemId(*content, "Add Sun"));
    fixture.draw(panel);
    expect(suns().size() == 1, "a non-primary sun should be shown rather than offering Add Sun");
    expect(!fixture.diagnostics.hasUnacknowledgedError(), "drawing the world components should not report errors");
}

void testRenderTab() {
    PanelFixture fixture;
    PropertiesPanel panel(fixture.renderSettings, fixture.componentLayout);
    GraphicsPanel graphics(fixture.renderSettings);
    HeadlessImGuiContext imgui;
    panel.selectTab({.kind = PropertiesTabKind::Render});
    fixture.draw(panel);
    const ImGuiWindow* content = HeadlessImGuiContext::findWindow("##PropertiesContent");
    expect(content != nullptr, "the Render tab should draw");
    if (content == nullptr) {
        return;
    }
    const bool before = fixture.renderSettings.settings().requested().dynamicResolution.enabled;
    HeadlessImGuiContext::activateNextFrame(HeadlessImGuiContext::itemId(*content, "Dynamic resolution"));
    fixture.draw(panel);
    expect(fixture.renderSettings.settings().requested().dynamicResolution.enabled == !before,
           "a Render tab edit should change the shared request");
    expect(fixture.rendering.settings().dynamicResolution.enabled == !before,
           "a Render tab edit should apply to the renderer live");
    expect(fixture.renderSettings.settings().unsaved(), "the edited request should be unsaved");
    // The Graphics panel draws the same request.
    fixture.draw(graphics);
    expect(fixture.renderSettings.settings().requested().dynamicResolution.enabled == !before,
           "drawing the Graphics panel should keep the shared request");
    expect(fixture.history.size() == 0, "render settings are not scene edits");
}

void testController() {
    RenderRuntime rendering;
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "zenith-render-controller-test.json";
    std::filesystem::remove(path);
    RenderSettingsController controller(EditorRenderSettings(ProjectConfig{}, path));
    expect(static_cast<bool>(controller.load(rendering)), "loading with nothing saved should succeed");
    RenderSettings edited = controller.settings().requested();
    edited.dynamicResolution.enabled = !edited.dynamicResolution.enabled;
    controller.request(edited, rendering);
    expect(rendering.settings().dynamicResolution.enabled == edited.dynamicResolution.enabled &&
               controller.settings().unsaved(),
           "a request should apply live and be unsaved");
    expect(static_cast<bool>(controller.load(rendering)) && !controller.settings().unsaved() &&
               rendering.settings().dynamicResolution.enabled != edited.dynamicResolution.enabled,
           "reverting should restore and apply the saved settings");
    std::filesystem::remove(path);
}

// The line of logged panel text that contains needle, or empty.
std::string lineContaining(const std::string& text, std::string_view needle) {
    const size_t found = text.find(needle);
    if (found == std::string::npos) {
        return {};
    }
    const size_t begin = text.rfind('\n', found);
    const size_t end = text.find('\n', found);
    return text.substr(begin == std::string::npos ? 0 : begin + 1,
                       end == std::string::npos ? std::string::npos
                                                : end - (begin == std::string::npos ? 0 : begin + 1));
}

// An entity with the module's Health component, and optionally a Marker.
SceneEntityId addHealthy(PanelFixture& fixture, const char* name, float amount, std::optional<bool> flagged) {
    const Entity entity = fixture.scene.createEntity(name);
    fixture.scene.registry().emplace<HealthComponent>(entity).amount = amount;
    if (flagged) {
        fixture.scene.registry().emplace<MarkerComponent>(entity).flagged = *flagged;
    }
    return fixture.scene.id(entity);
}

float healthOf(PanelFixture& fixture, SceneEntityId id) {
    return fixture.scene.registry().get<HealthComponent>(fixture.scene.findEntity(id)).amount;
}

ImGuiID healthAmountId(const char* windowName) {
    const ImGuiWindow* window = HeadlessImGuiContext::findWindow(windowName);
    expect(window != nullptr, "the panel window should exist");
    return window != nullptr ? HeadlessImGuiContext::itemId(*window, "health", "amount", "Amount") : 0;
}

void testMultiSelectionDisplay() {
    PanelFixture fixture;
    const SceneEntityId first = addHealthy(fixture, "First", 11.0f, true);
    const SceneEntityId second = addHealthy(fixture, "Second", 22.0f, false);
    componentTypesFor(fixture.document.options())
        .find("point_light")
        ->addDefault(fixture.scene, fixture.scene.findEntity(first));
    fixture.selection.assign({first, second});
    InspectorPanel inspector(fixture.componentLayout);
    HeadlessImGuiContext imgui;
    fixture.draw(inspector);
    const std::string text = fixture.drawLogged(inspector);
    expect(text.find("2 entities selected") != std::string::npos, "the Inspector should summarize the selection");
    expect(lineContaining(text, "Amount").find(mixedValueText) != std::string::npos &&
               text.find("11.000") == std::string::npos,
           "a shared field whose values differ should show the mixed mark instead of a value");
    expect(lineContaining(text, "Flagged").find("[~]") != std::string::npos,
           "a checkbox whose values differ should show ImGui's mixed state");
    const size_t heading = text.find("Active Entity Only");
    expect(heading != std::string::npos && text.find("Health") < heading && text.find("Point Light") > heading,
           "shared components should come first, then those only the active entity has");
    expect(fixture.history.size() == 0, "drawing a multi-selection should not record edits");
    // Remove acts on every selected entity with the component, so a section
    // only the active entity has does not offer it.
    expect(lineContaining(text, "Point Light").find("Remove") == std::string::npos &&
               lineContaining(text, "Health").find("Remove") != std::string::npos,
           "only sections the selection shares should offer Remove");

    // Equal values show the value itself.
    fixture.scene.registry().get<HealthComponent>(fixture.scene.findEntity(second)).amount = 11.0f;
    expect(lineContaining(fixture.drawLogged(inspector), "Amount").find("11.000") != std::string::npos,
           "a shared field with equal values should show the value");
}

void testMultiSelectionEdit() {
    PanelFixture fixture;
    const SceneEntityId first = addHealthy(fixture, "First", 10.0f, std::nullopt);
    const SceneEntityId second = addHealthy(fixture, "Second", 20.0f, std::nullopt);
    fixture.selection.assign({first, second});
    InspectorPanel inspector(fixture.componentLayout);
    HeadlessImGuiContext imgui;
    fixture.draw(inspector);
    typeInto(fixture, inspector, healthAmountId(inspector.title()), "42");
    expect(healthOf(fixture, first) == 42.0f && healthOf(fixture, second) == 42.0f,
           "an edit in a multi-selection should change every selected entity");
    expect(fixture.history.size() == 1, "the multi-entity edit should be one undo step");
    fixture.editing().undo();
    expect(healthOf(fixture, first) == 10.0f && healthOf(fixture, second) == 20.0f,
           "undo should restore every entity's own value");

    // The Properties panel's category tab edits the selection the same way.
    PropertiesPanel panel(fixture.renderSettings, fixture.componentLayout);
    panel.selectTab({.kind = PropertiesTabKind::Components, .category = "Gameplay"});
    expect(panel.activeTab(fixture.context()).category == "Gameplay",
           "a multi-selection should offer the active entity's category tabs");
    fixture.draw(panel);
    typeInto(fixture, panel, healthAmountId("##PropertiesContent"), "7");
    expect(healthOf(fixture, first) == 7.0f && healthOf(fixture, second) == 7.0f,
           "a Properties tab edit should change every selected entity");
}

void testAltAppliesToSelected() {
    PanelFixture fixture;
    const SceneEntityId first = addHealthy(fixture, "First", 10.0f, std::nullopt);
    const SceneEntityId second = addHealthy(fixture, "Second", 20.0f, std::nullopt);
    const SceneEntityId bare = fixture.scene.id(fixture.scene.createEntity("Bare"));
    // Health is not shared by the whole selection, so it is shown for the
    // active entity only.
    fixture.selection.assign({first, second, bare});
    InspectorPanel inspector(fixture.componentLayout);
    {
        HeadlessImGuiContext imgui;
        fixture.draw(inspector);
        typeInto(fixture, inspector, healthAmountId(inspector.title()), "42");
        expect(healthOf(fixture, first) == 42.0f && healthOf(fixture, second) == 20.0f,
               "without Alt an active-only edit should change only the active entity");
        fixture.editing().undo();
    }

    // A fresh context, so the field from the first edit is no longer active.
    HeadlessImGuiContext imgui;
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Alt, true);
    fixture.draw(inspector);
    typeInto(fixture, inspector, healthAmountId(inspector.title()), "55");
    io.AddKeyEvent(ImGuiMod_Alt, false);
    fixture.draw(inspector);
    expect(healthOf(fixture, first) == 55.0f && healthOf(fixture, second) == 55.0f,
           "Alt should apply the edit to every selected entity with the component");
    expect(!fixture.scene.registry().all_of<HealthComponent>(fixture.scene.findEntity(bare)),
           "Alt should not add the component to entities without it");
    fixture.editing().undo();
    expect(healthOf(fixture, first) == 10.0f && healthOf(fixture, second) == 20.0f,
           "undo should restore every entity the Alt edit changed");
}

// Opens the component menu of the module's Health section, then activates
// the menu item labelled item.
void runHealthMenuItem(PanelFixture& fixture, InspectorPanel& inspector, const char* item) {
    const ImGuiWindow* window = HeadlessImGuiContext::findWindow(inspector.title());
    expect(window != nullptr, "the Inspector window should exist");
    if (window == nullptr) {
        return;
    }
    HeadlessImGuiContext::activateNextFrame(HeadlessImGuiContext::itemId(*window, "health", "..."));
    fixture.draw(inspector);
    fixture.draw(inspector);
    const ImGuiWindow* popup = HeadlessImGuiContext::findWindow("##Popup_");
    expect(popup != nullptr && popup->Active, "the component menu should open");
    if (popup == nullptr) {
        return;
    }
    HeadlessImGuiContext::activateNextFrame(HeadlessImGuiContext::itemId(*popup, item));
    fixture.draw(inspector);
}

void testComponentMenu() {
    PanelFixture fixture;
    const SceneEntityId first = addHealthy(fixture, "First", 10.0f, false);
    const SceneEntityId second = addHealthy(fixture, "Second", 20.0f, false);
    fixture.selection.assign({first, second});
    InspectorPanel inspector(fixture.componentLayout);
    HeadlessImGuiContext imgui;
    fixture.draw(inspector);

    runHealthMenuItem(fixture, inspector, "Reset to Default");
    expect(healthOf(fixture, first) == 100.0f && healthOf(fixture, second) == 100.0f,
           "Reset to Default should reset every selected entity's component");
    expect(fixture.history.size() == 1, "the menu's reset should be one undo step");

    fixture.scene.registry().get<HealthComponent>(fixture.scene.findEntity(first)).amount = 64.0f;
    runHealthMenuItem(fixture, inspector, "Copy Values");
    expect(fixture.clipboard.text().find("\"health\"") != std::string::npos,
           "Copy Values should put the active entity's values on the clipboard");
    runHealthMenuItem(fixture, inspector, "Paste Values");
    expect(healthOf(fixture, second) == 64.0f, "Paste Values should apply the copied values to the selection");

    // Health is listed before Marker; moving Marker to the top reorders the
    // sections for every entity and is remembered by the layout.
    const ComponentTypeRegistry& types = componentTypesFor(fixture.document.options());
    expect(displayedComponentKeys(types, fixture.componentLayout, fixture.scene, first).front() == "health",
           "sections should start in registry order");
    runHealthMenuItem(fixture, inspector, "Move Down");
    expect(displayedComponentKeys(types, fixture.componentLayout, fixture.scene, first).front() == "marker",
           "Move Down should reorder the sections");
    expect(fixture.componentLayout.takeChanged(), "the move should be a layout change to save");
}

void testCollapseState() {
    PanelFixture fixture;
    const SceneEntityId subject = addHealthy(fixture, "Subject", 10.0f, std::nullopt);
    fixture.selection.selectOnly(subject);
    InspectorPanel inspector(fixture.componentLayout);
    HeadlessImGuiContext imgui;
    fixture.draw(inspector);
    const ImGuiWindow* window = HeadlessImGuiContext::findWindow(inspector.title());
    expect(window != nullptr, "the Inspector window should exist");
    if (window == nullptr) {
        return;
    }
    expect(!fixture.componentLayout.collapsed("health"), "sections start expanded");
    HeadlessImGuiContext::activateNextFrame(HeadlessImGuiContext::itemId(*window, "health", "Health"));
    fixture.draw(inspector);
    fixture.draw(inspector);
    expect(fixture.componentLayout.collapsed("health") && fixture.componentLayout.takeChanged(),
           "collapsing a section should be remembered per component type");
    expect(fixture.drawLogged(inspector).find("Amount") == std::string::npos, "a collapsed section should stay closed");

    // Another Inspector (or a restarted editor with the saved layout) opens
    // the type collapsed.
    InspectorPanel another(fixture.componentLayout);
    expect(fixture.drawLogged(another).find("Amount") == std::string::npos,
           "the collapse state should be shared by every inspector");
}

// An entity at position, rotated 45 degrees about Y when turned.
SceneEntityId addPlaced(PanelFixture& fixture, const char* name, glm::vec3 position, bool turned) {
    const Entity entity = fixture.scene.createEntity(name);
    EditableTransform transform{.translation = position};
    transform.rotationDegrees.y = turned ? 45.0f : 0.0f;
    fixture.scene.registry().get<TransformComponent>(entity).local = ComponentEditors::composeTransform(transform);
    return fixture.scene.id(entity);
}

EditableTransform transformOf(PanelFixture& fixture, SceneEntityId id) {
    EditableTransform transform;
    ComponentEditors::decomposeTransform(
        fixture.scene.registry().get<TransformComponent>(fixture.scene.findEntity(id)).local, transform);
    return transform;
}

bool near(glm::vec3 actual, glm::vec3 expected) {
    return glm::all(glm::lessThan(glm::abs(actual - expected), glm::vec3(0.001f)));
}

std::string nameOf(PanelFixture& fixture, SceneEntityId id) {
    return fixture.scene.registry().get<NameComponent>(fixture.scene.findEntity(id)).name;
}

// The id of element index of a reflected vector field drawn in window, as
// drawReflectedFields lays its elements out.
ImGuiID elementId(const char* windowName, const char* fieldKey, const char* label, int index) {
    const ImGuiWindow* window = HeadlessImGuiContext::findWindow(windowName);
    expect(window != nullptr, "the panel window should exist");
    if (window == nullptr) {
        return 0;
    }
    const ImGuiID field = HeadlessImGuiContext::itemId(*window, fieldKey, label);
    return ImHashStr("", 0, ImHashData(&index, sizeof(index), field));
}

// Presses and releases Enter, which commits a field being typed into.
template <typename Panel> void pressEnter(PanelFixture& fixture, Panel& panel) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiKey_Enter, true);
    fixture.draw(panel);
    io.AddKeyEvent(ImGuiKey_Enter, false);
    fixture.draw(panel);
    fixture.draw(panel);
}

void testMultiSelectionNameAndTransform() {
    PanelFixture fixture;
    const SceneEntityId first = addPlaced(fixture, "First", {1.0f, 2.0f, 3.0f}, false);
    const SceneEntityId second = addPlaced(fixture, "Second", {4.0f, 2.0f, 6.0f}, true);
    fixture.selection.assign({first, second});
    InspectorPanel inspector(fixture.componentLayout);
    HeadlessImGuiContext imgui;
    fixture.draw(inspector);
    const std::string text = fixture.drawLogged(inspector);
    expect(lineContaining(text, "Name").find(mixedValueText) != std::string::npos &&
               text.find("First") == std::string::npos,
           "names that differ should show the mixed mark");
    const std::string position = lineContaining(text, "Position");
    expect(position.find(mixedValueText) != std::string::npos && position.find("2.000") != std::string::npos &&
               position.find("1.000") == std::string::npos,
           "an axis that differs should show the mixed mark and an equal axis its value");
    expect(lineContaining(text, "Scale").find(mixedValueText) == std::string::npos,
           "equal scales should show their values");

    // Renaming names every selected entity, as one step.
    const ImGuiWindow* window = HeadlessImGuiContext::findWindow(inspector.title());
    if (window == nullptr) {
        expect(false, "the Inspector window should exist");
        return;
    }
    typeInto(fixture, inspector, HeadlessImGuiContext::itemId(*window, "Name"), "Crate");
    expect(nameOf(fixture, first) == "Crate" && nameOf(fixture, second) == "Crate",
           "typing a name should rename every selected entity");
    expect(fixture.history.size() == 1, "renaming the selection should be one undo step");

    // An edit of one axis changes only that axis, on every entity.
    typeInto(fixture, inspector, elementId(inspector.title(), "position", "Position", 1), "5");
    expect(near(transformOf(fixture, first).translation, {1.0f, 5.0f, 3.0f}) &&
               near(transformOf(fixture, second).translation, {4.0f, 5.0f, 6.0f}),
           "editing one axis should set it on every entity and keep their other axes");
    expect(near(transformOf(fixture, second).rotationDegrees, {0.0f, 45.0f, 0.0f}),
           "a position edit should keep each entity's rotation");

    // Typing the shown entity's own value into a mixed axis still applies it.
    typeInto(fixture, inspector, elementId(inspector.title(), "position", "Position", 0), "1");
    pressEnter(fixture, inspector);
    expect(transformOf(fixture, first).translation.x == 1.0f && transformOf(fixture, second).translation.x == 1.0f,
           "committing the shown value of a mixed axis should apply it to every entity");
    expect(fixture.history.size() == 2, "the transform edits should merge into one undo step after the rename");
    fixture.editing().undo();
    expect(near(transformOf(fixture, first).translation, {1.0f, 2.0f, 3.0f}) &&
               near(transformOf(fixture, second).translation, {4.0f, 2.0f, 6.0f}) && nameOf(fixture, first) == "Crate",
           "undo should restore every entity's own transform and keep the rename");

    // Alt applies to the whole selection here too (every entity has a
    // transform), without slowing the drag.
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Alt, true);
    fixture.draw(inspector);
    typeInto(fixture, inspector, elementId(inspector.title(), "scale", "Scale", 2), "2");
    io.AddKeyEvent(ImGuiMod_Alt, false);
    fixture.draw(inspector);
    expect(near(transformOf(fixture, first).scale, {1.0f, 1.0f, 2.0f}) &&
               near(transformOf(fixture, second).scale, {1.0f, 1.0f, 2.0f}),
           "an Alt edit of the transform should apply to every selected entity");

    // The Properties Object tab offers the same fields.
    PropertiesPanel panel(fixture.renderSettings, fixture.componentLayout);
    panel.selectTab({.kind = PropertiesTabKind::Object});
    fixture.draw(panel);
    const std::string objectTab = fixture.drawLogged(panel);
    expect(objectTab.find("Crate") != std::string::npos && objectTab.find("Position") != std::string::npos,
           "the Object tab should show the selection's name and transform");
}

void testMixedFieldCommitsTheShownValue() {
    PanelFixture fixture;
    const SceneEntityId first = addHealthy(fixture, "First", 10.0f, std::nullopt);
    const SceneEntityId second = addHealthy(fixture, "Second", 20.0f, std::nullopt);
    fixture.selection.assign({first, second});
    InspectorPanel inspector(fixture.componentLayout);
    HeadlessImGuiContext imgui;
    fixture.draw(inspector);
    typeInto(fixture, inspector, healthAmountId(inspector.title()), "10");
    pressEnter(fixture, inspector);
    expect(healthOf(fixture, first) == 10.0f && healthOf(fixture, second) == 10.0f,
           "committing the active entity's own value in a mixed field should apply it to every entity");
    expect(fixture.history.size() == 1, "the committed value should be one undo step");
}

void testTransformEditKeepsUntouchedAxes() {
    PanelFixture fixture;
    const SceneEntityId first = addPlaced(fixture, "First", {0.0f, 0.1f, 0.0f}, false);
    const SceneEntityId second = addPlaced(fixture, "Second", {1.0f, 0.10008f, 0.0f}, false);
    fixture.selection.assign({first, second});
    InspectorPanel inspector(fixture.componentLayout);
    HeadlessImGuiContext imgui;
    fixture.draw(inspector);
    const float before = transformOf(fixture, second).translation.y;
    typeInto(fixture, inspector, elementId(inspector.title(), "position", "Position", 0), "7");
    expect(transformOf(fixture, first).translation.x == 7.0f && transformOf(fixture, second).translation.x == 7.0f,
           "the edited axis should change on every entity");
    expect(std::abs(transformOf(fixture, second).translation.y - before) < 1.0e-6f,
           "an axis within display tolerance of the shown entity's should keep its own value when another is edited");
}
} // namespace

int main() {
    testCategories();
    testTabs();
    testEditsRecordUndo();
    testWorldTab();
    testRenderTab();
    testController();
    testMultiSelectionDisplay();
    testMultiSelectionEdit();
    testAltAppliesToSelected();
    testComponentMenu();
    testCollapseState();
    testMultiSelectionNameAndTransform();
    testMixedFieldCommitsTheShownValue();
    testTransformEditKeepsUntouchedAxes();
    if (failures != 0) {
        std::cerr << failures << " properties panel test(s) failed\n";
        return 1;
    }
    return 0;
}
