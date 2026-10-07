#include "editor/application/EditorDiagnostics.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/operators/ComponentOperators.hpp"
#include "editor/operators/ObjectOperators.hpp"
#include "editor/operators/OperatorRunner.hpp"
#include "editor/settings/ComponentInspectorLayout.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/physics/PhysicsComponents.hpp"
#include "engine/scene/ComponentTypes.hpp"

#include "support/Doctest.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <variant>
#include <vector>

TEST_SUITE_BEGIN("ComponentOperator");

namespace {

// The document, history, selection, clipboard, and inspector layout the
// editor session owns, without a window.
struct ComponentFixture {
    // The document's asset root; declared first, so it outlives the document.
    const testsupport::TempDirectory root{"component-operators"};
    AssetManager assets;
    Scene scene;
    EditorDocument document{root.path()};
    CommandHistory history;
    EditorDiagnostics diagnostics;
    Selection selection;
    MemoryClipboard clipboard;
    ComponentInspectorLayout layout;
    OperatorRegistry registry;
    OperatorRunner runner{registry, history};

    ComponentFixture() {
        CHECK_MESSAGE(registerObjectOperators(registry).success, "the object operators should register");
        CHECK_MESSAGE(registerComponentOperators(registry, {.clipboard = clipboard, .layout = layout}).success,
                      "the component operators should register");
    }

    OperatorResult invoke(std::string_view id, std::string_view properties = {}) {
        OperatorContext context{.editing = SceneEditing(scene, assets, document, history, diagnostics),
                                .selection = selection,
                                .mode = EditorMode::Edit};
        return runner.invokeWithJson(id, context, properties);
    }

    void undo() {
        SceneEditing(scene, assets, document, history, diagnostics).undo();
    }

    // An entity with a point light of the given range.
    SceneEntityId light(const char* name, float range) {
        const Entity entity = scene.createEntity(name);
        scene.registry().emplace<PointLightComponent>(entity).range = range;
        return scene.id(entity);
    }

