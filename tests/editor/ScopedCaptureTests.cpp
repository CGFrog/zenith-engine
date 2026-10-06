// Edits that cost what they change, not the scene: operators capture only
// their declared scope (OperatorScope), and copy and paste work on the copied
// subtrees alone (SceneFragment). Every migrated operator must undo and redo
// to exactly what a whole-scene capture of the same run gives; delete records
// descendants, new-entity scopes record what they create, Play skips the
// whole-scene capture, and paste keeps every other entity (its EnTT handle,
// and in Play its physics state) while preserving fragment semantics: prefab
// instances, collections, entity references, and unknown components.
#include "EditorTestFixture.hpp"

#include "editor/application/EntityCommands.hpp"
#include "editor/document/PrefabAuthoring.hpp"
#include "editor/document/SceneFragment.hpp"
#include "editor/operators/ComponentOperators.hpp"
#include "editor/operators/EyedropperOperators.hpp"
#include "editor/operators/ModelOperators.hpp"
#include "editor/operators/ObjectOperators.hpp"
#include "editor/operators/PlacementOperators.hpp"
#include "editor/operators/PrefabOperators.hpp"
#include "engine/assets/MeshBuilder.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/EntityReferenceRemap.hpp"
#include "engine/scene/Prefab.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
using Json = nlohmann::json;

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

// A game module component referring to another entity.
struct FollowComponent {
    SceneEntityId target;
};

std::shared_ptr<const ComponentTypeRegistry> componentTypes() {
    auto types = std::make_shared<ComponentTypeRegistry>();
    expect(registerBuiltInComponentTypes(*types).success, "built-in component types should register");
    expect(types
               ->add(ComponentTypeBuilder<FollowComponent>("follow", "Follow")
                         .category("Gameplay")
                         .fields(std::move(Fields<FollowComponent>().add("target", &FollowComponent::target)))
                         .build())
               .success,
           "a component with an entity reference should register");
    return types;
}

std::filesystem::path testDirectory() {
    return std::filesystem::temp_directory_path() / "zenith_scoped_capture_tests";
}

// An editor with every migrated operator registered: with their scopes, or,
// for comparison, with every scope removed so they capture the whole scene.
struct ScopeFixture : EditorTestFixture {
    explicit ScopeFixture(bool scoped = true)
        : EditorTestFixture({.documentRoot = testDirectory()}) {
        document.setComponentTypes(componentTypes());
        OperatorRegistry registered;
        expect(registerObjectOperators(registered).success, "the object operators should register");
        expect(registerComponentOperators(registered, {.clipboard = clipboard, .layout = componentLayout}).success,
               "the component operators should register");
        expect(registerPlacementOperators(registered).success, "the placement operators should register");
        expect(registerModelOperators(registered).success, "the model operators should register");
        expect(registerPrefabOperators(registered).success, "the prefab operators should register");
        expect(registerEyedropperOperators(registered, eyedropper).success, "the eyedropper operators should register");
        for (const Operator* op : registered.operators()) {
            Operator copy = *op;
            if (!scoped) {
                copy.scope = {};
            }
            operators.add(std::move(copy));
        }
    }

    std::string serialized() {
        std::string output;
        expect(SceneDocument::serialize(scene, assets, document.options(), output).success,
               "the scene should serialize");
        return output;
    }

    void load(const std::string& text) {
        expect(SceneDocument::deserialize(text, assets, document.options(), scene).success,
               "the test scene should load");
    }

    SceneEntityId id(std::string_view name) const {
        for (const Entity entity : scene.registry().view<const NameComponent>()) {
            if (scene.registry().get<NameComponent>(entity).name == name) {
                return scene.id(entity);
            }
        }
        expect(false, "the test scene should have an entity named " + std::string(name));
        return {};
    }

    void select(std::initializer_list<std::string_view> names) {
        std::vector<SceneEntityId> ids;
        for (const std::string_view name : names) {
            ids.push_back(id(name));
        }
        selection.assign(std::move(ids));
    }

    OperatorResult run(std::string_view operatorId, const Json& properties = Json::object()) {
        return invoke(operatorId, properties.dump());
    }
};

