#pragma once

// AnimatorComponent state machine: parameters, states, transitions, events; updateAnimator.

#include "engine/scene/SceneEntityId.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class AssetManager;
class EventQueue;
class Scene;
struct AnimationClip;
struct AnimationComponent;

// A value an animator's transitions test. Bool and trigger parameters are
// true when their value is not zero; a trigger is reset when a transition it
// enabled fires.
enum class AnimatorParameterType {
    Float,
    Bool,
    Trigger,
};

struct AnimatorParameter {
    std::string name;
    AnimatorParameterType type{AnimatorParameterType::Float};
    float value{0.0f};
};

// A state plays one clip of the entity's model, by clip name, at a speed.
struct AnimatorState {
    std::string name;
    std::string clip;
    float speed{1.0f};
    bool looping{true};
};

// How a condition tests its parameter: Greater and Less compare a float with
// the threshold, True and False test a bool, Triggered tests a trigger.
enum class AnimatorConditionMode {
    Greater,
    Less,
    True,
    False,
    Triggered,
};

struct AnimatorCondition {
    std::string parameter;
    AnimatorConditionMode mode{AnimatorConditionMode::True};
    float threshold{0.0f};
};

// Moves from a state (any state when from is empty) to another when every
// condition holds and, with an exit time, once the source state has played
// that many clip lengths (0.9 is 90% of the first loop). The clips cross-fade
// over crossFadeSeconds.
struct AnimatorTransition {
    std::string from;
    std::string to;
    std::vector<AnimatorCondition> conditions;
    float crossFadeSeconds{0.2f};
    std::optional<float> exitTime;
};

// Published (as AnimationEvent) when a state's playback passes time seconds
// into its clip, on every loop.
struct AnimatorEventMarker {
    std::string state;
    float time{0.0f};
    std::string name;
};

// A state machine over the entity's AnimationComponent, defined in scene JSON
// (the "animator" component) or in code. The engine's zenith.animator system
// runs it each Play frame before the animations advance.
struct AnimatorDefinition {
    std::vector<AnimatorParameter> parameters;
    std::vector<AnimatorState> states;
    std::vector<AnimatorTransition> transitions;
    std::vector<AnimatorEventMarker> events;
    // The state the animator starts in; empty means the first state.
    std::string entryState;
};

struct AnimatorComponent {
    AnimatorDefinition definition;

    // Runtime state, not serialized: the current state (empty until the
    // animator first runs) and how long it has played, in clip seconds.
    std::string currentState;
    float stateTime{0.0f};

    // Set a parameter by name; false when the animator has no parameter of
    // that name (and, for setTrigger, of the trigger type).
    bool setFloat(std::string_view name, float value);
    bool setBool(std::string_view name, bool value);
    bool setTrigger(std::string_view name);
    // The parameter's value, or nullopt when there is none of that name.
    std::optional<float> parameter(std::string_view name) const;
};

// The event an animator publishes to the session's EventQueue.
struct AnimationEvent {
    static constexpr std::string_view eventKey = "zenith.animation_event";
    SceneEntityId entity;
    std::string state;
    std::string name;
};

// Empty when the definition is usable, otherwise its first problem: a state
// without a name or clip, a duplicate state or parameter, a transition or
// event naming a missing state or parameter, a condition that does not fit its
// parameter's type, or a transition with neither conditions nor an exit time.
std::string validateAnimatorDefinition(const AnimatorDefinition& definition);

// Advances one animator by deltaSeconds against its model's clips: enters the
// entry state on the first call, fires at most one transition (cross-fading
// animation to the target clip), advances the state time, and appends the
// names of the event markers passed to events. Returns false, changing
// nothing, when a state names a clip clips lacks.
bool updateAnimator(AnimatorComponent& animator, AnimationComponent& animation, std::span<const AnimationClip> clips,
                    float deltaSeconds, std::vector<std::string>& events);

// Puts the animator in the named state at once, without a cross-fade: the
// animation plays the state's clip from its start at the state's speed and
// looping, and the state time restarts (an editor preview, a respawn). Returns
// false, changing nothing, when there is no such state or clips lacks its clip.
bool enterAnimatorState(AnimatorComponent& animator, AnimationComponent& animation,
                        std::span<const AnimationClip> clips, std::string_view state);

// Runs updateAnimator for every entity with an animator, an animation, and a
// model, and publishes the events as AnimationEvent to events when it is set.
void updateAnimators(Scene& scene, const AssetManager& assets, EventQueue* events, float deltaSeconds);
