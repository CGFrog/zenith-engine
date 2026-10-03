#include "HeadlessImGui.hpp"
#include "engine/diagnostics/Logger.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/document/CommandHistory.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/interaction/EditorCamera.hpp"
#include "editor/interaction/EntityPropertiesView.hpp"
#include "editor/operators/ObjectOperators.hpp"
#include "editor/operators/OperatorRegistry.hpp"
#include "editor/operators/OperatorRunner.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/panels/GraphicsPanel.hpp"
#include "editor/panels/InspectorPanel.hpp"
#include "editor/panels/PropertiesPanel.hpp"
#include "editor/settings/RenderSettingsController.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/audio/AudioSystem.hpp"
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

    void updateModalTransform(ApplicationContext&) override {}

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
    EditorCamera camera;
    OperatorRegistry operators;
    OperatorRunner runner{operators, history};
    OperatorActions actions{runner, [this] { return operatorContext(); }};
    RenderSettingsController renderSettings{EditorRenderSettings(
        ProjectConfig{}, std::filesystem::temp_directory_path() / "zenith-properties-panel-render.json")};

    PanelFixture() {
        expect(registerObjectOperators(operators).success, "the object operators should register");
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
                .camera = camera,
                .mode = EditorMode::Edit,
                .actions = actions};
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
    PropertiesPanel panel(fixture.renderSettings);
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
    PropertiesPanel panel(fixture.renderSettings);
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
    InspectorPanel inspector;
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
    PropertiesPanel panel(fixture.renderSettings);
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
    PropertiesPanel panel(fixture.renderSettings);
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
} // namespace

int main() {
    testCategories();
    testTabs();
    testEditsRecordUndo();
    testWorldTab();
    testRenderTab();
    testController();
    if (failures != 0) {
        std::cerr << failures << " properties panel test(s) failed\n";
        return 1;
    }
    return 0;
}