// Collections, a hierarchy, references inside and outside it, a component
// no registry knows, a hidden object, and two instances of a prefab.
std::string buildTestScene() {
    ScopeFixture fixture;
    Scene& scene = fixture.scene;
    SceneCollections collections;
    SceneCollectionId props;
    SceneCollectionId lights;
    collections.create("Props", {}, props);
    collections.create("Lights", {}, lights);
    scene.setCollections(collections);

    const Entity group = scene.createEntity("Group");
    const Entity a = scene.createEntity("A");
    const Entity a1 = scene.createEntity("A1");
    const Entity b = scene.createEntity("B");
    scene.setParent(a, group);
    scene.setParent(a1, a);
    scene.setParent(b, group);
    scene.registry().emplace<PointLightComponent>(a);
    scene.registry().emplace<FollowComponent>(a, scene.id(b));
    scene.registry().emplace<FollowComponent>(b, scene.id(a1));
    scene.registry().emplace<PreservedComponentsComponent>(b).components.emplace_back("mystery", R"({"value":3})");
    linkToCollection(scene, b, props);
    // B before A: Group's document form then holds its child order.
    expect(static_cast<bool>(scene.setSiblingIndex(b, 0)), "B should move before A");

    const Entity lamp = scene.createEntity("Lamp");
    scene.registry().get<TransformComponent>(lamp).local[3] = glm::vec4(4.0f, 1.0f, 0.0f, 1.0f);
    scene.registry().emplace<PointLightComponent>(lamp);
    scene.registry().emplace<FollowComponent>(lamp, scene.id(group));
    linkToCollection(scene, lamp, lights);

    const Entity hidden = scene.createEntity("Hidden");
    scene.registry().emplace<VisibilityComponent>(hidden).hidden = true;

    const Entity crate = scene.createEntity("Crate");
    const Entity lid = scene.createEntity("Lid");
    scene.setParent(lid, crate);
    scene.registry().emplace<PointLightComponent>(lid);
    scene.updateWorldTransforms();
    PrefabAuthoring prefabs(fixture.assets, fixture.document.options());
    expect(prefabs.create(scene, crate, "prefabs/crate.prefab").success, "the crate should become a prefab");
    Entity second = entt::null;
    expect(prefabs.instantiate(scene, "prefabs/crate.prefab", entt::null, second).success,
           "the crate prefab should instantiate");
    scene.registry().get<NameComponent>(second).name = "Crate 2";
    return fixture.serialized();
}

// One migrated operator run: its selection and properties, chosen against
// the loaded scene, and anything to do first (outside the undo step).
struct OperatorCase {
    std::string operatorId;
    std::function<Json(ScopeFixture&)> prepare;
};

// Runs the case with and without scopes on the same scene: both must reach
// the same scene, record one step, and undo and redo to the same states, the
// undo exactly to the scene before.
void expectScopeParity(const std::string& scene, const OperatorCase& operatorCase) {
    ScopeFixture scoped(true);
    ScopeFixture whole(false);
    scoped.load(scene);
    whole.load(scene);
    const Json properties = operatorCase.prepare(scoped);
    expect(operatorCase.prepare(whole) == properties, operatorCase.operatorId + ": the runs should match");
    const std::string before = scoped.serialized();
    const size_t steps = scoped.history.size();

    const OperatorResult scopedResult = scoped.run(operatorCase.operatorId, properties);
    const OperatorResult wholeResult = whole.run(operatorCase.operatorId, properties);
    expect(scopedResult.status == OperatorStatus::Finished && wholeResult.status == OperatorStatus::Finished,
           operatorCase.operatorId + " should run: " + scopedResult.message);
    const std::string after = scoped.serialized();
    expect(after != before, operatorCase.operatorId + " should change the scene");
    expect(after == whole.serialized(), operatorCase.operatorId + " should edit the same way with a scope");
    expect(scoped.history.size() == steps + 1 && whole.history.size() == steps + 1,
           operatorCase.operatorId + " should record one undo step");

    scoped.editing().undo();
    whole.editing().undo();
    expect(scoped.serialized() == before, operatorCase.operatorId + ": undo should restore the scene exactly");
    expect(whole.serialized() == before, operatorCase.operatorId + ": undo of the whole-scene capture should too");
    scoped.editing().redo();
    whole.editing().redo();
    expect(scoped.serialized() == after && whole.serialized() == after,
           operatorCase.operatorId + ": redo should give the operator's result");
    expect(!scoped.diagnostics.hasUnacknowledgedError(), operatorCase.operatorId + " should report no error");
}

