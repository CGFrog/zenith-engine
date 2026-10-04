// Behaviours: registration as component types, the order and lifecycle of
// their hooks, deferred destruction from a hook, trigger and collision hooks,
// the modes they run in, and copies.

#include "engine/assets/Assets.hpp"
#include "engine/graphics/DebugDraw.hpp"
#include "engine/module/BuiltInSystems.hpp"
#include "engine/module/GameServices.hpp"
#include "engine/module/Gameplay.hpp"
#include "engine/module/ModuleRegistrar.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/SceneDocument.hpp"

#include <algorithm>
#include <iostream>
#include <optional>
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

// Every hook call, as "<behaviour> <entity name> <hook>".
std::vector<std::string> calls;

std::string nameOf(const BehaviourContext& context, Entity entity) {
    const auto* name = context.scene.registry().try_get<NameComponent>(entity);
    return name != nullptr ? name->name : "?";
}

// Records each hook; destroys its entity in onUpdate when asked to.
template <char Tag> class Recorder : public Behaviour {
  public:
    int32_t updates{0};
    bool destroyOnUpdate{false};
    bool spawnOnUpdate{false};

    void onCreate(BehaviourContext& context) override {
        record(context, "create");
    }

    void onStart(BehaviourContext& context) override {
        record(context, "start");
    }

    void onUpdate(BehaviourContext& context) override {
        ++updates;
        record(context, "update");
        if (destroyOnUpdate && context.services != nullptr) {
            context.services->commands().destroy(context.scene.id(context.entity));
        }
        if (spawnOnUpdate && context.services != nullptr) {
            spawnOnUpdate = false;
            context.services->commands().spawnEmpty(
                "Spawned", {}, [](Scene& scene, Entity entity) { scene.registry().emplace<Recorder<'A'>>(entity); });
        }
    }

    void onFixedUpdate(BehaviourContext& context) override {
        record(context, "fixed");
    }

    void onLateUpdate(BehaviourContext& context) override {
        record(context, "late");
    }

    void onTriggerEnter(BehaviourContext& context, Entity other) override {
        record(context, "enter " + nameOf(context, other));
    }

    void onTriggerExit(BehaviourContext& context, Entity other) override {
        record(context, "exit " + nameOf(context, other));
    }

    void onCollision(BehaviourContext& context, const BehaviourCollision& collision) override {
        record(context, "collide " + nameOf(context, collision.other) + (collision.normal.x > 0.0f ? " +x" : " -x"));
    }

    void onEnable(BehaviourContext& context) override {
        record(context, "enable");
    }

    void onDisable(BehaviourContext& context) override {
        record(context, "disable");
    }

    void onDestroy(BehaviourContext& context) override {
        record(context, "destroy");
    }

  private:
    void record(const BehaviourContext& context, const std::string& hook) const {
        calls.push_back(std::string(1, Tag) + " " + nameOf(context, context.entity) + " " + hook);
    }
};

using RecorderA = Recorder<'A'>;
using RecorderB = Recorder<'B'>;

// Runs only in Edit mode as well as Play.
class EditTicker : public Behaviour {
  public:
    int32_t ticks{0};

    void onUpdate(BehaviourContext&) override {
        ++ticks;
    }
};

const BehaviourType* registeredA = nullptr;

void registerBehaviours(ModuleRegistrar& registrar) {
    // Registered B first: hooks run in registration order of the types.
    registrar.addBehaviour<RecorderB>("recorder_b", "Recorder B");
    registrar.addBehaviour<RecorderA>("recorder_a", "Recorder A",
                                      Fields<RecorderA>().add("destroy_on_update", &RecorderA::destroyOnUpdate));
    registrar.addBehaviour<EditTicker>("edit_ticker", "Edit Ticker",
                                       Fields<EditTicker>().add("ticks", &EditTicker::ticks),
                                       {.runIn = RunIn::EditAndPlay, .category = "Tools"});
}

struct Fixture {
    Fixture() {
        const GameplayResult linked = linkGameplay(registerBehaviours, gameplay);
        expect(linked.success, "behaviours register through a module registrar");
        if (!linked) {
            std::cerr << linked.error << '\n';
        }
        services.emplace(assets, SceneDocumentOptions{.componentTypes = gameplay.componentTypes}, debugDraw, &logger,
                         gameplay.behaviours);
        calls.clear();
    }

