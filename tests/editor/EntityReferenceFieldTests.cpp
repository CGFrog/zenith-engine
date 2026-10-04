#include "EditorTestFixture.hpp"
#include "HeadlessImGui.hpp"

#include "editor/interaction/EntityPropertiesView.hpp"
#include "editor/interaction/EntityReferenceField.hpp"
#include "editor/operators/EyedropperOperators.hpp"
#include "editor/panels/HierarchyPanel.hpp"
#include "engine/scene/ComponentTypes.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Entity reference fields in the editor: the widget's name, Clear, and
// outliner drop; the eyedropper armed from the widget and filled by an
// outliner click as one undo step without changing the selection; and the
// eyedropper operators' failure and cancel paths.

namespace {
int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

constexpr const char* propertiesWindow = "Entity reference test";

// A game module component referring to other entities, one reference nested
// in a struct.
struct FollowSettings {
    SceneEntityId backup;
};

struct FollowComponent {
    SceneEntityId target;
    FollowSettings settings;
};

std::shared_ptr<const ComponentTypeRegistry> followComponentTypes() {
    auto types = std::make_shared<ComponentTypeRegistry>();
    expect(registerBuiltInComponentTypes(*types).success, "built-in component types should register");
    expect(types
               ->add(ComponentTypeBuilder<FollowComponent>("follow", "Follow")
                         .category("Gameplay")
                         .fields(
                             std::move(Fields<FollowComponent>()
                                           .add("target", &FollowComponent::target)
                                           .structure("settings", &FollowComponent::settings,
                                                      Fields<FollowSettings>().add("backup", &FollowSettings::backup))))
                         .build())
               .success,
           "a component with entity references should register");
    return types;
}

struct Fixture : EditorTestFixture {
    EntityPropertiesView view{componentLayout};
    HierarchyPanel hierarchy;
    SceneEntityId owner;
    SceneEntityId chased;

    Fixture() {
        expect(registerEyedropperOperators(operators, eyedropper).success, "the eyedropper operators should register");
        document.setComponentTypes(followComponentTypes());
        const Entity ownerEntity = scene.createEntity("Hunter");
        componentTypesFor(document.options()).find("follow")->addDefault(scene, ownerEntity);
        owner = scene.id(ownerEntity);
        chased = scene.id(scene.createEntity("Prey"));
        selection.selectOnly(owner);
    }

    FollowComponent& follow() {
        return scene.registry().get<FollowComponent>(scene.findEntity(owner));
    }

    // Draws the owner's Follow component; before runs first inside the window.
    void drawProperties(const std::function<void()>& before = {}) {
        HeadlessImGuiFrame frame(propertiesWindow);
        if (before) {
            before();
        }
        EditorContext editor = context();
        view.drawComponent(editor, *componentTypesFor(document.options()).find("follow"), std::span(&owner, 1), {});
    }

    void drawHierarchy() {
        HeadlessImGuiFrame frame(hierarchy.title());
        EditorContext editor = context();
        hierarchy.draw(editor);
    }

    OperatorResult invoke(std::string_view operatorId, const nlohmann::json& properties) {
        return EditorTestFixture::invoke(operatorId, properties.dump());
    }
};

ImGuiID propertiesItem(const char* field, const char* item) {
    const ImGuiWindow* window = HeadlessImGuiContext::findWindow(propertiesWindow);
    return window == nullptr ? 0 : HeadlessImGuiContext::itemId(*window, "follow", field, item);
}

// Moves the mouse down the screen at x until the item with id is hovered
// after a frame of draw; returns whether it was found.
bool hoverItem(ImGuiID id, float x, const std::function<void()>& draw) {
    ImGuiIO& io = ImGui::GetIO();
    for (float y = 0.0f; y < io.DisplaySize.y; y += 2.0f) {
        io.AddMousePosEvent(x, y);
        draw();
        if (ImGui::GetCurrentContext()->HoveredId == id) {
            return true;
        }
    }
    return false;
}

void click(const std::function<void()>& draw) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    draw();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    draw();
}

