// Live editing in Play and Simulate, headless: the runtime SceneEditing edits
// the running scene without recording edit history, operators run there
// unrecorded (and Edit-only ones explain why they refuse), Apply to Edit
// Scene records exactly the right undoable change on the edit scene, which
// Stop restores and undo and redo then act on, and the Inspector shows and
// applies the running scene's values.

#include "EditorTestFixture.hpp"
#include "HeadlessImGui.hpp"

#include "editor/document/RuntimeApply.hpp"
#include "editor/modules/GameModuleHost.hpp"
#include "editor/operators/ComponentOperators.hpp"
#include "editor/operators/ObjectOperators.hpp"
#include "editor/operators/PlayOperators.hpp"
#include "editor/panels/HierarchyPanel.hpp"
#include "editor/panels/InspectorPanel.hpp"
#include "engine/behaviour/Behaviour.hpp"
#include "engine/module/ModuleRegistrar.hpp"
#include "engine/scene/ComponentTypes.hpp"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
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

// What the session owns around live editing, without a window or GPU. The
// scene is the active one: the edit scene in Edit mode, the runtime scene
// while Play runs (EditorPlayMode parks the edit scene).
struct LiveFixture : EditorTestFixture {
    EditorPlayMode playMode;
    GameModuleHost modules{{.operators = operators,
                            .runner = runner,
                            .menus = menus.registry(),
                            .viewports = viewports,
                            .componentEditors = componentEditors}};

    LiveFixture()
        : EditorTestFixture({.logger = {.minimumLevel = LogLevel::Debug, .echoToStderr = false}}) {
        followedPlayMode = &playMode;
        expect(registerObjectOperators(operators).success, "the object operators register");
        expect(registerComponentOperators(operators, {.clipboard = clipboard, .layout = componentLayout}).success,
               "the component operators register");
        expect(registerPlayOperators(operators, {.playMode = playMode, .modules = modules}).success,
               "the play operators register");
    }

    std::string snapshot(const Scene& target) {
        std::string text;
        expect(SceneDocument::serialize(target, assets, document.options(), text).success, "the scene serializes");
        return text;
    }

    void play() {
        expect(playMode.enter(scene, assets, document.options(), selection.ids()).success, "Play starts");
    }

    void stop() {
        std::vector<SceneEntityId> restored;
        playMode.stop(scene, restored);
        selection.assign(std::move(restored));
    }

    Entity find(SceneEntityId id) const {
        return scene.findEntity(id);
    }
};

glm::vec3 location(const Scene& scene, SceneEntityId id) {
    return glm::vec3(scene.registry().get<TransformComponent>(scene.findEntity(id)).local[3]);
}

void setLocation(Scene& scene, SceneEntityId id, glm::vec3 value) {
    const Entity entity = scene.findEntity(id);
    scene.registry().get<TransformComponent>(entity).local[3] = glm::vec4(value, 1.0f);
    scene.markTransformDirty(entity);
}

float intensity(const Scene& scene, SceneEntityId id) {
    return scene.registry().get<PointLightComponent>(scene.findEntity(id)).intensity;
}

bool hasInfo(const EditorDiagnostics& diagnostics, std::string_view fragment) {
    return std::ranges::any_of(diagnostics.entries(), [fragment](const EditorDiagnostic& entry) {
        return entry.severity == EditorDiagnosticSeverity::Info && entry.message.find(fragment) != std::string::npos;
    });
}