    void run(SystemPhase phase, SimulationMode mode = SimulationMode::Play) {
        runGameplayPhase(*gameplay.systems, phase,
                         {.scene = scene,
                          .mode = mode,
                          .deltaSeconds = 0.016f,
                          .logger = &logger,
                          .services = mode == SimulationMode::Edit ? nullptr : &*services,
                          .behaviours = gameplay.behaviours.get()});
    }

    Entity add(std::string name) {
        return scene.createEntity(std::move(name));
    }

    Gameplay gameplay;
    Logger logger;
    AssetManager assets{&logger};
    DebugDraw debugDraw;
    Scene scene;
    std::optional<GameServices> services;
};

bool sameCalls(const std::vector<std::string>& expected) {
    if (calls == expected) {
        return true;
    }
    std::cerr << "Calls were:\n";
    for (const std::string& call : calls) {
        std::cerr << "  " << call << '\n';
    }
    return false;
}

void testRegistration() {
    Fixture fixture;
    const BehaviourRegistry& behaviours = *fixture.gameplay.behaviours;
    expect(behaviours.types().size() == 3 && behaviours.types()[0].key == "recorder_b" &&
               behaviours.types()[1].key == "recorder_a",
           "behaviour types keep their registration order");
    registeredA = behaviours.find("recorder_a");
    expect(registeredA != nullptr && registeredA->source.file.find("BehaviourTests.cpp") != std::string::npos &&
               registeredA->source.line > 0,
           "a behaviour records where it was registered");

    const ComponentType* type = fixture.gameplay.componentTypes->find("recorder_a");
    expect(type != nullptr && type->category() == "Behaviours" && type->fields().size() == 2 &&
               type->fields()[0].key == "enabled" && type->fields()[1].key == "destroy_on_update",
           "a behaviour is a component type with an enabled field before its own fields");
    expect(fixture.gameplay.componentTypes->find("edit_ticker")->category() == "Tools",
           "a behaviour may choose its category");

    // Behaviours serialize like any component.
    const Entity entity = fixture.add("Saved");
    auto& recorder = fixture.scene.registry().emplace<RecorderA>(entity);
    recorder.enabled = false;
    recorder.destroyOnUpdate = true;
    std::string text;
    const SceneDocumentOptions options{.componentTypes = fixture.gameplay.componentTypes};
    expect(SceneDocument::serialize(fixture.scene, fixture.assets, options, text).success, "behaviours serialize");
    Scene loaded;
    expect(SceneDocument::deserialize(text, fixture.assets, options, loaded).success, "behaviours deserialize");
    const Entity reloaded = loaded.findEntity(fixture.scene.id(entity));
    const auto* restored = reloaded != entt::null ? loaded.registry().try_get<RecorderA>(reloaded) : nullptr;
    expect(restored != nullptr && !restored->enabled && restored->destroyOnUpdate && !restored->created(),
           "a loaded behaviour keeps its fields and starts uncreated");

    const SystemScheduler& systems = *fixture.gameplay.systems;
    expect(systems.schedule(SystemPhase::Update, SimulationMode::Play) ==
               std::vector<std::string>{std::string(EngineSystems::Behaviours), std::string(EngineSystems::Animator)},
           "the engine's behaviour system runs Update behaviours");
}