    PointLightComponent* pointLight(SceneEntityId id) {
        return scene.registry().try_get<PointLightComponent>(scene.findEntity(id));
    }
};

TEST_CASE("ComponentOperator: reset to default") {
    ComponentFixture fixture;
    const SceneEntityId first = fixture.light("First", 2.0f);
    const SceneEntityId second = fixture.light("Second", 3.0f);
    const SceneEntityId bare = fixture.scene.id(fixture.scene.createEntity("Bare"));
    fixture.pointLight(first)->intensity = 99.0f;

    const ComponentType* type = componentTypesFor(fixture.document.options()).find("point_light");
    CHECK_MESSAGE((std::any_cast<PointLightComponent>(type->makeDefault()).range == PointLightComponent{}.range),
                  "makeDefault should hold a default-constructed component");

    fixture.selection.assign({first, second, bare});
    CHECK_MESSAGE((static_cast<bool>(fixture.invoke("object.component_reset", R"({"component":"point_light"})"))),
                  "resetting a component the selection has should finish");
    const PointLightComponent defaults;
    CHECK_MESSAGE((fixture.pointLight(first)->range == defaults.range &&
                   fixture.pointLight(first)->intensity == defaults.intensity &&
                   fixture.pointLight(second)->range == defaults.range),
                  "every selected object with the component should be reset");
    CHECK_MESSAGE(fixture.pointLight(bare) == nullptr, "reset should not add the component to objects without it");
    CHECK_MESSAGE(fixture.history.size() == 1, "the reset should be one undo step");
    fixture.undo();
    CHECK_MESSAGE((fixture.pointLight(first)->range == 2.0f && fixture.pointLight(second)->range == 3.0f),
                  "undo should restore every reset component");

    fixture.selection.selectOnly(bare);
    CHECK_MESSAGE(fixture.invoke("object.component_reset", R"({"component":"point_light"})").status ==
                      OperatorStatus::Cancelled,
                  "reset should cancel when no selected object has the component");
    CHECK_MESSAGE(fixture.invoke("object.component_reset", R"({"component":"no_such_type"})").status ==
                      OperatorStatus::Failed,
                  "reset should fail for an unknown component type");
    fixture.diagnostics.acknowledgeErrors();
}

TEST_CASE("ComponentOperator: copy and paste") {
    ComponentFixture fixture;
    const SceneEntityId source = fixture.light("Source", 4.5f);
    const SceneEntityId target = fixture.light("Target", 1.0f);
    const SceneEntityId other = fixture.light("Other", 2.0f);
    const SceneEntityId bare = fixture.scene.id(fixture.scene.createEntity("Bare"));
    fixture.pointLight(source)->color = glm::vec3(0.25f, 0.5f, 1.0f);

    fixture.selection.selectOnly(source);
    CHECK_MESSAGE((static_cast<bool>(fixture.invoke("object.component_copy", R"({"component":"point_light"})"))),
                  "copying the active object's component should finish");
    CHECK_MESSAGE(fixture.history.size() == 0, "copying values should not record an undo step");
    const nlohmann::json copied = nlohmann::json::parse(fixture.clipboard.text(), nullptr, false);
    CHECK_MESSAGE((copied.is_object() && copied.value("zenith_component", "") == "point_light" &&
                   copied["value"].value("range", 0.0f) == 4.5f),
                  "the clipboard should hold the component's document JSON");

    fixture.selection.assign({target, other, bare});
    CHECK_MESSAGE((static_cast<bool>(fixture.invoke("object.component_paste", R"({"component":"point_light"})"))),
                  "pasting onto selected objects with the component should finish");
    CHECK_MESSAGE((fixture.pointLight(target)->range == 4.5f && fixture.pointLight(other)->range == 4.5f &&
                   fixture.pointLight(target)->color == glm::vec3(0.25f, 0.5f, 1.0f)),
                  "paste should copy every value onto each selected object with the component");
    CHECK_MESSAGE(fixture.pointLight(bare) == nullptr, "paste should not add the component");
    CHECK_MESSAGE(fixture.history.size() == 1, "the paste should be one undo step");
    fixture.undo();
    CHECK_MESSAGE((fixture.pointLight(target)->range == 1.0f && fixture.pointLight(other)->range == 2.0f),
                  "undo should restore the pasted components");

    // An empty component property pastes whatever type was copied.
    CHECK_MESSAGE((static_cast<bool>(fixture.invoke("object.component_paste"))),
                  "paste should default to the copied type");
    CHECK_MESSAGE(fixture.pointLight(target)->range == 4.5f, "a default paste should apply the copied type");

    CHECK_MESSAGE(fixture.invoke("object.component_paste", R"({"component":"spot_light"})").status ==
                      OperatorStatus::Cancelled,
                  "pasting values of another type should cancel");
    fixture.clipboard.setText("not component values");
    CHECK_MESSAGE(fixture.invoke("object.component_paste", R"({"component":"point_light"})").status ==
                      OperatorStatus::Cancelled,
                  "pasting without copied values should cancel");
    fixture.clipboard.setText(R"({"zenith_component":"point_light","value":{"range":-1}})");
    const size_t steps = fixture.history.size();
    CHECK_MESSAGE(
        (fixture.invoke("object.component_paste", R"({"component":"point_light"})").status == OperatorStatus::Failed &&
         fixture.history.size() == steps && fixture.pointLight(target)->range == 4.5f),
        "pasting invalid values should fail and change nothing");
    fixture.diagnostics.acknowledgeErrors();
}

TEST_CASE("ComponentOperator: custom codec copy to selected") {
    ComponentFixture fixture;
    const Entity source = fixture.scene.createEntity("Source");
    fixture.scene.registry().emplace<ColliderComponent>(source).shape = SphereCollider{.radius = 2.5f};
    const Entity withCollider = fixture.scene.createEntity("With collider");
    fixture.scene.registry().emplace<ColliderComponent>(withCollider);
    const Entity without = fixture.scene.createEntity("Without");

    fixture.selection.selectOnly(fixture.scene.id(source));
    CHECK_MESSAGE(fixture.invoke("object.component_copy_to_selected", R"({"component":"collider"})").status ==
                      OperatorStatus::Cancelled,
                  "copy to selected needs other selected objects");

    fixture.selection.assign({fixture.scene.id(source), fixture.scene.id(withCollider), fixture.scene.id(without)});
    CHECK_MESSAGE(
        (static_cast<bool>(fixture.invoke("object.component_copy_to_selected", R"({"component":"collider"})"))),
        "copying a component to the selection should finish");
    for (const Entity entity : {withCollider, without}) {
        const auto* collider = fixture.scene.registry().try_get<ColliderComponent>(entity);
        const auto* sphere = collider != nullptr ? std::get_if<SphereCollider>(&collider->shape) : nullptr;
        CHECK_MESSAGE((sphere != nullptr && sphere->radius == 2.5f),
                      "every other selected object should get the active object's collider, added where missing");
    }
    CHECK_MESSAGE(fixture.history.size() == 1, "copy to selected should be one undo step");
    fixture.undo();
    CHECK_MESSAGE(
        (!fixture.scene.registry().all_of<ColliderComponent>(fixture.scene.findEntity(fixture.selection.ids()[2]))),
        "undo should remove the added component");

    // A custom codec round-trips through the clipboard too.
    fixture.selection.selectOnly(fixture.scene.id(source));
    fixture.invoke("object.component_copy", R"({"component":"collider"})");
    const SceneEntityId boxId = fixture.scene.id(fixture.scene.createEntity("Box"));
    fixture.scene.registry().emplace<ColliderComponent>(fixture.scene.findEntity(boxId));
    fixture.selection.selectOnly(boxId);
    CHECK_MESSAGE((static_cast<bool>(fixture.invoke("object.component_paste", R"({"component":"collider"})"))),
                  "pasting custom codec values should finish");
    const auto& pasted = fixture.scene.registry().get<ColliderComponent>(fixture.scene.findEntity(boxId));
    CHECK_MESSAGE((std::holds_alternative<SphereCollider>(pasted.shape)), "the pasted collider should keep its shape");
}

TEST_CASE("ComponentOperator: remove from selection") {
    ComponentFixture fixture;
    const SceneEntityId first = fixture.light("First", 2.0f);
    const SceneEntityId second = fixture.light("Second", 3.0f);
    fixture.selection.assign({first, second});
    CHECK_MESSAGE((static_cast<bool>(fixture.invoke("object.component_remove", R"({"component":"point_light"})"))),
                  "removing a component from the selection should finish");
    CHECK_MESSAGE((fixture.pointLight(first) == nullptr && fixture.pointLight(second) == nullptr),
                  "remove should act on every selected object with the component");
    CHECK_MESSAGE(fixture.history.size() == 1, "the removal should be one undo step");
}

TEST_CASE("ComponentOperator: move") {
    ComponentFixture fixture;
    const Entity entity = fixture.scene.createEntity("Lit");
    const ComponentTypeRegistry& types = componentTypesFor(fixture.document.options());
    for (const char* key : {"point_light", "volumetric_fog", "collider"}) {
        types.find(key)->addDefault(fixture.scene, entity);
    }
    const SceneEntityId id = fixture.scene.id(entity);
    fixture.selection.selectOnly(id);
    const auto displayed = [&] { return displayedComponentKeys(types, fixture.layout, fixture.scene, id); };
    const std::vector<std::string> initial = displayed();
    CHECK_MESSAGE(initial.size() == 3, "the displayed keys should list the entity's components");

    CHECK_MESSAGE((static_cast<bool>(fixture.invoke("object.component_move",
                                                    R"({"component":")" + initial[2] + R"(","direction":"top"})"))),
                  "moving a component to the top should finish");
    std::vector<std::string> moved = displayed();
    CHECK_MESSAGE((moved == std::vector<std::string>{initial[2], initial[0], initial[1]}),
                  "the moved component should be listed first");
    CHECK_MESSAGE((fixture.history.size() == 0 && fixture.layout.takeChanged()),
                  "moving is a layout change, not a scene edit");

    fixture.invoke("object.component_move", R"({"component":")" + initial[0] + R"(","direction":"down"})");
    CHECK_MESSAGE((displayed() == std::vector<std::string>{initial[2], initial[1], initial[0]}),
                  "moving down should swap with the next component");
    CHECK_MESSAGE(
        fixture.invoke("object.component_move", R"({"component":")" + initial[0] + R"(","direction":"down"})").status ==
            OperatorStatus::Cancelled,
        "the last component cannot move down");
    CHECK_MESSAGE(fixture.invoke("object.component_move", R"({"component":"spot_light","direction":"up"})").status ==
                      OperatorStatus::Cancelled,
                  "a component the active object lacks cannot move");

    // A Properties category tab lists one category; moving there passes the
    // sections of other categories.
    types.find("spot_light")->addDefault(fixture.scene, entity);
    const std::vector<std::string> lights = displayedComponentKeys(types, fixture.layout, fixture.scene, id, "Lights");
    CHECK_MESSAGE(lights.size() == 2, "a category should list only its own components");
    CHECK_MESSAGE(
        (static_cast<bool>(fixture.invoke("object.component_move", R"({"component":")" + lights[1] +
                                                                       R"(","direction":"up","category":"Lights"})"))),
        "moving within a category should finish");
    CHECK_MESSAGE((displayedComponentKeys(types, fixture.layout, fixture.scene, id, "Lights") ==
                   std::vector<std::string>{lights[1], lights[0]}),
                  "moving up within a category should pass the previous component of that category");
}

TEST_CASE("ComponentOperator: layout ordering") {
    ComponentInspectorLayout layout;
    const std::vector<std::string> registryOrder{"a", "b", "c", "d"};
    CHECK_MESSAGE(layout.sorted(registryOrder) == registryOrder, "without a stored order keys keep registry order");
    CHECK_MESSAGE(layout.move("c", ComponentMoveDirection::Up, std::vector<std::string>{"a", "b", "c"}),
                  "a displayed key should move");
    CHECK_MESSAGE((layout.sorted(registryOrder) == std::vector<std::string>({"a", "c", "b", "d"})),
                  "stored keys come first in their order, then the rest");
    // Another entity shows a different subset: stored keys it does not show
    // keep their places.
    CHECK_MESSAGE(layout.move("d", ComponentMoveDirection::Top, std::vector<std::string>{"c", "d"}), "d should move");
    CHECK_MESSAGE((layout.order() == std::vector<std::string>({"a", "d", "b", "c"})),
                  "moving within a subset should only reorder that subset's slots");
    CHECK_MESSAGE(!layout.move("x", ComponentMoveDirection::Up, std::vector<std::string>{"a"}),
                  "a key that is not displayed cannot move");

    layout.setCollapsed("b", true);
    CHECK_MESSAGE((layout.collapsed("b") && !layout.collapsed("a")), "collapse state is per component type");
    layout.setCollapsed("b", false);
    CHECK_MESSAGE(!layout.collapsed("b"), "a section can be expanded again");
}

TEST_CASE("ComponentOperator: layout persistence") {
    const testsupport::TempDirectory directoryDirectory("zenith-component-layout-test");
    const std::filesystem::path directory = directoryDirectory.path();
    const std::filesystem::path path = directory / "component-inspector.json";

    ComponentInspectorLayout missing;
    CHECK_MESSAGE((static_cast<bool>(missing.load(path)) && missing.order().empty()),
                  "a missing layout file should load as defaults");

    ComponentInspectorLayout layout;
    layout.move("collider", ComponentMoveDirection::Top, std::vector<std::string>{"point_light", "collider"});
    layout.setCollapsed("point_light", true);
    CHECK_MESSAGE((layout.takeChanged() && !layout.takeChanged()), "changes should be reported once");
    CHECK_MESSAGE((static_cast<bool>(layout.save(path))), "the layout should save");

    ComponentInspectorLayout loaded;
    CHECK_MESSAGE((static_cast<bool>(loaded.load(path)) && loaded == layout && !loaded.takeChanged()),
                  "a saved layout should load back unchanged");

    {
        std::ofstream invalid(path, std::ios::trunc);
        invalid << R"({"version":1,"order":[3]})";
    }
    const ComponentInspectorLayoutResult result = loaded.load(path);
    CHECK_MESSAGE((!result && result.error.find("order") != std::string::npos && loaded.order().empty() &&
                   loaded.collapsedKeys().empty()),
                  "an invalid file should report why and leave the defaults");
}
} // namespace

TEST_SUITE_END();