Json selected(ScopeFixture& fixture, std::initializer_list<std::string_view> names, Json properties = Json::object()) {
    fixture.select(names);
    return properties;
}

std::vector<OperatorCase> migratedOperators() {
    using Fixture = ScopeFixture;
    return {
        {"object.add_empty", [](Fixture&) { return Json{{"location", {1.0, 2.0, 3.0}}}; }},
        {"object.add_sun", [](Fixture&) { return Json::object(); }},
        {"object.add_empty_with", [](Fixture&) { return Json{{"component", "point_light"}}; }},
        {"object.create_entity", [](Fixture& f) { return selected(f, {"A"}, {{"placement", "child"}}); }},
        {"object.create_entity", [](Fixture& f) { return selected(f, {"A"}, {{"placement", "sibling"}}); }},
        {"object.duplicate", [](Fixture& f) { return selected(f, {"A"}); }},
        {"outliner.reorder",
         [](Fixture& f) { return Json{{"entity", f.id("A").value}, {"parent", f.id("Group").value}, {"index", 0}}; }},
        {"outliner.reorder",
         [](Fixture& f) {
             return Json{{"entity", f.id("Lamp").value}, {"parent", f.id("Group").value}, {"index", 1}};
         }},
        {"outliner.parent_drop", [](Fixture& f) { return selected(f, {"B"}, {{"parent", f.id("Lamp").value}}); }},
        {"collection.rename",
         [](Fixture& f) {
             const uint32_t props = static_cast<uint32_t>(f.scene.collections().all().front().id.value);
             return Json{{"collection", props}, {"name", "Renamed Props"}};
         }},
        {"object.array", [](Fixture& f) { return selected(f, {"A"}, {{"count", 2}}); }},
        {"object.duplicate", [](Fixture& f) { return selected(f, {"Group", "Lamp"}, {{"offset", {1.0, 0.0, 0.0}}}); }},
        {"object.duplicate", [](Fixture& f) { return selected(f, {"Crate 2", "A1"}); }},
        {"object.delete", [](Fixture& f) { return selected(f, {"Group"}); }},
        {"object.delete", [](Fixture& f) { return selected(f, {"Lid", "Lamp"}); }},
        {"object.location_clear", [](Fixture& f) { return selected(f, {"Lamp", "A"}); }},
        {"object.rotation_clear",
         [](Fixture& f) {
             f.scene.registry().get<TransformComponent>(f.scene.findEntity(f.id("Lamp"))).local[0][1] = 0.5f;
             return selected(f, {"Lamp"});
         }},
        {"object.rename", [](Fixture& f) { return selected(f, {"B"}, {{"name", "Renamed"}}); }},
        {"object.component_add", [](Fixture& f) { return selected(f, {"B", "A"}, {{"component", "point_light"}}); }},
        {"object.component_remove",
         [](Fixture& f) { return selected(f, {"A", "Lamp"}, {{"component", "point_light"}}); }},
        {"object.component_remove", [](Fixture& f) { return selected(f, {"B"}, {{"component", "mystery"}}); }},
        {"object.component_reset",
         [](Fixture& f) {
             f.scene.registry().get<PointLightComponent>(f.scene.findEntity(f.id("Lamp"))).range = 42.0f;
             return selected(f, {"Lamp", "A"}, {{"component", "point_light"}});
         }},
        {"object.component_copy_to_selected",
         [](Fixture& f) {
             f.scene.registry().get<PointLightComponent>(f.scene.findEntity(f.id("Lamp"))).range = 42.0f;
             return selected(f, {"Lamp", "B", "Group"}, {{"component", "point_light"}});
         }},
        {"object.component_paste",
         [](Fixture& f) {
             f.scene.registry().get<PointLightComponent>(f.scene.findEntity(f.id("Lamp"))).range = 42.0f;
             f.select({"Lamp"});
             expect(static_cast<bool>(f.run("object.component_copy", {{"component", "point_light"}})),
                    "the lamp's light should copy");
             return selected(f, {"A", "Lid"}, {{"component", "point_light"}});
         }},
        {"outliner.parent_drop", [](Fixture& f) { return selected(f, {"Lid"}, {{"parent", f.id("Lamp").value}}); }},
        {"outliner.parent_drop",
         [](Fixture& f) { return selected(f, {"A", "Lamp"}, {{"parent", 0}, {"keep_transform", false}}); }},
        {"object.set_visibility",
         [](Fixture& f) { return Json{{"entity", f.id("A1").value}, {"flag", "selectable"}, {"value", false}}; }},
        {"object.hide", [](Fixture& f) { return selected(f, {"A", "Lamp"}); }},
        {"object.unhide", [](Fixture& f) { return selected(f, {"A"}); }},
        {"object.array", [](Fixture& f) { return selected(f, {"Group", "Lamp"}, {{"count", 3}}); }},
        {"object.scatter", [](Fixture& f) { return selected(f, {"Lamp", "Crate"}, {{"count", 4}, {"seed", 7}}); }},
        {"collection.new", [](Fixture& f) { return selected(f, {"A", "Lamp"}, {{"name", "Fresh"}}); }},
        {"collection.link",
         [](Fixture& f) {
             const uint32_t props = static_cast<uint32_t>(f.scene.collections().all().front().id.value);
             return selected(f, {"A", "Lamp"}, {{"collection", props}});
         }},
        {"collection.unlink",
         [](Fixture& f) {
             const uint32_t props = static_cast<uint32_t>(f.scene.collections().all().front().id.value);
             return selected(f, {"B"}, {{"collection", props}});
         }},
        {"collection.set_flag",
         [](Fixture& f) {
             const uint32_t lights = static_cast<uint32_t>(f.scene.collections().all().back().id.value);
             return Json{{"collection", lights}, {"flag", "visible"}, {"value", false}};
         }},
        {"object.collection_instance_add",
         [](Fixture& f) {
             const uint32_t props = static_cast<uint32_t>(f.scene.collections().all().front().id.value);
             return Json{{"collection", props}, {"location", {0.0, 0.0, 5.0}}};
         }},
        {"prefab.instantiate",
         [](Fixture&) { return Json{{"asset", "prefabs/crate.prefab"}, {"location", {7.0, 0.0, 0.0}}}; }},
        {"prefab.unpack", [](Fixture& f) { return selected(f, {"Lid"}); }},
        {"ui.eyedropper_pick",
         [](Fixture& f) {
             expect(static_cast<bool>(
                        f.run("ui.eyedropper_entity",
                              {{"entity", f.id("Lamp").value}, {"component", "follow"}, {"field", "target"}})),
                    "the eyedropper should arm");
             return Json{{"entity", f.id("Hidden").value}};
         }},
    };
}