void testLifecycleOrder() {
    Fixture fixture;
    const Entity first = fixture.add("First");
    const Entity second = fixture.add("Second");
    // A on both, B on the second: types run in registration order (B, A),
    // then entities in creation order.
    fixture.scene.registry().emplace<RecorderA>(second);
    fixture.scene.registry().emplace<RecorderA>(first);
    fixture.scene.registry().emplace<RecorderB>(second);

    fixture.run(SystemPhase::Update);
    expect(sameCalls({"B Second create", "B Second enable", "A First create", "A First enable", "A Second create",
                      "A Second enable", "B Second start", "A First start", "A Second start", "B Second update",
                      "A First update", "A Second update"}),
           "the first pass creates, enables, and starts every behaviour before updating");

    calls.clear();
    fixture.run(SystemPhase::Update);
    fixture.run(SystemPhase::FixedUpdate);
    fixture.run(SystemPhase::LateUpdate);
    expect(sameCalls({"B Second update", "A First update", "A Second update", "B Second fixed", "A First fixed",
                      "A Second fixed", "B Second late", "A First late", "A Second late"}),
           "later passes run only the phase's hook, and onStart runs once");

    calls.clear();
    fixture.scene.registry().get<RecorderA>(first).enabled = false;
    fixture.run(SystemPhase::Update);
    fixture.scene.registry().get<RecorderA>(first).enabled = true;
    fixture.run(SystemPhase::Update);
    expect(sameCalls({"A First disable", "B Second update", "A Second update", "A First enable", "B Second update",
                      "A First update", "A Second update"}),
           "disabling stops a behaviour's updates and enabling resumes them without a second start");

    calls.clear();
    const Entity late = fixture.add("Late");
    auto& disabled = fixture.scene.registry().emplace<RecorderA>(late);
    disabled.enabled = false;
    fixture.run(SystemPhase::Update);
    fixture.run(SystemPhase::Update);
    expect(sameCalls({"A Late create", "B Second update", "A First update", "A Second update", "B Second update",
                      "A First update", "A Second update"}),
           "a behaviour created disabled is created but not enabled or started");
}

void testDestroyAndSpawnFromHooks() {
    Fixture fixture;
    const Entity doomed = fixture.add("Doomed");
    const Entity child = fixture.add("Child");
    fixture.scene.setParent(child, doomed);
    const Entity survivor = fixture.add("Survivor");
    fixture.scene.registry().emplace<RecorderA>(doomed).destroyOnUpdate = true;
    fixture.scene.registry().emplace<RecorderB>(child);
    auto& spawner = fixture.scene.registry().emplace<RecorderA>(survivor);
    spawner.spawnOnUpdate = true;
    fixture.run(SystemPhase::Update);

    const std::vector<std::string> expected{
        "B Child create",    "B Child enable",    "A Doomed create", "A Doomed enable",
        "A Survivor create", "A Survivor enable", "B Child start",   "A Doomed start",
        "A Survivor start",  "B Child update",    "A Doomed update", "A Survivor update",
        "A Doomed disable",  "A Doomed destroy",  "B Child disable", "B Child destroy"};
    expect(sameCalls(expected),
           "a destroy recorded in a hook applies after the pass, after onDisable and onDestroy of the subtree");
    expect(fixture.scene.findEntity(fixture.scene.id(survivor)) != entt::null &&
               !fixture.scene.registry().valid(doomed) && !fixture.scene.registry().valid(child),
           "the destroyed subtree is gone and the rest of the scene stays");

    calls.clear();
    fixture.run(SystemPhase::Update);
    expect(
        sameCalls({"A Spawned create", "A Spawned enable", "A Spawned start", "A Survivor update", "A Spawned update"}),
        "an entity spawned from a hook runs its behaviours from the next pass");
}

