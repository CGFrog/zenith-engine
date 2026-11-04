#include "Animator.hpp"

#include "AnimationSystem.hpp"
#include "engine/module/EventQueue.hpp"
#include "engine/scene/Scene.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace {
template <typename Item> const Item* findNamed(const std::vector<Item>& items, std::string_view name) {
    const auto found = std::ranges::find_if(items, [name](const Item& item) { return item.name == name; });
    return found != items.end() ? &*found : nullptr;
}

template <typename Item> Item* findNamed(std::vector<Item>& items, std::string_view name) {
    const auto found = std::ranges::find_if(items, [name](const Item& item) { return item.name == name; });
    return found != items.end() ? &*found : nullptr;
}

std::optional<uint32_t> clipIndex(std::span<const AnimationClip> clips, std::string_view name) {
    for (uint32_t index = 0; index < clips.size(); ++index) {
        if (clips[index].name == name) {
            return index;
        }
    }
    return std::nullopt;
}

bool holds(const AnimatorCondition& condition, const AnimatorDefinition& definition) {
    const AnimatorParameter* parameter = findNamed(definition.parameters, condition.parameter);
    if (parameter == nullptr) {
        return false;
    }
    switch (condition.mode) {
    case AnimatorConditionMode::Greater:
        return parameter->value > condition.threshold;
    case AnimatorConditionMode::Less:
        return parameter->value < condition.threshold;
    case AnimatorConditionMode::True:
    case AnimatorConditionMode::Triggered:
        return parameter->value != 0.0f;
    case AnimatorConditionMode::False:
        return parameter->value == 0.0f;
    }
    return false;
}

// Whether the transition may leave the current state now.
bool ready(const AnimatorTransition& transition, const AnimatorComponent& animator,
           std::span<const AnimationClip> clips, const AnimatorState& current) {
    if (!transition.from.empty() ? transition.from != animator.currentState : transition.to == animator.currentState) {
        return false;
    }
    if (transition.exitTime) {
        const std::optional<uint32_t> clip = clipIndex(clips, current.clip);
        const float duration = clip ? clips[*clip].duration : 0.0f;
        if (duration > 0.0f && animator.stateTime / duration < *transition.exitTime) {
            return false;
        }
    }
    return std::ranges::all_of(transition.conditions, [&](const AnimatorCondition& condition) {
        return holds(condition, animator.definition);
    });
}

void enter(AnimatorComponent& animator, AnimationComponent& animation, const AnimatorState& state, uint32_t clip,
           std::optional<float> crossFadeSeconds) {
    if (crossFadeSeconds) {
        beginAnimationCrossFade(animation, clip, *crossFadeSeconds);
    } else {
        animation.clipIndex = clip;
        animation.time = 0.0f;
        animation.blendClipIndex.reset();
        animation.blendTime = 0.0f;
        animation.blendDuration = 0.0f;
    }
    animation.playbackSpeed = state.speed;
    animation.looping = state.looping;
    animation.playing = true;
    animator.currentState = state.name;
    animator.stateTime = 0.0f;
}

// The markers of state the playback passed going from `from` to `to` seconds,
// each loop counting; markers at exactly `from` count, at `to` do not.
void passedMarkers(const AnimatorDefinition& definition, const AnimatorState& state, float duration, float from,
                   float to, std::vector<std::string>& events) {
    constexpr int maximumLoopsPerUpdate = 16;
    for (const AnimatorEventMarker& marker : definition.events) {
        if (marker.state != state.name) {
            continue;
        }
        if (!state.looping || duration <= 0.0f) {
            if (marker.time >= from && marker.time < to) {
                events.push_back(marker.name);
            }
            continue;
        }
        const float first = std::max(0.0f, std::ceil((from - marker.time) / duration));
        for (int loop = 0; loop < maximumLoopsPerUpdate; ++loop) {
            const float time = marker.time + (first + static_cast<float>(loop)) * duration;
            if (time >= to) {
                break;
            }
            events.push_back(marker.name);
        }
    }
}
} // namespace

bool AnimatorComponent::setFloat(std::string_view name, float value) {
    AnimatorParameter* found = findNamed(definition.parameters, name);
    if (found == nullptr) {
        return false;
    }
    found->value = value;
    return true;
}

bool AnimatorComponent::setBool(std::string_view name, bool value) {
    return setFloat(name, value ? 1.0f : 0.0f);
}

bool AnimatorComponent::setTrigger(std::string_view name) {
    AnimatorParameter* found = findNamed(definition.parameters, name);
    if (found == nullptr || found->type != AnimatorParameterType::Trigger) {
        return false;
    }
    found->value = 1.0f;
    return true;
}

std::optional<float> AnimatorComponent::parameter(std::string_view name) const {
    const AnimatorParameter* found = findNamed(definition.parameters, name);
    return found != nullptr ? std::optional<float>(found->value) : std::nullopt;
}