void testScopeParity(const std::string& scene) {
    for (const OperatorCase& operatorCase : migratedOperators()) {
        expectScopeParity(scene, operatorCase);
    }
}

// An entity that cannot be serialized: any whole-scene capture fails on it,
// so an operator that still runs did not capture the whole scene.
Entity addUnserializable(Scene& scene) {
    const Entity broken = scene.createEntity("Broken");
    scene.registry().remove<NameComponent>(broken);
    return broken;
}

void testScopedCaptureIgnoresTheRest(const std::string& scene) {
    ScopeFixture fixture;
    fixture.load(scene);
    const SceneEntityId lamp = fixture.id("Lamp");
    addUnserializable(fixture.scene);
    fixture.selection.selectOnly(lamp);
    expect(static_cast<bool>(fixture.run("object.rename", {{"name", "Torch"}})) && fixture.history.size() == 1,
           "a scoped operator should record without serializing the rest of the scene");
    expect(static_cast<bool>(fixture.run("object.delete")) && fixture.history.size() == 2 &&
               fixture.scene.findEntity(lamp) == entt::null,
           "delete should record only the deleted entities");
    fixture.editing().undo();
    expect(fixture.scene.findEntity(lamp) != entt::null &&
               fixture.scene.registry().get<NameComponent>(fixture.scene.findEntity(lamp)).name == "Torch",
           "undoing the delete should bring the renamed lamp back");
    fixture.diagnostics.acknowledgeErrors();
    fixture.selection.selectOnly(lamp);
    expect(!fixture.run("object.isolate"), "an operator without a scope still captures the whole scene");
}