// The runtime SceneEditing edits live: commits, performs, undo, and redo
// record and replay nothing, and the document stays clean.
void testRuntimeEditingRecordsNothing() {
    LiveFixture fixture;
    const SceneEntityId lamp = fixture.scene.id(fixture.scene.createEntity("Lamp"));
    fixture.play();
    const SceneEditing live = fixture.editing();
    expect(live.runtime() && live.editScene() == fixture.playMode.editScene() && &live.scene() == &fixture.scene,
           "in Play the editing names the runtime scene and the parked edit scene");
    const uint64_t revision = fixture.history.revision();
    const uint64_t lightRevision = fixture.scene.revisions().light;
    std::string before;
    expect(live.capture(before), "the runtime scene can be captured");
    fixture.scene.registry().get<TransformComponent>(fixture.find(lamp)).local[3] = glm::vec4(4.0f, 0.0f, 0.0f, 1.0f);
    live.commit("Transform Gizmo", {}, std::move(before));
    expect(fixture.scene.revisions().light != lightRevision &&
               glm::vec3(fixture.scene.registry().get<TransformComponent>(fixture.find(lamp)).world[3]) ==
                   glm::vec3(4.0f, 0.0f, 0.0f),
           "a runtime commit tells the renderer and transforms what changed");
    live.perform("Rename", {}, [&] {
        fixture.scene.registry().get<NameComponent>(fixture.find(lamp)).name = "Live Lamp";
        return SceneResult{.success = true};
    });
    expect(location(fixture.scene, lamp) == glm::vec3(4.0f, 0.0f, 0.0f) &&
               fixture.scene.registry().get<NameComponent>(fixture.find(lamp)).name == "Live Lamp",
           "runtime edits stay applied to the running scene");
    expect(fixture.history.size() == 0 && fixture.history.revision() == revision && !fixture.document.dirty(),
           "runtime edits (the gizmo's and the inspector's commits) never enter the edit history");
    live.undo();
    expect(location(fixture.scene, lamp) == glm::vec3(4.0f, 0.0f, 0.0f), "undo does nothing to the running scene");
    expect(location(*fixture.playMode.editScene(), lamp) == glm::vec3(0.0f), "the edit scene is untouched");
    fixture.stop();
    expect(location(fixture.scene, lamp) == glm::vec3(0.0f) && !fixture.editing().runtime(),
           "Stop discards runtime edits and editing returns to the document");
}

// Structural operators run on the running scene, unrecorded and noted as
// lost on Stop; operators that need Edit mode say so.
void testRuntimeOperators() {
    LiveFixture fixture;
    fixture.play();
    const OperatorResult addEmpty = fixture.invoke("object.add_empty");
    expect(addEmpty.status == OperatorStatus::Cancelled &&
               addEmpty.message.find("only in Edit mode") != std::string::npos,
           "the Add menu's operators refuse in Play, as the menu does");
    fixture.selection.clear();
    expect(static_cast<bool>(fixture.invoke("object.create_entity")) && fixture.selection.size() == 1,
           "creating an object works in Play");
    const SceneEntityId added = *fixture.selection.active();
    expect(static_cast<bool>(fixture.invoke("object.component_add", R"({"component":"point_light"})")) &&
               fixture.scene.registry().all_of<PointLightComponent>(fixture.find(added)),
           "adding a component works in Play");
    expect(fixture.history.size() == 0 && fixture.runner.lastOperator() == nullptr,
           "runtime operators record no undo step and cannot be adjusted");
    expect(hasInfo(fixture.diagnostics, "Stop discards it"), "a runtime structural change is noted as runtime-only");
    expect(fixture.playMode.editScene()->findEntity(added) == entt::null, "the edit scene does not get the object");

    const OperatorResult collection = fixture.invoke("collection.new");
    expect(collection.status == OperatorStatus::Cancelled &&
               collection.message.find("only in Edit mode") != std::string::npos,
           "an Edit-only operator refuses in Play and says why");
    fixture.selection.clear();
    const OperatorResult nothing = fixture.invoke("object.delete");
    expect(nothing.status == OperatorStatus::Cancelled && nothing.message.find("Edit mode") == std::string::npos,
           "an operator unavailable for another reason does not blame the mode");
    fixture.stop();
    expect(fixture.find(added) == entt::null, "Stop discards the runtime object");
    expect(fixture.invoke("play.apply_to_edit").status == OperatorStatus::Cancelled,
           "Apply to Edit Scene needs Play or Simulate");
}

