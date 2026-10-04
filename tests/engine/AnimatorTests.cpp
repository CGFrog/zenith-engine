// The animation state machine: entry, parameter conditions, triggers,
// any-state transitions, exit times, cross-fades, events across loops, its
// validation, and its scene JSON.

#include "engine/animation/AnimationSystem.hpp"
#include "engine/animation/Animator.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/module/BuiltInSystems.hpp"
#include "engine/module/Gameplay.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/Scene.hpp"
#include "engine/scene/SceneDocument.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
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

std::vector<AnimationClip> clips() {
    return {{.name = "Idle", .duration = 1.0f}, {.name = "Walk", .duration = 0.5f}, {.name = "Jump", .duration = 0.8f}};
}

// Idle <-> Walk on a speed parameter, Jump from any state on a trigger, back
// to Idle at the end of the jump, and a footstep event in Walk.
AnimatorDefinition locomotion() {
    return {.parameters = {{.name = "speed", .type = AnimatorParameterType::Float},
                           {.name = "jump", .type = AnimatorParameterType::Trigger},
                           {.name = "crouched", .type = AnimatorParameterType::Bool}},
            .states = {{.name = "Idle", .clip = "Idle"},
                       {.name = "Walk", .clip = "Walk", .speed = 2.0f},
                       {.name = "Jump", .clip = "Jump", .looping = false}},
            .transitions =
                {{.from = "Idle",
                  .to = "Walk",
                  .conditions = {{.parameter = "speed", .mode = AnimatorConditionMode::Greater, .threshold = 0.1f}},
                  .crossFadeSeconds = 0.25f},
                 {.from = "Walk",
                  .to = "Idle",
                  .conditions = {{.parameter = "speed", .mode = AnimatorConditionMode::Less, .threshold = 0.1f}}},
                 {.to = "Jump",
                  .conditions = {{.parameter = "jump", .mode = AnimatorConditionMode::Triggered}},
                  .crossFadeSeconds = 0.1f},
                 {.from = "Jump", .to = "Idle", .exitTime = 1.0f}},
            .events = {{.state = "Walk", .time = 0.2f, .name = "footstep"}},
            .entryState = "Idle"};
}

struct Rig {
    AnimatorComponent animator{.definition = locomotion()};
    AnimationComponent animation;
    std::vector<AnimationClip> available = clips();
    std::vector<std::string> events;

    bool step(float seconds) {
        events.clear();
        return updateAnimator(animator, animation, available, seconds, events);
    }
};

void testEntryAndConditions() {
    Rig rig;
    expect(validateAnimatorDefinition(rig.animator.definition).empty(), "the locomotion animator is valid");
    expect(rig.step(0.1f) && rig.animator.currentState == "Idle" && rig.animation.clipIndex == 0 &&
               !rig.animation.blendClipIndex && std::abs(rig.animator.stateTime - 0.1f) < 1.0e-5f,
           "an animator enters its entry state without a cross-fade");

    rig.step(0.1f);
    expect(rig.animator.currentState == "Idle", "an animator stays while no transition holds");

    expect(rig.animator.setFloat("speed", 1.0f) && !rig.animator.setFloat("missing", 1.0f),
           "parameters are set by name");
    rig.step(0.1f);
    expect(rig.animator.currentState == "Walk" && rig.animation.blendClipIndex == 1u &&
               rig.animation.blendDuration == 0.25f && rig.animation.playbackSpeed == 2.0f,
           "a float condition moves to its state, cross-fading the clip");
    expect(std::abs(rig.animator.stateTime - 0.2f) < 1.0e-5f, "state time advances at the state's speed");

    rig.animator.setFloat("speed", 0.0f);
    rig.step(0.1f);
    expect(rig.animator.currentState == "Idle", "a less-than condition moves back");
}

void testTriggersAndExitTime() {
    Rig rig;
    rig.step(0.1f);
    expect(!rig.animator.setTrigger("speed") && rig.animator.setTrigger("jump"),
           "only trigger parameters can be triggered");
    rig.step(0.1f);
    expect(rig.animator.currentState == "Jump" && rig.animator.parameter("jump") == 0.0f && !rig.animation.looping,
           "a trigger fires an any-state transition and is consumed");
    rig.step(0.1f);
    expect(rig.animator.currentState == "Jump", "an any-state transition does not re-enter its own state");

    rig.step(0.5f);
    expect(rig.animator.currentState == "Jump", "an exit-time transition waits for its clip to play");
    rig.step(0.3f);
    rig.step(0.01f);
    expect(rig.animator.currentState == "Idle", "an exit-time transition leaves once the clip has played");
}