// A runtime mesh cannot be serialized, so it blocks recording only the
// operators that touch its entity.
void testRuntimeMeshBlocksOnlyItsEntity(const std::string& scene) {
    ScopeFixture fixture;
    fixture.load(scene);
    const AssetLoadResult<MaterialHandle> material = fixture.assets.createMaterial(MaterialAsset{}, "procedural");
    expect(static_cast<bool>(material), "a runtime material should be created: " + material.error);
    MeshBuilder builder;
    builder.beginPrimitive(material.handle).box(glm::vec3(0.0f), glm::vec3(0.5f));
    MeshBuildResult built = builder.build();
    const AssetLoadResult<MeshHandle> runtime = fixture.assets.createMesh(std::move(built.mesh), "procedural");
    expect(static_cast<bool>(runtime), "a runtime mesh should be created: " + runtime.error);
    const Entity procedural = fixture.scene.createEntity("Procedural");
    fixture.scene.attachMesh(procedural, runtime.handle);

    fixture.select({"Lamp"});
    expect(static_cast<bool>(fixture.run("object.rename", {{"name", "Torch"}})) && fixture.history.size() == 1,
           "an operator on other entities should record beside a runtime mesh");
    fixture.selection.selectOnly(fixture.scene.id(procedural));
    expect(fixture.run("object.rename", {{"name", "Changed"}}).status == OperatorStatus::Failed &&
               fixture.scene.registry().get<NameComponent>(procedural).name == "Procedural" &&
               fixture.history.size() == 1,
           "an operator on the runtime mesh's entity should be refused unchanged");
    fixture.diagnostics.acknowledgeErrors();
    expect(fixture.run("object.isolate").status == OperatorStatus::Failed,
           "a whole-scene operator should still be refused beside a runtime mesh");
    fixture.diagnostics.acknowledgeErrors();
}

void testDeleteRecordsDescendants(const std::string& scene) {
    ScopeFixture fixture;
    fixture.load(scene);
    const SceneEntityId a1 = fixture.id("A1");
    const std::string original = fixture.serialized();
    // Captures of the other entities would fail on this one.
    const Entity broken = addUnserializable(fixture.scene);
    fixture.select({"A"});
    expect(static_cast<bool>(fixture.run("object.delete")) && fixture.scene.findEntity(a1) == entt::null,
           "deleting A should delete its child");
    fixture.editing().undo();
    expect(fixture.scene.findEntity(a1) != entt::null, "undo should restore the child");
    fixture.scene.destroyEntity(broken);
    expect(fixture.serialized() == original, "undo should restore A and its child exactly");

    // A prefab entity's removal is recorded on its instance root.
    fixture.select({"Lid"});
    const std::string before = fixture.serialized();
    expect(static_cast<bool>(fixture.run("object.delete")), "deleting the lid should work");
    const Entity crate = fixture.scene.findEntity(fixture.id("Crate"));
    expect(!fixture.scene.registry().get<PrefabInstanceComponent>(crate).overrides.empty(),
           "the crate should record the lid's removal");
    fixture.editing().undo();
    expect(fixture.serialized() == before, "undo should restore the lid and the crate's record");
}

void testAdjustLastOperation(const std::string& scene) {
    ScopeFixture fixture;
    fixture.load(scene);
    addUnserializable(fixture.scene);
    fixture.select({"Lamp"});
    expect(static_cast<bool>(fixture.run("object.duplicate", {{"offset", {1.0, 0.0, 0.0}}})),
           "duplicate should run beside an unserializable entity");
    const LastOperator* last = fixture.runner.lastOperator();
    expect(last != nullptr && last->after.entities.has_value(), "the runner should keep only the scope's result");
    OperatorProperties adjusted = fixture.operators.find("object.duplicate")->properties;
    adjusted.get<DuplicateProperties>()->offset = {0.0f, 0.0f, 3.0f};
    OperatorContext context = fixture.operatorContext();
    expect(static_cast<bool>(fixture.runner.adjustLast(context, adjusted)) && fixture.history.size() == 1,
           "Adjust Last Operation should re-run a scoped operator");
    const Entity copy = fixture.scene.findEntity(*fixture.selection.active());
    expect(copy != entt::null &&
               fixture.scene.registry().get<TransformComponent>(copy).local[3] == glm::vec4(4.0f, 1.0f, 3.0f, 1.0f),
           "the adjusted copy should use the new offset");

    // Changing the operator's result outside history blocks adjusting.
    fixture.scene.registry().get<NameComponent>(copy).name = "Changed";
    OperatorContext again = fixture.operatorContext();
    expect(!fixture.runner.adjustLast(again, adjusted) && fixture.history.size() == 1,
           "a result changed since the operator should not be adjusted");
    fixture.diagnostics.acknowledgeErrors();
}