void testTriggerAndCollisionHooks() {
    Fixture fixture;
    const Entity sensor = fixture.add("Sensor");
    const Entity visitor = fixture.add("Visitor");
    const Entity wall = fixture.add("Wall");
    fixture.scene.registry().emplace<RecorderA>(sensor);
    fixture.scene.registry().emplace<RecorderA>(visitor);
    fixture.scene.registry().emplace<RecorderB>(wall);
    fixture.run(SystemPhase::FixedUpdate);
    calls.clear();

    const TriggerEvent entered{.type = TriggerEventType::Entered,
                               .sensor = sensor,
                               .other = visitor,
                               .sensorId = fixture.scene.id(sensor),
                               .otherId = fixture.scene.id(visitor)};
    const ContactEvent touched{
        .type = ContactEventType::Begin, .first = visitor, .second = wall, .normal = {1.0f, 0.0f, 0.0f}};
    const ContactEvent persisted{
        .type = ContactEventType::Persist, .first = visitor, .second = wall, .normal = {1.0f, 0.0f, 0.0f}};
    const TriggerEvent triggers[] = {entered};
    const ContactEvent contacts[] = {touched, persisted};
    fixture.services->publishPhysicsEvents(fixture.scene, triggers, contacts);
    fixture.run(SystemPhase::FixedUpdate);
    expect(sameCalls({"A Sensor enter Visitor", "A Visitor enter Sensor", "A Visitor collide Wall +x",
                      "B Wall collide Visitor -x", "B Wall fixed", "A Sensor fixed", "A Visitor fixed"}),
           "trigger and collision hooks run on both sides before the next fixed update");

    calls.clear();
    fixture.run(SystemPhase::FixedUpdate);
    expect(sameCalls({"B Wall fixed", "A Sensor fixed", "A Visitor fixed"}), "physics hooks run once per event");

    calls.clear();
    fixture.scene.registry().get<RecorderA>(visitor).enabled = false;
    const TriggerEvent exited{.type = TriggerEventType::Exited,
                              .sensor = sensor,
                              .other = visitor,
                              .sensorId = fixture.scene.id(sensor),
                              .otherId = fixture.scene.id(visitor)};
    const TriggerEvent exits[] = {exited};
    fixture.services->publishPhysicsEvents(fixture.scene, exits, {});
    fixture.run(SystemPhase::FixedUpdate);
    expect(sameCalls({"A Visitor disable", "A Sensor exit Visitor", "B Wall fixed", "A Sensor fixed"}),
           "a disabled behaviour gets no trigger hooks");
}

void testModes() {
    Fixture fixture;
    const Entity entity = fixture.add("Tool");
    fixture.scene.registry().emplace<EditTicker>(entity);
    fixture.scene.registry().emplace<RecorderA>(entity);
    fixture.run(SystemPhase::Update, SimulationMode::Edit);
    expect(fixture.scene.registry().get<EditTicker>(entity).ticks == 1 && calls.empty(),
           "Edit mode runs only the behaviours that opt into it");

    // Simulate runs the behaviour systems the Simulate mask enables:
    // FixedUpdate by default.
    const SystemScheduler& systems = *fixture.gameplay.systems;
    const std::vector<std::string> simulateUpdate = systems.schedule(SystemPhase::Update, SimulationMode::Simulate);
    const std::vector<std::string> simulateFixed = systems.schedule(SystemPhase::FixedUpdate, SimulationMode::Simulate);
    expect(std::ranges::find(simulateUpdate, EngineSystems::Behaviours) == simulateUpdate.end() &&
               std::ranges::find(simulateFixed, EngineSystems::FixedBehaviours) != simulateFixed.end(),
           "Simulate runs fixed-update behaviours and not update behaviours by default");
}

void testCopiesAreNewBehaviours() {
    Fixture fixture;
    const Entity original = fixture.add("Original");
    fixture.scene.registry().emplace<RecorderA>(original);
    fixture.run(SystemPhase::Update);
    auto& behaviour = fixture.scene.registry().get<RecorderA>(original);
    expect(behaviour.created() && behaviour.started() && behaviour.updates == 1, "the original has run");

    const RecorderA copy = behaviour;
    expect(!copy.created() && !copy.started() && copy.updates == 1, "a copy keeps fields but not lifecycle state");
    RecorderA assigned;
    assigned = copy;
    behaviour = copy;
    expect(behaviour.created() && behaviour.started(), "assigning fields keeps the target's lifecycle state");

    const Entity duplicate = fixture.add("Duplicate");
    // As duplicating an entity does: a copy of the component.
    const RecorderA source = fixture.scene.registry().get<RecorderA>(original);
    fixture.scene.registry().emplace<RecorderA>(duplicate, source);
    calls.clear();
    fixture.run(SystemPhase::Update);
    expect(sameCalls({"A Duplicate create", "A Duplicate enable", "A Duplicate start", "A Original update",
                      "A Duplicate update"}),
           "a duplicated behaviour is created and started on its own");
}
} // namespace

int main() {
    testRegistration();
    testLifecycleOrder();
    testDestroyAndSpawnFromHooks();
    testTriggerAndCollisionHooks();
    testModes();
    testCopiesAreNewBehaviours();
    return failures == 0 ? 0 : 1;
}
