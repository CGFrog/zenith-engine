#include "EditorTestFixture.hpp"
#include "HeadlessImGui.hpp"

#include "editor/interaction/EntityPropertiesView.hpp"
#include "editor/interaction/EntityReferenceField.hpp"
#include "editor/operators/EyedropperOperators.hpp"
#include "editor/panels/HierarchyPanel.hpp"
#include "engine/scene/ComponentTypes.hpp"

#include "support/Doctest.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

TEST_SUITE_BEGIN("EntityReferenceField");

// Entity reference fields in the editor: the widget's name, Clear, and
// outliner drop; the eyedropper armed from the widget and filled by an
// outliner click as one undo step without changing the selection; and the
// eyedropper operators' failure and cancel paths.

namespace {

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
    CHECK_MESSAGE(registerBuiltInComponentTypes(*types).success, "built-in component types should register");
    CHECK_MESSAGE(types
                      ->add(ComponentTypeBuilder<FollowComponent>("follow", "Follow")
                                .category("Gameplay")
                                .fields(std::move(
                                    Fields<FollowComponent>()
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
        CHECK_MESSAGE(registerEyedropperOperators(operators, eyedropper).success,
                      "the eyedropper operators should register");
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

TEST_CASE("EntityReferenceField: reference text") {
    Fixture fixture;
    CHECK_MESSAGE(entityReferenceText(fixture.scene, {}) == "None", "an empty reference should read None");
    CHECK_MESSAGE(entityReferenceText(fixture.scene, fixture.chased) == "Prey", "a reference should name its entity");
    CHECK_MESSAGE(entityReferenceText(fixture.scene, SceneEntityId{424242}) == "Missing",
                  "a reference to no entity should read Missing");
    CHECK_MESSAGE(entityReferenceState(fixture.scene, SceneEntityId{424242}) == EntityReferenceState::Missing,
                  "a reference to no entity should be missing");
}

TEST_CASE("EntityReferenceField: field paths") {
    const std::shared_ptr<const ComponentTypeRegistry> types = followComponentTypes();
    const std::span<const FieldDescriptor> fields = types->find("follow")->fields();
    const std::optional<std::vector<std::string>> nested = findFieldPath(fields, fields[1].children[0]);
    CHECK_MESSAGE((nested && *nested == std::vector<std::string>{"settings", "backup"}),
                  "a nested field's path should run through its struct");
    const FieldDescriptor copy = fields[0];
    CHECK_MESSAGE(!findFieldPath(fields, copy), "a copied descriptor is not part of the tree");
    CHECK_MESSAGE((joinFieldPath(*nested) == "settings.backup" && splitFieldPath("settings.backup") == *nested),
                  "field paths should join and split with dots");
    CHECK_MESSAGE((splitFieldPath("").empty() && splitFieldPath("a..b").empty() && splitFieldPath("a.").empty()),
                  "paths with empty keys should be rejected");
}

TEST_CASE("EntityReferenceField: clear") {
    Fixture fixture;
    fixture.follow().target = fixture.chased;
    HeadlessImGuiContext imgui;
    fixture.drawProperties();
    HeadlessImGuiContext::activateNextFrame(propertiesItem("target", "Clear"));
    fixture.drawProperties();
    CHECK_MESSAGE((!fixture.follow().target && fixture.history.size() == 1),
                  "Clear should empty the field as one edit");
    fixture.editing().undo();
    CHECK_MESSAGE(fixture.follow().target == fixture.chased, "undo should restore the cleared reference");
}

TEST_CASE("EntityReferenceField: drop") {
    Fixture fixture;
    HeadlessImGuiContext imgui;
    ImGui::GetIO().ConfigInputTrickleEventQueue = false;
    const auto draw = [&fixture] { fixture.drawProperties(); };
    draw();
    CHECK_MESSAGE(hoverItem(propertiesItem("target", "##value"), 120.0f, draw), "the value box should be hoverable");

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
    CHECK_MESSAGE((fixture.follow().target == fixture.chased && fixture.history.size() == 1),
                  "dropping an outliner entity should set the field as one edit");
    fixture.editing().undo();
    CHECK_MESSAGE(!fixture.follow().target, "undo should restore the field before the drop");
}

TEST_CASE("EntityReferenceField: eyedropper from widget and outliner") {
    Fixture fixture;
    HeadlessImGuiContext imgui;
    ImGui::GetIO().ConfigInputTrickleEventQueue = false;
    fixture.drawProperties();

    HeadlessImGuiContext::activateNextFrame(propertiesItem("target", "Pick"));
    fixture.drawProperties();
    const EntityReferenceTarget expected{.entity = fixture.owner, .componentKey = "follow", .fieldPath = {"target"}};
    CHECK_MESSAGE((fixture.eyedropper.target() != nullptr && *fixture.eyedropper.target() == expected),
                  "Pick should arm the eyedropper for its field");

    // Pick again cancels.
    fixture.drawProperties();
    HeadlessImGuiContext::activateNextFrame(propertiesItem("target", "Pick"));
    fixture.drawProperties();
    CHECK_MESSAGE(!fixture.eyedropper.armed(), "Pick should cancel the eyedropper it armed");
    HeadlessImGuiContext::activateNextFrame(propertiesItem("target", "Pick"));
    fixture.drawProperties();

    // An outliner click fills the field instead of selecting.
    const auto draw = [&fixture] { fixture.drawHierarchy(); };
    fixture.drawHierarchy();
    const ImGuiWindow* window = HeadlessImGuiContext::findWindow(fixture.hierarchy.title());
    CHECK_MESSAGE(window != nullptr, "the hierarchy should draw a window");
    if (window == nullptr) {
        return;
    }
    const void* rowId = reinterpret_cast<void*>(static_cast<uintptr_t>(fixture.chased.value));
    const ImGuiID row = ImHashData(&rowId, sizeof(rowId), window->ID);
    CHECK_MESSAGE(hoverItem(row, 200.0f, draw), "the prey's outliner row should be hoverable");
    click(draw);
    CHECK_MESSAGE(fixture.follow().target == fixture.chased, "an outliner click should fill the armed field");
    CHECK_MESSAGE(!fixture.eyedropper.armed(), "the eyedropper should disarm after a pick");
    CHECK_MESSAGE((fixture.selection.size() == 1 && fixture.selection.contains(fixture.owner)),
                  "an eyedropper pick should leave the selection alone");
    CHECK_MESSAGE(fixture.history.size() == 1, "the pick should be one undo step");

    // With the eyedropper idle, the same click selects again.
    click(draw);
    CHECK_MESSAGE(fixture.selection.contains(fixture.chased),
                  "an outliner click should select when nothing is picking");

    fixture.editing().undo();
    CHECK_MESSAGE(!fixture.follow().target, "undo should restore the field before the pick");
}

TEST_CASE("EntityReferenceField: eyedropper operators") {
    Fixture fixture;
    CHECK_MESSAGE(!fixture.invoke("ui.eyedropper_entity",
                                  {{"entity", fixture.owner.value}, {"component", "follow"}, {"field", "speed"}}),
                  "arming for a field that is not an entity reference should fail");
    CHECK_MESSAGE(
        !fixture.invoke("ui.eyedropper_entity", {{"entity", 999}, {"component", "follow"}, {"field", "target"}}),
        "arming for a missing entity should fail");
    CHECK_MESSAGE(!fixture.eyedropper.armed(), "failed arming should leave the eyedropper idle");

    // A nested field, filled through the pick operator a viewport click runs,
    // after the registry was replaced as a module reload does.
    const nlohmann::json nested{{"entity", fixture.owner.value}, {"component", "follow"}, {"field", "settings.backup"}};
    CHECK_MESSAGE((static_cast<bool>(fixture.invoke("ui.eyedropper_entity", nested))),
                  "arming a nested field should work");
    fixture.document.setComponentTypes(followComponentTypes());
    {
        EditorContext context = fixture.context();
        context.pickEyedropperEntity(fixture.chased);
    }
    CHECK_MESSAGE((fixture.follow().settings.backup == fixture.chased && fixture.history.size() == 1),
                  "a pick should fill a nested field as one step");

    CHECK_MESSAGE((static_cast<bool>(fixture.invoke("ui.eyedropper_entity", nested))), "arming again should work");
    CHECK_MESSAGE((static_cast<bool>(fixture.invoke("ui.eyedropper_cancel", nlohmann::json::object()))),
                  "cancel should run while armed");
    CHECK_MESSAGE((!fixture.eyedropper.armed() &&
                   !fixture.operators.find("ui.eyedropper_cancel")->canRun(fixture.operatorContext())),
                  "cancel should disarm and then be unavailable");

    // The owner loses the component while armed: the pick fails, changes
    // nothing, and disarms.
    CHECK_MESSAGE((static_cast<bool>(fixture.invoke("ui.eyedropper_entity", nested))), "arming should work");
    componentTypesFor(fixture.document.options())
        .find("follow")
        ->remove(fixture.scene, fixture.scene.findEntity(fixture.owner));
    const OperatorResult orphaned = fixture.invoke("ui.eyedropper_pick", {{"entity", fixture.chased.value}});
    CHECK_MESSAGE((!orphaned && orphaned.message.find("Follow") != std::string::npos),
                  ("a pick for a removed component should fail naming it: " + orphaned.message));
    CHECK_MESSAGE((!fixture.eyedropper.armed() && fixture.history.size() == 1),
                  "a failed pick should disarm and record nothing");
    CHECK_MESSAGE(!fixture.invoke("ui.eyedropper_pick", {{"entity", fixture.chased.value}}),
                  "a pick without an armed eyedropper should not run");
}
} // namespace

// Entity ids are per scene: an eyedropper armed in one document picks only
// there, though another document has an entity with the same id.
TEST_CASE("EntityReferenceField: eyedropper stays in its document") {
    Fixture fixture;
    CHECK_MESSAGE(
        (static_cast<bool>(fixture.invoke(
            "ui.eyedropper_entity", {{"entity", fixture.owner.value}, {"component", "follow"}, {"field", "target"}}))),
        "the eyedropper should arm in the fixture's document");

    Scene otherScene;
    EditorDocument otherDocument(fixture.document.options().assetRoot);
    otherDocument.setComponentTypes(followComponentTypes());
    CommandHistory otherHistory;
    Selection otherSelection;
    while (otherScene.nextEntityId().value <= fixture.chased.value) {
        otherScene.createEntity("Stranger");
    }
    OperatorContext other{.editing = {otherScene, fixture.assets, otherDocument, otherHistory, fixture.diagnostics},
                          .selection = otherSelection,
                          .mode = EditorMode::Edit};
    const OperatorResult refused = fixture.runner.invokeWithJson(
        "ui.eyedropper_pick", other, nlohmann::json{{"entity", fixture.chased.value}}.dump());
    CHECK_MESSAGE((refused.status != OperatorStatus::Finished && fixture.eyedropper.armed() &&
                   !fixture.follow().target && otherHistory.size() == 0),
                  "a pick from another document should be refused and leave the eyedropper armed");
    CHECK_MESSAGE((fixture.eyedropper.armedFor(fixture.document) && !fixture.eyedropper.armedFor(otherDocument)),
                  "the eyedropper should be armed for its own document only");

    CHECK_MESSAGE((static_cast<bool>(fixture.invoke("ui.eyedropper_pick", {{"entity", fixture.chased.value}})) &&
                   fixture.follow().target == fixture.chased),
                  "a pick in the eyedropper's document should fill the field");
}

TEST_SUITE_END();