void testEventsAcrossLoops() {
    Rig rig;
    rig.step(0.0f);
    rig.animator.setFloat("speed", 1.0f);
    // Enters Walk and plays 0.1 s of it at speed 2: 0.2 clip seconds, so the
    // footstep at 0.2 is not yet passed.
    rig.step(0.1f);
    expect(rig.animator.currentState == "Walk" && rig.events.empty(), "no event before its time");
    rig.step(0.05f);
    expect(rig.events == std::vector<std::string>{"footstep"}, "an event fires when playback passes its time");
    rig.step(0.05f);
    expect(rig.events.empty(), "an event fires once per pass");
    // From 0.4 to 1.4 clip seconds passes the footstep at 0.7 and 1.2.
    rig.step(0.5f);
    expect(rig.events.size() == 2, "a looping state fires its event on every loop it passes");
}

void testMissingClipsAndValidation() {
    Rig rig;
    rig.available = {{.name = "Idle", .duration = 1.0f}};
    expect(!rig.step(0.1f) && rig.animator.currentState.empty(), "an animator whose clip is missing does nothing");

    AnimatorDefinition definition = locomotion();
    definition.transitions.push_back({.from = "Idle", .to = "Nowhere", .exitTime = 0.5f});
    expect(!validateAnimatorDefinition(definition).empty(), "a transition to a missing state is invalid");
    definition = locomotion();
    definition.transitions.push_back({.from = "Idle", .to = "Walk"});
    expect(!validateAnimatorDefinition(definition).empty(), "a transition with nothing to wait for is invalid");
    definition = locomotion();
    definition.transitions.front().conditions.front().mode = AnimatorConditionMode::Triggered;
    expect(!validateAnimatorDefinition(definition).empty(), "a condition must fit its parameter's type");
    definition = locomotion();
    definition.states.push_back({.name = "Idle", .clip = "Walk"});
    expect(!validateAnimatorDefinition(definition).empty(), "state names are unique");
}

void testSceneDocument() {
    const std::shared_ptr<const ComponentTypeRegistry> types = builtInComponentTypes();
    AssetManager assets;
    Scene scene;
    const Entity entity = scene.createEntity("Character");
    AnimatorComponent animator{.definition = locomotion()};
    animator.currentState = "Walk";
    scene.registry().emplace<AnimatorComponent>(entity, animator);
    std::string text;
    expect(SceneDocument::serialize(scene, assets, {.componentTypes = types}, text).success, "an animator serializes");
    Scene loaded;
    expect(SceneDocument::deserialize(text, assets, {.componentTypes = types}, loaded).success,
           "an animator deserializes");
    const Entity reloaded = loaded.findEntity(scene.id(entity));
    const auto* read = reloaded != entt::null ? loaded.registry().try_get<AnimatorComponent>(reloaded) : nullptr;
    expect(read != nullptr && read->definition.states.size() == 3 && read->definition.transitions.size() == 4 &&
               read->definition.transitions[3].exitTime == 1.0f && !read->definition.transitions[0].exitTime &&
               read->definition.transitions[2].conditions[0].mode == AnimatorConditionMode::Triggered &&
               read->definition.parameters[1].type == AnimatorParameterType::Trigger &&
               read->definition.events[0].name == "footstep" && read->definition.entryState == "Idle" &&
               read->currentState.empty(),
           "an animator's definition round-trips and its runtime state does not");

    const std::string invalid =
        R"({"version":1,"entities":[{"id":1,"name":"a","local_transform":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],)"
        R"("parent":null,"components":{"animator":{"states":[{"name":"A","clip":"A"}],)"
        R"("transitions":[{"from":"A","to":"B","exit_time":1}]}}}]})";
    Scene rejected;
    expect(!SceneDocument::deserialize(invalid, assets, {.componentTypes = types}, rejected).success,
           "a scene with an invalid animator fails to load");
}

void testSystemSchedule() {
    const Gameplay gameplay = builtInGameplay();
    const std::vector<std::string> update = gameplay.systems->schedule(SystemPhase::Update, SimulationMode::Play);
    expect(!update.empty() && update.back() == EngineSystems::Animator,
           "the engine's animator system runs after the other Update systems");
}
} // namespace

int main() {
    testEntryAndConditions();
    testTriggersAndExitTime();
    testEventsAcrossLoops();
    testMissingClipsAndValidation();
    testSceneDocument();
    testSystemSchedule();
    return failures == 0 ? 0 : 1;
}