void testReferenceText() {
    Fixture fixture;
    expect(entityReferenceText(fixture.scene, {}) == "None", "an empty reference should read None");
    expect(entityReferenceText(fixture.scene, fixture.chased) == "Prey", "a reference should name its entity");
    expect(entityReferenceText(fixture.scene, SceneEntityId{424242}) == "Missing",
           "a reference to no entity should read Missing");
    expect(entityReferenceState(fixture.scene, SceneEntityId{424242}) == EntityReferenceState::Missing,
           "a reference to no entity should be missing");
}

void testFieldPaths() {
    const std::shared_ptr<const ComponentTypeRegistry> types = followComponentTypes();
    const std::span<const FieldDescriptor> fields = types->find("follow")->fields();
    const std::optional<std::vector<std::string>> nested = findFieldPath(fields, fields[1].children[0]);
    expect(nested && *nested == std::vector<std::string>{"settings", "backup"},
           "a nested field's path should run through its struct");
    const FieldDescriptor copy = fields[0];
    expect(!findFieldPath(fields, copy), "a copied descriptor is not part of the tree");
    expect(joinFieldPath(*nested) == "settings.backup" && splitFieldPath("settings.backup") == *nested,
           "field paths should join and split with dots");
    expect(splitFieldPath("").empty() && splitFieldPath("a..b").empty() && splitFieldPath("a.").empty(),
           "paths with empty keys should be rejected");
}

void testClear() {
    Fixture fixture;
    fixture.follow().target = fixture.chased;
    HeadlessImGuiContext imgui;
    fixture.drawProperties();
    HeadlessImGuiContext::activateNextFrame(propertiesItem("target", "Clear"));
    fixture.drawProperties();
    expect(!fixture.follow().target && fixture.history.size() == 1, "Clear should empty the field as one edit");
    fixture.editing().undo();
    expect(fixture.follow().target == fixture.chased, "undo should restore the cleared reference");
}

void testDrop() {
    Fixture fixture;
    HeadlessImGuiContext imgui;
    ImGui::GetIO().ConfigInputTrickleEventQueue = false;
    const auto draw = [&fixture] { fixture.drawProperties(); };
    draw();
    expect(hoverItem(propertiesItem("target", "##value"), 120.0f, draw), "the value box should be hoverable");

    // An extern drag source stands in for the outliner row, carrying the same
    // payload: the target accepts it on the first frame and receives it on
    // the next, once the source is no longer submitted.
    const uint64_t dragged = fixture.chased.value;
    fixture.drawProperties([dragged] {
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceExtern | ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
            ImGui::SetDragDropPayload(sceneEntityDragPayload, &dragged, sizeof(dragged));
            ImGui::EndDragDropSource();
        }
    });
    draw();
    expect(fixture.follow().target == fixture.chased && fixture.history.size() == 1,
           "dropping an outliner entity should set the field as one edit");
    fixture.editing().undo();
    expect(!fixture.follow().target, "undo should restore the field before the drop");
}

void testEyedropperFromWidgetAndOutliner() {
    Fixture fixture;
    HeadlessImGuiContext imgui;
    ImGui::GetIO().ConfigInputTrickleEventQueue = false;
    fixture.drawProperties();

    HeadlessImGuiContext::activateNextFrame(propertiesItem("target", "Pick"));
    fixture.drawProperties();
    const EntityReferenceTarget expected{.entity = fixture.owner, .componentKey = "follow", .fieldPath = {"target"}};
    expect(fixture.eyedropper.target() != nullptr && *fixture.eyedropper.target() == expected,
           "Pick should arm the eyedropper for its field");

    // Pick again cancels.
    fixture.drawProperties();
    HeadlessImGuiContext::activateNextFrame(propertiesItem("target", "Pick"));
    fixture.drawProperties();
    expect(!fixture.eyedropper.armed(), "Pick should cancel the eyedropper it armed");
    HeadlessImGuiContext::activateNextFrame(propertiesItem("target", "Pick"));
    fixture.drawProperties();

    // An outliner click fills the field instead of selecting.
    const auto draw = [&fixture] { fixture.drawHierarchy(); };
    fixture.drawHierarchy();
    const ImGuiWindow* window = HeadlessImGuiContext::findWindow(fixture.hierarchy.title());
    expect(window != nullptr, "the hierarchy should draw a window");
    if (window == nullptr) {
        return;
    }
    const void* rowId = reinterpret_cast<void*>(static_cast<uintptr_t>(fixture.chased.value));
    const ImGuiID row = ImHashData(&rowId, sizeof(rowId), window->ID);
    expect(hoverItem(row, 200.0f, draw), "the prey's outliner row should be hoverable");
    click(draw);
    expect(fixture.follow().target == fixture.chased, "an outliner click should fill the armed field");
    expect(!fixture.eyedropper.armed(), "the eyedropper should disarm after a pick");
    expect(fixture.selection.size() == 1 && fixture.selection.contains(fixture.owner),
           "an eyedropper pick should leave the selection alone");
    expect(fixture.history.size() == 1, "the pick should be one undo step");

    // With the eyedropper idle, the same click selects again.
    click(draw);
    expect(fixture.selection.contains(fixture.chased), "an outliner click should select when nothing is picking");

    fixture.editing().undo();
    expect(!fixture.follow().target, "undo should restore the field before the pick");
}

