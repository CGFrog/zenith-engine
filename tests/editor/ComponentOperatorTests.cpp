#include "editor/application/EditorDiagnostics.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/operators/ComponentOperators.hpp"
#include "editor/operators/ObjectOperators.hpp"
#include "editor/operators/OperatorRunner.hpp"
#include "editor/settings/ComponentInspectorLayout.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/physics/PhysicsComponents.hpp"
#include "engine/scene/ComponentTypes.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <variant>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

// The document, history, selection, clipboard, and inspector layout the
// editor session owns, without a window.
struct ComponentFixture {
    AssetManager assets;
    Scene scene;
    EditorDocument document{std::filesystem::temp_directory_path()};
    CommandHistory history;
    EditorDiagnostics diagnostics;
    Selection selection;
    MemoryClipboard clipboard;
    ComponentInspectorLayout layout;
    OperatorRegistry registry;
    OperatorRunner runner{registry, history};

    ComponentFixture() {
        expect(registerObjectOperators(registry).success, "the object operators should register");
        expect(registerComponentOperators(registry, {.clipboard = clipboard, .layout = layout}).success,
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

void testResetToDefault() {
    ComponentFixture fixture;
    const SceneEntityId first = fixture.light("First", 2.0f);
    const SceneEntityId second = fixture.light("Second", 3.0f);
    const SceneEntityId bare = fixture.scene.id(fixture.scene.createEntity("Bare"));
    fixture.pointLight(first)->intensity = 99.0f;

    const ComponentType* type = componentTypesFor(fixture.document.options()).find("point_light");
    expect(std::any_cast<PointLightComponent>(type->makeDefault()).range == PointLightComponent{}.range,
           "makeDefault should hold a default-constructed component");

    fixture.selection.assign({first, second, bare});
    expect(static_cast<bool>(fixture.invoke("object.component_reset", R"({"component":"point_light"})")),
           "resetting a component the selection has should finish");
    const PointLightComponent defaults;
    expect(fixture.pointLight(first)->range == defaults.range &&
               fixture.pointLight(first)->intensity == defaults.intensity &&
               fixture.pointLight(second)->range == defaults.range,
           "every selected object with the component should be reset");
    expect(fixture.pointLight(bare) == nullptr, "reset should not add the component to objects without it");
    expect(fixture.history.size() == 1, "the reset should be one undo step");
    fixture.undo();
    expect(fixture.pointLight(first)->range == 2.0f && fixture.pointLight(second)->range == 3.0f,
           "undo should restore every reset component");

    fixture.selection.selectOnly(bare);
    expect(fixture.invoke("object.component_reset", R"({"component":"point_light"})").status ==
               OperatorStatus::Cancelled,
           "reset should cancel when no selected object has the component");
    expect(fixture.invoke("object.component_reset", R"({"component":"no_such_type"})").status == OperatorStatus::Failed,
           "reset should fail for an unknown component type");
    fixture.diagnostics.acknowledgeErrors();
}

void testCopyAndPaste() {
    ComponentFixture fixture;
    const SceneEntityId source = fixture.light("Source", 4.5f);
    const SceneEntityId target = fixture.light("Target", 1.0f);
    const SceneEntityId other = fixture.light("Other", 2.0f);
    const SceneEntityId bare = fixture.scene.id(fixture.scene.createEntity("Bare"));
    fixture.pointLight(source)->color = glm::vec3(0.25f, 0.5f, 1.0f);

    fixture.selection.selectOnly(source);
    expect(static_cast<bool>(fixture.invoke("object.component_copy", R"({"component":"point_light"})")),
           "copying the active object's component should finish");
    expect(fixture.history.size() == 0, "copying values should not record an undo step");
    const nlohmann::json copied = nlohmann::json::parse(fixture.clipboard.text(), nullptr, false);
    expect(copied.is_object() && copied.value("zenith_component", "") == "point_light" &&
               copied["value"].value("range", 0.0f) == 4.5f,
           "the clipboard should hold the component's document JSON");

    fixture.selection.assign({target, other, bare});
    expect(static_cast<bool>(fixture.invoke("object.component_paste", R"({"component":"point_light"})")),
           "pasting onto selected objects with the component should finish");
    expect(fixture.pointLight(target)->range == 4.5f && fixture.pointLight(other)->range == 4.5f &&
               fixture.pointLight(target)->color == glm::vec3(0.25f, 0.5f, 1.0f),
           "paste should copy every value onto each selected object with the component");
    expect(fixture.pointLight(bare) == nullptr, "paste should not add the component");
    expect(fixture.history.size() == 1, "the paste should be one undo step");
    fixture.undo();
    expect(fixture.pointLight(target)->range == 1.0f && fixture.pointLight(other)->range == 2.0f,
           "undo should restore the pasted components");

    // An empty component property pastes whatever type was copied.
    expect(static_cast<bool>(fixture.invoke("object.component_paste")), "paste should default to the copied type");
    expect(fixture.pointLight(target)->range == 4.5f, "a default paste should apply the copied type");

    expect(fixture.invoke("object.component_paste", R"({"component":"spot_light"})").status ==
               OperatorStatus::Cancelled,
           "pasting values of another type should cancel");
    fixture.clipboard.setText("not component values");
    expect(fixture.invoke("object.component_paste", R"({"component":"point_light"})").status ==
               OperatorStatus::Cancelled,
           "pasting without copied values should cancel");
    fixture.clipboard.setText(R"({"zenith_component":"point_light","value":{"range":-1}})");
    const size_t steps = fixture.history.size();
    expect(fixture.invoke("object.component_paste", R"({"component":"point_light"})").status ==
                   OperatorStatus::Failed &&
               fixture.history.size() == steps && fixture.pointLight(target)->range == 4.5f,
           "pasting invalid values should fail and change nothing");
    fixture.diagnostics.acknowledgeErrors();
}

void testCustomCodecCopyToSelected() {
    ComponentFixture fixture;
    const Entity source = fixture.scene.createEntity("Source");
    fixture.scene.registry().emplace<ColliderComponent>(source).shape = SphereCollider{.radius = 2.5f};
    const Entity withCollider = fixture.scene.createEntity("With collider");
    fixture.scene.registry().emplace<ColliderComponent>(withCollider);
    const Entity without = fixture.scene.createEntity("Without");

    fixture.selection.selectOnly(fixture.scene.id(source));
    expect(fixture.invoke("object.component_copy_to_selected", R"({"component":"collider"})").status ==
               OperatorStatus::Cancelled,
           "copy to selected needs other selected objects");

    fixture.selection.assign({fixture.scene.id(source), fixture.scene.id(withCollider), fixture.scene.id(without)});
    expect(static_cast<bool>(fixture.invoke("object.component_copy_to_selected", R"({"component":"collider"})")),
           "copying a component to the selection should finish");
    for (const Entity entity : {withCollider, without}) {
        const auto* collider = fixture.scene.registry().try_get<ColliderComponent>(entity);
        const auto* sphere = collider != nullptr ? std::get_if<SphereCollider>(&collider->shape) : nullptr;
        expect(sphere != nullptr && sphere->radius == 2.5f,
               "every other selected object should get the active object's collider, added where missing");
    }
    expect(fixture.history.size() == 1, "copy to selected should be one undo step");
    fixture.undo();
    expect(!fixture.scene.registry().all_of<ColliderComponent>(fixture.scene.findEntity(fixture.selection.ids()[2])),
           "undo should remove the added component");

    // A custom codec round-trips through the clipboard too.
    fixture.selection.selectOnly(fixture.scene.id(source));
    fixture.invoke("object.component_copy", R"({"component":"collider"})");
    const SceneEntityId boxId = fixture.scene.id(fixture.scene.createEntity("Box"));
    fixture.scene.registry().emplace<ColliderComponent>(fixture.scene.findEntity(boxId));
    fixture.selection.selectOnly(boxId);
    expect(static_cast<bool>(fixture.invoke("object.component_paste", R"({"component":"collider"})")),
           "pasting custom codec values should finish");
    const auto& pasted = fixture.scene.registry().get<ColliderComponent>(fixture.scene.findEntity(boxId));
    expect(std::holds_alternative<SphereCollider>(pasted.shape), "the pasted collider should keep its shape");
}

void testRemoveFromSelection() {
    ComponentFixture fixture;
    const SceneEntityId first = fixture.light("First", 2.0f);
    const SceneEntityId second = fixture.light("Second", 3.0f);
    fixture.selection.assign({first, second});
    expect(static_cast<bool>(fixture.invoke("object.component_remove", R"({"component":"point_light"})")),
           "removing a component from the selection should finish");
    expect(fixture.pointLight(first) == nullptr && fixture.pointLight(second) == nullptr,
           "remove should act on every selected object with the component");
    expect(fixture.history.size() == 1, "the removal should be one undo step");
}

void testMove() {
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
    expect(initial.size() == 3, "the displayed keys should list the entity's components");

    expect(static_cast<bool>(
               fixture.invoke("object.component_move", R"({"component":")" + initial[2] + R"(","direction":"top"})")),
           "moving a component to the top should finish");
    std::vector<std::string> moved = displayed();
    expect(moved == std::vector<std::string>{initial[2], initial[0], initial[1]},
           "the moved component should be listed first");
    expect(fixture.history.size() == 0 && fixture.layout.takeChanged(), "moving is a layout change, not a scene edit");

    fixture.invoke("object.component_move", R"({"component":")" + initial[0] + R"(","direction":"down"})");
    expect(displayed() == std::vector<std::string>{initial[2], initial[1], initial[0]},
           "moving down should swap with the next component");
    expect(
        fixture.invoke("object.component_move", R"({"component":")" + initial[0] + R"(","direction":"down"})").status ==
            OperatorStatus::Cancelled,
        "the last component cannot move down");
    expect(fixture.invoke("object.component_move", R"({"component":"spot_light","direction":"up"})").status ==
               OperatorStatus::Cancelled,
           "a component the active object lacks cannot move");

    // A Properties category tab lists one category; moving there passes the
    // sections of other categories.
    types.find("spot_light")->addDefault(fixture.scene, entity);
    const std::vector<std::string> lights = displayedComponentKeys(types, fixture.layout, fixture.scene, id, "Lights");
    expect(lights.size() == 2, "a category should list only its own components");
    expect(static_cast<bool>(fixture.invoke("object.component_move", R"({"component":")" + lights[1] +
                                                                         R"(","direction":"up","category":"Lights"})")),
           "moving within a category should finish");
    expect(displayedComponentKeys(types, fixture.layout, fixture.scene, id, "Lights") ==
               std::vector<std::string>{lights[1], lights[0]},
           "moving up within a category should pass the previous component of that category");
}

void testLayoutOrdering() {
    ComponentInspectorLayout layout;
    const std::vector<std::string> registryOrder{"a", "b", "c", "d"};
    expect(layout.sorted(registryOrder) == registryOrder, "without a stored order keys keep registry order");
    expect(layout.move("c", ComponentMoveDirection::Up, std::vector<std::string>{"a", "b", "c"}),
           "a displayed key should move");
    expect(layout.sorted(registryOrder) == std::vector<std::string>({"a", "c", "b", "d"}),
           "stored keys come first in their order, then the rest");
    // Another entity shows a different subset: stored keys it does not show
    // keep their places.
    expect(layout.move("d", ComponentMoveDirection::Top, std::vector<std::string>{"c", "d"}), "d should move");
    expect(layout.order() == std::vector<std::string>({"a", "d", "b", "c"}),
           "moving within a subset should only reorder that subset's slots");
    expect(!layout.move("x", ComponentMoveDirection::Up, std::vector<std::string>{"a"}),
           "a key that is not displayed cannot move");

    layout.setCollapsed("b", true);
    expect(layout.collapsed("b") && !layout.collapsed("a"), "collapse state is per component type");
    layout.setCollapsed("b", false);
    expect(!layout.collapsed("b"), "a section can be expanded again");
}

void testLayoutPersistence() {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "zenith-component-layout-test";
    std::filesystem::remove_all(directory);
    const std::filesystem::path path = directory / "component-inspector.json";

    ComponentInspectorLayout missing;
    expect(static_cast<bool>(missing.load(path)) && missing.order().empty(),
           "a missing layout file should load as defaults");

    ComponentInspectorLayout layout;
    layout.move("collider", ComponentMoveDirection::Top, std::vector<std::string>{"point_light", "collider"});
    layout.setCollapsed("point_light", true);
    expect(layout.takeChanged() && !layout.takeChanged(), "changes should be reported once");
    expect(static_cast<bool>(layout.save(path)), "the layout should save");

    ComponentInspectorLayout loaded;
    expect(static_cast<bool>(loaded.load(path)) && loaded == layout && !loaded.takeChanged(),
           "a saved layout should load back unchanged");

    {
        std::ofstream invalid(path, std::ios::trunc);
        invalid << R"({"version":1,"order":[3]})";
    }
    const ComponentInspectorLayoutResult result = loaded.load(path);
    expect(!result && result.error.find("order") != std::string::npos && loaded.order().empty() &&
               loaded.collapsedKeys().empty(),
           "an invalid file should report why and leave the defaults");
    std::filesystem::remove_all(directory);
}
} // namespace

int main() {
    testResetToDefault();
    testCopyAndPaste();
    testCustomCodecCopyToSelected();
    testRemoveFromSelection();
    testMove();
    testLayoutOrdering();
    testLayoutPersistence();
    if (failures != 0) {
        std::cerr << failures << " component operator test(s) failed\n";
        return 1;
    }
    return 0;
}