std::string validateAnimatorDefinition(const AnimatorDefinition& definition) {
    std::unordered_set<std::string> names;
    for (const AnimatorParameter& parameter : definition.parameters) {
        if (parameter.name.empty() || !names.insert(parameter.name).second) {
            return "animator parameter names must be unique and not empty";
        }
    }
    names.clear();
    for (const AnimatorState& state : definition.states) {
        if (state.name.empty() || state.clip.empty()) {
            return "animator states need a name and a clip";
        }
        if (!names.insert(state.name).second) {
            return "animator state '" + state.name + "' is defined twice";
        }
    }
    if (!definition.entryState.empty() && !names.contains(definition.entryState)) {
        return "animator entry state '" + definition.entryState + "' does not exist";
    }
    for (const AnimatorTransition& transition : definition.transitions) {
        if ((!transition.from.empty() && !names.contains(transition.from)) || !names.contains(transition.to)) {
            return "animator transition to '" + transition.to + "' names a missing state";
        }
        if (transition.conditions.empty() && !transition.exitTime) {
            return "animator transition to '" + transition.to + "' needs a condition or an exit time";
        }
        if (!(transition.crossFadeSeconds >= 0.0f)) {
            return "animator transition to '" + transition.to + "' has a negative cross-fade";
        }
        for (const AnimatorCondition& condition : transition.conditions) {
            const AnimatorParameter* parameter = findNamed(definition.parameters, condition.parameter);
            if (parameter == nullptr) {
                return "animator condition names missing parameter '" + condition.parameter + "'";
            }
            const bool numeric =
                condition.mode == AnimatorConditionMode::Greater || condition.mode == AnimatorConditionMode::Less;
            const bool fits = numeric ? parameter->type == AnimatorParameterType::Float
                              : condition.mode == AnimatorConditionMode::Triggered
                                  ? parameter->type == AnimatorParameterType::Trigger
                                  : parameter->type == AnimatorParameterType::Bool;
            if (!fits) {
                return "animator condition on '" + condition.parameter + "' does not fit the parameter's type";
            }
        }
    }
    for (const AnimatorEventMarker& marker : definition.events) {
        if (!names.contains(marker.state) || marker.name.empty()) {
            return "animator events need a name and an existing state";
        }
    }
    return {};
}

bool updateAnimator(AnimatorComponent& animator, AnimationComponent& animation, std::span<const AnimationClip> clips,
                    float deltaSeconds, std::vector<std::string>& events) {
    AnimatorDefinition& definition = animator.definition;
    if (definition.states.empty()) {
        return true;
    }
    for (const AnimatorState& state : definition.states) {
        if (!clipIndex(clips, state.clip)) {
            return false;
        }
    }

    const AnimatorState* current = findNamed(definition.states, animator.currentState);
    if (current == nullptr) {
        const AnimatorState* entry = definition.entryState.empty()
                                         ? &definition.states.front()
                                         : findNamed(definition.states, definition.entryState);
        if (entry == nullptr) {
            return false;
        }
        enter(animator, animation, *entry, *clipIndex(clips, entry->clip), std::nullopt);
        current = entry;
    } else {
        for (const AnimatorTransition& transition : definition.transitions) {
            if (!ready(transition, animator, clips, *current)) {
                continue;
            }
            for (const AnimatorCondition& condition : transition.conditions) {
                if (condition.mode == AnimatorConditionMode::Triggered) {
                    animator.setFloat(condition.parameter, 0.0f);
                }
            }
            const AnimatorState* target = findNamed(definition.states, transition.to);
            enter(animator, animation, *target, *clipIndex(clips, target->clip), transition.crossFadeSeconds);
            current = target;
            break;
        }
    }

    const float from = animator.stateTime;
    animator.stateTime += std::max(deltaSeconds, 0.0f) * current->speed;
    if (animator.stateTime > from) {
        passedMarkers(definition, *current, clips[*clipIndex(clips, current->clip)].duration, from, animator.stateTime,
                      events);
    }
    return true;
}

bool enterAnimatorState(AnimatorComponent& animator, AnimationComponent& animation,
                        std::span<const AnimationClip> clips, std::string_view state) {
    const AnimatorState* found = findNamed(animator.definition.states, state);
    const std::optional<uint32_t> clip = found != nullptr ? clipIndex(clips, found->clip) : std::nullopt;
    if (!clip) {
        return false;
    }
    enter(animator, animation, *found, *clip, std::nullopt);
    return true;
}

void updateAnimators(Scene& scene, const AssetManager& assets, EventQueue* events, float deltaSeconds) {
    entt::registry& registry = scene.registry();
    std::vector<std::string> passed;
    for (const Entity entity : registry.view<AnimatorComponent, AnimationComponent, ModelComponent>()) {
        const ModelAsset* model = assets.model(registry.get<ModelComponent>(entity).model);
        if (model == nullptr) {
            continue;
        }
        auto& animator = registry.get<AnimatorComponent>(entity);
        passed.clear();
        if (!updateAnimator(animator, registry.get<AnimationComponent>(entity), model->animations, deltaSeconds,
                            passed) ||
            events == nullptr) {
            continue;
        }
        for (std::string& name : passed) {
            events->publish(AnimationEvent{.entity = scene.id(entity), .state = animator.currentState, .name = name});
        }
    }
}