void testPlaySkipsWholeCapture(const std::string& scene) {
    ScopeFixture fixture;
    fixture.load(scene);
    Scene parked;
    fixture.mode = EditorMode::Play;
    const SceneEditing live(fixture.scene, fixture.assets, fixture.document, fixture.history, fixture.diagnostics,
                            &parked);
    addUnserializable(fixture.scene);
    const SceneEntityId lamp = fixture.id("Lamp");
    fixture.selection.selectOnly(lamp);
    OperatorContext context{.editing = live, .selection = fixture.selection, .mode = EditorMode::Play};
    fixture.scene.retirePhysicsLifecycleEvents();
    expect(static_cast<bool>(
               fixture.runner.invokeWithJson("object.component_reset", context, R"({"component":"point_light"})")) &&
               fixture.history.size() == 0,
           "a scoped operator should edit the running scene without a whole-scene capture or a step");
    const Entity group = fixture.scene.findEntity(fixture.id("Group"));
    fixture.selection.selectOnly(fixture.scene.id(group));
    OperatorContext duplicating{.editing = live, .selection = fixture.selection, .mode = EditorMode::Play};
    fixture.scene.retirePhysicsLifecycleEvents();
    expect(static_cast<bool>(fixture.runner.invoke("object.duplicate", duplicating)) && fixture.history.size() == 0,
           "duplicating in Play should run live");
    expect(fixture.scene.findEntity(fixture.scene.id(group)) == group,
           "the original keeps its handle, so its runtime state is untouched");
    for (const PhysicsLifecycleEvent& event : fixture.scene.physicsLifecycleEvents()) {
        expect(event.entity != group && fixture.scene.id(event.entity).value >= parked.nextEntityId().value,
               "only the copies should be marked for physics");
    }
    expect(fixture.runner.lastOperator() == nullptr, "nothing in Play can be adjusted");
    fixture.diagnostics.acknowledgeErrors();
}

// The handles of every entity, by stable id.
std::map<uint64_t, Entity> handles(const Scene& scene) {
    std::map<uint64_t, Entity> result;
    for (const Entity entity : scene.registry().view<const SceneEntityIdComponent>()) {
        result.emplace(scene.id(entity).value, entity);
    }
    return result;
}

void testPasteKeepsOtherEntities(const std::string& scene) {
    ScopeFixture fixture;
    fixture.load(scene);
    const std::map<uint64_t, Entity> before = handles(fixture.scene);
    fixture.select({"Group", "Crate"});
    expect(static_cast<bool>(fixture.run("object.duplicate")), "duplicate should run");
    fixture.select({"Lamp"});
    expect(static_cast<bool>(fixture.run("object.array", {{"count", 5}})) && fixture.history.size() == 2,
           "an array of five should be one step");
    const std::map<uint64_t, Entity> after = handles(fixture.scene);
    for (const auto& [id, entity] : before) {
        const auto found = after.find(id);
        expect(found != after.end() && found->second == entity, "pasting should keep every other entity's handle");
    }
    // Group, A, A1, B, Crate, and Lid, then five lamps.
    expect(after.size() == before.size() + 6 + 5, "the copies should be added");
}