// Apply to Edit Scene: a component, a no-op, a whole entity, and an entity
// that exists only at runtime, each one undo step on the edit scene that
// leaves the running scene alone; Stop keeps them and undo and redo work.
void testApplyToEditScene() {
    LiveFixture fixture;
    Scene& scene = fixture.scene;
    const Entity lampEntity = scene.createEntity("Lamp");
    scene.registry().emplace<PointLightComponent>(lampEntity).intensity = 1.0f;
    const SceneEntityId lamp = scene.id(lampEntity);
    const SceneEntityId other = scene.id(scene.createEntity("Other"));
    const std::string original = fixture.snapshot(scene);
    fixture.play();

    // Running values: a brighter light, a moved and renamed lamp, a child
    // spawned under it, and another entity changed but never applied.
    scene.registry().get<PointLightComponent>(fixture.find(lamp)).intensity = 5.0f;
    setLocation(scene, lamp, {1.0f, 2.0f, 3.0f});
    scene.registry().get<NameComponent>(fixture.find(lamp)).name = "Running Lamp";
    const Entity sparkEntity = scene.createEntity("Spark");
    scene.setParent(sparkEntity, fixture.find(lamp));
    const SceneEntityId spark = scene.id(sparkEntity);
    setLocation(scene, other, {9.0f, 9.0f, 9.0f});
    const std::string running = fixture.snapshot(scene);
    const Scene& edit = *fixture.playMode.editScene();

    const std::string lampId = std::to_string(lamp.value);
    expect(static_cast<bool>(
               fixture.invoke("play.apply_to_edit", R"({"entity":")" + lampId + R"(","component":"point_light"})")),
           "applying a component finishes");
    expect(fixture.history.size() == 1 && intensity(edit, lamp) == 5.0f && location(edit, lamp) == glm::vec3(0.0f) &&
               edit.registry().get<NameComponent>(edit.findEntity(lamp)).name == "Lamp",
           "applying a component changes only that component in the edit scene, as one step");
    expect(fixture.snapshot(scene) == running, "applying never changes the running scene");
    expect(hasInfo(fixture.diagnostics, "Applied"), "a successful apply is reported in Info");

    expect(fixture.invoke("play.apply_to_edit", R"({"entity":")" + lampId + R"(","component":"point_light"})").status ==
                   OperatorStatus::Cancelled &&
               fixture.history.size() == 1,
           "applying a component that already matches records nothing");
    expect(fixture.invoke("play.apply_to_edit", R"({"entity":")" + lampId + R"(","component":"spot_light"})").status ==
               OperatorStatus::Failed,
           "applying a component the running entity lacks fails");
    fixture.diagnostics.acknowledgeErrors();

    fixture.selection.selectOnly(lamp);
    expect(static_cast<bool>(fixture.invoke("play.apply_to_edit")) && fixture.history.size() == 2 &&
               location(edit, lamp) == glm::vec3(1.0f, 2.0f, 3.0f) &&
               edit.registry().get<NameComponent>(edit.findEntity(lamp)).name == "Running Lamp",
           "applying the selected entity applies its name and transform as one more step");
    expect(location(edit, other) == glm::vec3(0.0f), "entities that were not applied keep their edit values");

    expect(static_cast<bool>(
               fixture.invoke("play.apply_to_edit", R"({"entity":")" + std::to_string(spark.value) + R"("})")) &&
               fixture.history.size() == 3 && edit.findEntity(spark) != entt::null &&
               edit.registry().get<RelationshipComponent>(edit.findEntity(spark)).parent == edit.findEntity(lamp),
           "applying an entity spawned at runtime creates it under its parent in the edit scene");
    expect(fixture.snapshot(scene) == running, "no apply changed the running scene");

    fixture.stop();
    expect(intensity(scene, lamp) == 5.0f && location(scene, lamp) == glm::vec3(1.0f, 2.0f, 3.0f) &&
               fixture.find(spark) != entt::null && location(scene, other) == glm::vec3(0.0f),
           "Stop restores the edit scene with everything applied and nothing else");
    expect(fixture.document.dirty(), "the applied edits leave the document modified");
    const std::string applied = fixture.snapshot(scene);
    SceneEditing editing = fixture.editing();
    for (int step = 0; step < 3; ++step) {
        editing.undo();
    }
    expect(fixture.snapshot(scene) == original, "undo after Stop reverts every apply");
    for (int step = 0; step < 3; ++step) {
        editing.redo();
    }
    expect(fixture.snapshot(scene) == applied, "redo after Stop applies them again");
}