void testEyedropperOperators() {
    Fixture fixture;
    expect(!fixture.invoke("ui.eyedropper_entity",
                           {{"entity", fixture.owner.value}, {"component", "follow"}, {"field", "speed"}}),
           "arming for a field that is not an entity reference should fail");
    expect(!fixture.invoke("ui.eyedropper_entity", {{"entity", 999}, {"component", "follow"}, {"field", "target"}}),
           "arming for a missing entity should fail");
    expect(!fixture.eyedropper.armed(), "failed arming should leave the eyedropper idle");

    // A nested field, filled through the pick operator a viewport click runs,
    // after the registry was replaced as a module reload does.
    const nlohmann::json nested{{"entity", fixture.owner.value}, {"component", "follow"}, {"field", "settings.backup"}};
    expect(static_cast<bool>(fixture.invoke("ui.eyedropper_entity", nested)), "arming a nested field should work");
    fixture.document.setComponentTypes(followComponentTypes());
    {
        EditorContext context = fixture.context();
        context.pickEyedropperEntity(fixture.chased);
    }
    expect(fixture.follow().settings.backup == fixture.chased && fixture.history.size() == 1,
           "a pick should fill a nested field as one step");

    expect(static_cast<bool>(fixture.invoke("ui.eyedropper_entity", nested)), "arming again should work");
    expect(static_cast<bool>(fixture.invoke("ui.eyedropper_cancel", nlohmann::json::object())),
           "cancel should run while armed");
    expect(!fixture.eyedropper.armed() &&
               !fixture.operators.find("ui.eyedropper_cancel")->canRun(fixture.operatorContext()),
           "cancel should disarm and then be unavailable");

    // The owner loses the component while armed: the pick fails, changes
    // nothing, and disarms.
    expect(static_cast<bool>(fixture.invoke("ui.eyedropper_entity", nested)), "arming should work");
    componentTypesFor(fixture.document.options())
        .find("follow")
        ->remove(fixture.scene, fixture.scene.findEntity(fixture.owner));
    const OperatorResult orphaned = fixture.invoke("ui.eyedropper_pick", {{"entity", fixture.chased.value}});
    expect(!orphaned && orphaned.message.find("Follow") != std::string::npos,
           "a pick for a removed component should fail naming it: " + orphaned.message);
    expect(!fixture.eyedropper.armed() && fixture.history.size() == 1,
           "a failed pick should disarm and record nothing");
    expect(!fixture.invoke("ui.eyedropper_pick", {{"entity", fixture.chased.value}}),
           "a pick without an armed eyedropper should not run");
}
} // namespace

int main() {
    testReferenceText();
    testFieldPaths();
    testClear();
    testDrop();
    testEyedropperFromWidgetAndOutliner();
    testEyedropperOperators();
    if (failures != 0) {
        std::cerr << failures << " entity reference field test(s) failed\n";
        return 1;
    }
    std::cout << "Entity reference field tests passed\n";
    return 0;
}