// The copy's document form must be the original's with every id inside the
// fragment mapped to its copy and everything else kept.
void testPasteParity(const std::string& scene) {
    ScopeFixture fixture;
    fixture.load(scene);
    Scene& live = fixture.scene;
    const std::vector<SceneEntityId> roots{fixture.id("Group"), fixture.id("Crate 2"), fixture.id("Lamp")};
    std::string fragment;
    expect(SceneFragment::copy(live, fixture.assets, fixture.document.options(), roots, fragment).success,
           "the selection should copy");
    const Json parsed = Json::parse(fragment);
    expect(parsed.at("entities").size() == 7, "the fragment should hold only the copied subtrees");
    expect(parsed.at("collections").size() == 2, "the fragment should name the collections it references");

    std::vector<SceneEntityId> pasted;
    expect(
        SceneFragment::paste(fragment, live, fixture.assets, fixture.document.options(), entt::null, pasted).success &&
            pasted.size() == roots.size(),
        "the fragment should paste");
    // Pair originals with copies by walking both subtrees in child order.
    std::unordered_map<uint64_t, uint64_t> copies;
    const auto pair = [&](const auto& self, Entity original, Entity copy) -> void {
        copies.emplace(live.id(original).value, live.id(copy).value);
        const auto& originalChildren = live.registry().get<RelationshipComponent>(original).children;
        const auto& copyChildren = live.registry().get<RelationshipComponent>(copy).children;
        expect(originalChildren.size() == copyChildren.size(), "a copy should have its original's children");
        for (size_t index = 0; index < originalChildren.size() && index < copyChildren.size(); ++index) {
            self(self, originalChildren[index], copyChildren[index]);
        }
    };
    for (size_t index = 0; index < roots.size() && index < pasted.size(); ++index) {
        pair(pair, live.findEntity(roots[index]), live.findEntity(pasted[index]));
    }
    std::vector<SceneEntityId> originalIds;
    std::vector<SceneEntityId> copyIds;
    for (const auto& [original, copy] : copies) {
        originalIds.push_back({original});
        copyIds.push_back({copy});
    }
    std::vector<std::optional<std::string>> originals;
    std::vector<std::optional<std::string>> pastedForms;
    const SceneDocumentOptions& options = fixture.document.options();
    SceneDocument::serializeEntities(live, fixture.assets, options, originalIds, originals);
    SceneDocument::serializeEntities(live, fixture.assets, options, copyIds, pastedForms);
    for (size_t index = 0; index < originals.size(); ++index) {
        Json expected = Json::parse(originals[index].value_or("{}"));
        remapEntityReferences(expected.at("components"), componentTypesFor(options),
                              [&copies](uint64_t id, const EntityReferenceSite&) -> std::optional<uint64_t> {
                                  const auto found = copies.find(id);
                                  return found == copies.end() ? std::nullopt : std::optional(found->second);
                              });
        remapChildOrder(expected, [&copies](uint64_t id) -> std::optional<uint64_t> {
            const auto found = copies.find(id);
            return found == copies.end() ? std::nullopt : std::optional(found->second);
        });
        expected["id"] = copyIds[index].value;
        // A copied root is renamed (Crate_1); its descendants keep their names.
        const Json pastedForm = Json::parse(pastedForms[index].value_or("{}"));
        const std::string originalName = expected.at("name").get<std::string>();
        const std::string copyName = pastedForm.value("name", std::string());
        expect(copyName == originalName || copyName.starts_with(originalName + "_"),
               "a copy keeps its original's name or numbers it");
        expected["name"] = copyName;
        if (!expected.at("parent").is_null()) {
            const auto parent = copies.find(expected.at("parent").get<uint64_t>());
            expected["parent"] = parent == copies.end() ? Json(nullptr) : Json(parent->second);
        }
        expect(Json::parse(pastedForms[index].value_or("{}")) == expected,
               "copy of entity " + std::to_string(originalIds[index].value) +
                   " should match its original with fragment ids remapped");
    }
    const Entity lampCopy = live.findEntity(pasted.back());
    // The Group is in the fragment, so the lamp's reference follows to its copy.
    expect(live.registry().get<FollowComponent>(lampCopy).target == pasted.front(),
           "a reference to an entity inside the fragment should follow to its copy");
    expect(prefabInstanceRoot(live, live.findEntity(pasted[1])) == live.findEntity(pasted[1]),
           "a pasted prefab instance should stay an instance");

    // Pasting into another scene matches collections by name.
    Scene other;
    SceneCollections otherCollections;
    SceneCollectionId otherLights;
    otherCollections.create("Unrelated", {}, otherLights);
    otherCollections.create("Lights", {}, otherLights);
    other.setCollections(otherCollections);
    std::vector<SceneEntityId> elsewhere;
    expect(SceneFragment::paste(fragment, other, fixture.assets, options, entt::null, elsewhere).success,
           "the fragment should paste into another scene");
    const Entity otherLamp = other.findEntity(elsewhere.back());
    expect(other.registry().get<CollectionMembershipComponent>(otherLamp).collections ==
               std::vector<SceneCollectionId>{otherLights},
           "collections should follow their names into another scene");
}