// The document function on its own: before sides are the edit scene's, and
// requests that cannot apply fail descriptively.
void testRuntimeChanges() {
    LiveFixture fixture;
    Scene editScene;
    Scene runtimeScene;
    const Entity editEntity = editScene.createEntity("Crate");
    const SceneEntityId crate = editScene.id(editEntity);
    std::string text;
    expect(SceneDocument::serialize(editScene, fixture.assets, fixture.document.options(), text).success &&
               SceneDocument::deserialize(text, fixture.assets, fixture.document.options(), runtimeScene).success,
           "the runtime scene starts as a copy");
    SceneChanges changes;
    const std::vector<SceneEntityId> ids{crate};
    expect(runtimeChangesForEditScene(runtimeScene, editScene, fixture.assets, fixture.document.options(),
                                      {.entities = ids}, changes)
                   .success &&
               changes.empty(),
           "an unchanged entity has nothing to apply");
    runtimeScene.registry().get<NameComponent>(runtimeScene.findEntity(crate)).name = "Moved Crate";
    expect(runtimeChangesForEditScene(runtimeScene, editScene, fixture.assets, fixture.document.options(),
                                      {.entities = ids}, changes)
                   .success &&
               changes.entities.size() == 1 && changes.entities.front().before &&
               changes.entities.front().before->find("\"Crate\"") != std::string::npos &&
               changes.entities.front().after->find("Moved Crate") != std::string::npos,
           "a change's before side is the edit entity and its after side the running one");
    const std::vector<SceneEntityId> missing{SceneEntityId{987654}};
    const SceneDocumentResult absent = runtimeChangesForEditScene(
        runtimeScene, editScene, fixture.assets, fixture.document.options(), {.entities = missing}, changes);
    expect(!absent && absent.error.find("987654") != std::string::npos,
           "an entity missing from the running scene fails, naming it");
    // A runtime-only child of a runtime-only parent lands at the root at its
    // running world placement.
    const Entity holder = runtimeScene.createEntity("Holder");
    const Entity held = runtimeScene.createEntity("Held");
    runtimeScene.setParent(held, holder);
    runtimeScene.registry().get<TransformComponent>(holder).local[3] = glm::vec4(5.0f, 0.0f, 0.0f, 1.0f);
    runtimeScene.registry().get<TransformComponent>(held).local[3] = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
    const std::vector<SceneEntityId> heldIds{runtimeScene.id(held)};
    expect(runtimeChangesForEditScene(runtimeScene, editScene, fixture.assets, fixture.document.options(),
                                      {.entities = heldIds}, changes)
                   .success &&
               changes.entities.size() == 1,
           "an entity spawned under an unapplied parent can be applied");
    Scene patched;
    expect(SceneDocument::deserialize(text, fixture.assets, fixture.document.options(), patched).success &&
               SceneDocument::applyChanges(changes, SceneChangeDirection::Forward, fixture.assets,
                                           fixture.document.options(), patched)
                   .success,
           "the change applies to a copy of the edit scene");
    const Entity placed = patched.findEntity(heldIds.front());
    expect(placed != entt::null && patched.registry().get<RelationshipComponent>(placed).parent == entt::null &&
               glm::vec3(patched.registry().get<TransformComponent>(placed).local[3]) == glm::vec3(6.0f, 0.0f, 0.0f),
           "under another parent the applied entity keeps its running world placement");

    const Entity spawned = runtimeScene.createEntity("Spawned");
    const std::vector<SceneEntityId> spawnedIds{runtimeScene.id(spawned)};
    runtimeScene.registry().emplace<PointLightComponent>(spawned);
    expect(!runtimeChangesForEditScene(runtimeScene, editScene, fixture.assets, fixture.document.options(),
                                       {.entities = spawnedIds, .component = "point_light"}, changes),
           "a component of an entity the edit scene lacks cannot be applied alone");
}