// Copies keep their originals' sibling order, at every level.
void testPasteKeepsSiblingOrder(const std::string& scene) {
    ScopeFixture fixture;
    fixture.load(scene);
    const auto childNames = [&fixture](SceneEntityId parent) {
        std::vector<std::string> names;
        for (const Entity child :
             fixture.scene.registry().get<RelationshipComponent>(fixture.scene.findEntity(parent)).children) {
            names.push_back(fixture.scene.registry().get<NameComponent>(child).name);
        }
        return names;
    };
    const SceneEntityId group = fixture.id("Group");
    const SceneEntityId b = fixture.id("B");
    const std::vector<std::string> order = childNames(group);
    expect(order == std::vector<std::string>{"B", "A"}, "the test group should list B before A");
    fixture.select({"Group"});
    expect(static_cast<bool>(fixture.run("object.duplicate")), "the group should duplicate");
    const SceneEntityId copy = *fixture.selection.active();
    expect(childNames(copy) == order, "a duplicated subtree should keep its sibling order");
    fixture.editing().undo();
    fixture.editing().redo();
    expect(childNames(copy) == order, "redoing the duplicate should keep the sibling order");

    std::string fragment;
    expect(
        SceneFragment::copy(fixture.scene, fixture.assets, fixture.document.options(), std::span(&group, 1), fragment)
            .success,
        "the group should copy");
    std::vector<SceneEntityId> pasted;
    expect(SceneFragment::paste(fragment, fixture.scene, fixture.assets, fixture.document.options(), entt::null, pasted)
                   .success &&
               childNames(pasted.front()) == order,
           "a pasted subtree should keep its sibling order");

    // A copy joining a reordered parent goes last and keeps the order.
    // By id: the copies above have the same names.
    fixture.selection.selectOnly(b);
    expect(static_cast<bool>(fixture.run("object.duplicate")), "B should duplicate");
    std::string joined;
    for (const std::string& name : childNames(group)) {
        joined += name + " ";
    }
    expect(childNames(group) == std::vector<std::string>{"B", "A", "B_1"},
           "the copy should join after its siblings, not " + joined);
    fixture.editing().undo();
    expect(childNames(group) == order, "undo should restore the group's order");
}

void testLargeSceneCopyIsLocal() {
    ScopeFixture fixture;
    Scene& scene = fixture.scene;
    for (int index = 0; index < 2'000; ++index) {
        scene.createEntity("Filler " + std::to_string(index));
    }
    const Entity crate = scene.createEntity("Crate");
    addUnserializable(scene);
    const SceneEntityId crateId = scene.id(crate);
    std::string fragment;
    expect(SceneFragment::copy(scene, fixture.assets, fixture.document.options(), std::span(&crateId, 1), fragment)
               .success,
           "copy should serialize only the selection, so an unrelated unserializable entity does not matter");
    std::vector<SceneEntityId> pasted;
    expect(
        SceneFragment::paste(fragment, scene, fixture.assets, fixture.document.options(), entt::null, pasted).success &&
            pasted.size() == 1,
        "paste should not rebuild the scene from its document");
}
} // namespace

int main() {
    std::error_code error;
    std::filesystem::remove_all(testDirectory(), error);
    std::filesystem::create_directories(testDirectory(), error);
    const std::string scene = buildTestScene();
    testScopeParity(scene);
    testScopedCaptureIgnoresTheRest(scene);
    testDeleteRecordsDescendants(scene);
    testRuntimeMeshBlocksOnlyItsEntity(scene);
    testAdjustLastOperation(scene);
    testPlaySkipsWholeCapture(scene);
    testPasteKeepsOtherEntities(scene);
    testPasteParity(scene);
    testPasteKeepsSiblingOrder(scene);
    testLargeSceneCopyIsLocal();
    std::filesystem::remove_all(testDirectory(), error);
    return failures == 0 ? 0 : 1;
}