// The Inspector and Hierarchy stay usable in Play: the Inspector shows the
// running values each frame, and its Apply button applies the selection.
void testInspectorInPlay() {
    LiveFixture fixture;
    const Entity lampEntity = fixture.scene.createEntity("Lamp");
    fixture.scene.registry().emplace<PointLightComponent>(lampEntity).intensity = 2.5f;
    const SceneEntityId lamp = fixture.scene.id(lampEntity);
    fixture.selection.selectOnly(lamp);
    InspectorPanel inspector(fixture.componentLayout);
    HierarchyPanel hierarchy;
    expect(!inspector.editModeOnly() && !hierarchy.editModeOnly(),
           "the Inspector and Hierarchy stay enabled in Play and Simulate");
    fixture.play();

    HeadlessImGuiContext imgui;
    const auto drawLogged = [&] {
        HeadlessImGuiFrame frame(inspector.title());
        ImGui::LogToBuffer();
        EditorContext editor = fixture.context();
        inspector.draw(editor);
        std::string text = ImGui::GetCurrentContext()->LogBuffer.c_str();
        ImGui::LogFinish();
        return text;
    };
    const std::string first = drawLogged();
    expect(first.find("lost on Stop") != std::string::npos && first.find("2.5") != std::string::npos,
           "the Inspector marks live editing and shows the running value");
    fixture.scene.registry().get<PointLightComponent>(fixture.find(lamp)).intensity = 7.25f;
    expect(drawLogged().find("7.25") != std::string::npos, "the Inspector shows running values as they change");
    {
        HeadlessImGuiFrame frame(hierarchy.title());
        EditorContext editor = fixture.context();
        hierarchy.draw(editor);
    }

    const ImGuiWindow* window = HeadlessImGuiContext::findWindow(inspector.title());
    expect(window != nullptr, "the Inspector draws into its window");
    if (window == nullptr) {
        return;
    }
    HeadlessImGuiContext::activateNextFrame(
        HeadlessImGuiContext::itemId(*window, "##RuntimeNotice", "Apply Selection to Edit Scene"));
    drawLogged();
    drawLogged();
    expect(fixture.history.size() == 1 && intensity(*fixture.playMode.editScene(), lamp) == 7.25f,
           "the Inspector's Apply button applies the selection to the edit scene");
    fixture.stop();
    expect(intensity(fixture.scene, lamp) == 7.25f, "the applied value survives Stop");
}
} // namespace

// A behaviour as a game module registers one.
class Beacon final : public Behaviour {
  public:
    float range{5.0f};
};

// Behaviours are ordinary component types: their fields edit live in Play
// and Apply to Edit Scene carries them, and enabled, back to the edit scene.
void testBehaviourFieldsInPlay() {
    LiveFixture fixture;
    auto types = std::make_shared<ComponentTypeRegistry>();
    expect(registerBuiltInComponentTypes(*types).success, "the built-in types register");
    SystemRegistry systems;
    InputActionMap actions;
    BehaviourRegistry behaviours;
    ModuleRegistrar registrar(*types, systems, actions, behaviours);
    expect(registrar.addBehaviour<Beacon>("beacon", "Beacon", std::move(Fields<Beacon>().add("range", &Beacon::range))),
           "the behaviour registers");
    fixture.document.setComponentTypes(types);
    const Entity tower = fixture.scene.createEntity("Tower");
    fixture.scene.registry().emplace<Beacon>(tower);
    const SceneEntityId towerId = fixture.scene.id(tower);

    fixture.play();
    Beacon& running = fixture.scene.registry().get<Beacon>(fixture.find(towerId));
    running.range = 12.0f;
    running.enabled = false;
    const Scene& edit = *fixture.playMode.editScene();
    expect(edit.registry().get<Beacon>(edit.findEntity(towerId)).range == 5.0f,
           "a live behaviour edit leaves the edit scene alone");
    expect(static_cast<bool>(fixture.invoke("play.apply_to_edit", R"({"entity":")" + std::to_string(towerId.value) +
                                                                      R"(","component":"beacon"})")),
           "applying a behaviour finishes");
    const Beacon& applied = edit.registry().get<Beacon>(edit.findEntity(towerId));
    expect(applied.range == 12.0f && !applied.enabled && fixture.history.size() == 1,
           "Apply to Edit Scene carries the behaviour's fields and enabled as one step");
    expect(!applied.created() && !applied.started(), "the edit scene's behaviour is not created or started");
    fixture.stop();
    expect(fixture.scene.registry().get<Beacon>(fixture.find(towerId)).range == 12.0f,
           "Stop keeps the applied behaviour values");
}

int main() {
    testRuntimeEditingRecordsNothing();
    testRuntimeOperators();
    testApplyToEditScene();
    testRuntimeChanges();
    testInspectorInPlay();
    testBehaviourFieldsInPlay();
    return failures == 0 ? 0 : 1;
}
