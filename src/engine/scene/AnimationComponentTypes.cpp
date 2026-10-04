// Field keys are the scene document format, so renaming one breaks saved
// scenes unless a schema migration is added. Presentation hints mirror the
// ranges the inspector has always offered.

#include "AnimationComponentTypes.hpp"

#include "ComponentTypeHelpers.hpp"
#include "ComponentTypes.hpp"
#include "SceneAssetReferences.hpp"
#include "engine/animation/Animator.hpp"

#include <array>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace {
using Json = nlohmann::json;
using component_type_helpers::loadModelReference;
using component_type_helpers::modelReference;

constexpr size_t maximumMorphTargets = 64;
} // namespace

ComponentType animationComponentType() {
    return ComponentTypeBuilder<AnimationComponent>("animation", "Animation")
        .category("Animation")
        .codec(
            [](const AnimationComponent& animation, const ComponentSaveContext&, Json& output) {
                Json blendClip = animation.blendClipIndex ? Json(*animation.blendClipIndex) : Json(nullptr);
                output = {{"clip", animation.clipIndex},       {"time", animation.time},
                          {"speed", animation.playbackSpeed},  {"looping", animation.looping},
                          {"playing", animation.playing},      {"blend_clip", std::move(blendClip)},
                          {"blend_time", animation.blendTime}, {"blend_duration", animation.blendDuration}};
                return SceneDocumentResult{.success = true};
            },
            [](const Json& input, AnimationComponent& animation, const ComponentLoadContext&) {
                animation.clipIndex = input.at("clip").get<uint32_t>();
                animation.time = input.at("time").get<float>();
                animation.playbackSpeed = input.at("speed").get<float>();
                animation.looping = input.at("looping").get<bool>();
                animation.playing = input.at("playing").get<bool>();
                if (const auto blend = input.find("blend_clip"); blend != input.end() && !blend->is_null()) {
                    animation.blendClipIndex = blend->get<uint32_t>();
                }
                animation.blendTime = input.value("blend_time", 0.0f);
                animation.blendDuration = input.value("blend_duration", 0.0f);
                return SceneDocumentResult{.success = true};
            })
        .build();
}

ComponentType morphWeightsComponentType() {
    return ComponentTypeBuilder<MorphWeightsComponent>("morph_weights", "Morph Weights")
        .category("Animation")
        .codec(
            [](const MorphWeightsComponent& morph, const ComponentSaveContext&, Json& output) {
                output = {{"weights", morph.weights}};
                return SceneDocumentResult{.success = true};
            },
            [](const Json& input, MorphWeightsComponent& morph, const ComponentLoadContext&) {
                morph.weights = input.at("weights").get<std::vector<float>>();
                return SceneDocumentResult{.success = true};
            })
        .validate([](const MorphWeightsComponent& morph) {
            return morph.weights.size() > maximumMorphTargets
                       ? std::string("Morph-weight component exceeds the 64-target safety limit")
                       : std::string{};
        })
        .build();
}

ComponentType skinComponentType() {
    return ComponentTypeBuilder<SkinComponent>("skin", "Skin")
        .category("Animation")
        .internal()
        .codec(
            [](const SkinComponent& skin, const ComponentSaveContext& context, Json& output) {
                std::string relative;
                if (SceneDocumentResult result =
                        modelReference(context.assets, skin.model, context.options, "skin", relative);
                    !result) {
                    return result;
                }
                output = {{"instance_root", skin.instanceRoot.value}, {"skin", skin.skinIndex}};
                writeAssetReference(relative, context.options, output);
                return SceneDocumentResult{.success = true};
            },
            [](const Json& input, SkinComponent& skin, const ComponentLoadContext& context) {
                if (SceneDocumentResult result = loadModelReference(input, context, skin.model); !result) {
                    return result;
                }
                skin.instanceRoot = SceneEntityId{input.at("instance_root").get<uint64_t>()};
                skin.skinIndex = input.at("skin").get<uint32_t>();
                return SceneDocumentResult{.success = true};
            })
        .build();
}

namespace {
using AnimatorParameterTypeName = std::pair<AnimatorParameterType, const char*>;
constexpr std::array<AnimatorParameterTypeName, 3> parameterTypeNames{{{AnimatorParameterType::Float, "float"},
                                                                       {AnimatorParameterType::Bool, "bool"},
                                                                       {AnimatorParameterType::Trigger, "trigger"}}};
using AnimatorConditionModeName = std::pair<AnimatorConditionMode, const char*>;
constexpr std::array<AnimatorConditionModeName, 5> conditionModeNames{
    {{AnimatorConditionMode::Greater, "greater"},
     {AnimatorConditionMode::Less, "less"},
     {AnimatorConditionMode::True, "true"},
     {AnimatorConditionMode::False, "false"},
     {AnimatorConditionMode::Triggered, "triggered"}}};

template <typename Value, size_t Count>
const char* nameOf(const std::array<std::pair<Value, const char*>, Count>& names, Value value) {
    for (const auto& [candidate, name] : names) {
        if (candidate == value) {
            return name;
        }
    }
    return names.front().second;
}

template <typename Value, size_t Count>
bool valueOf(const std::array<std::pair<Value, const char*>, Count>& names, const std::string& name, Value& value) {
    for (const auto& [candidate, candidateName] : names) {
        if (name == candidateName) {
            value = candidate;
            return true;
        }
    }
    return false;
}

Json writeAnimator(const AnimatorDefinition& definition) {
    Json parameters = Json::array();
    for (const AnimatorParameter& parameter : definition.parameters) {
        parameters.push_back({{"name", parameter.name},
                              {"type", nameOf(parameterTypeNames, parameter.type)},
                              {"value", parameter.value}});
    }
    Json states = Json::array();
    for (const AnimatorState& state : definition.states) {
        states.push_back(
            {{"name", state.name}, {"clip", state.clip}, {"speed", state.speed}, {"looping", state.looping}});
    }
    Json transitions = Json::array();
    for (const AnimatorTransition& transition : definition.transitions) {
        Json conditions = Json::array();
        for (const AnimatorCondition& condition : transition.conditions) {
            conditions.push_back({{"parameter", condition.parameter},
                                  {"mode", nameOf(conditionModeNames, condition.mode)},
                                  {"threshold", condition.threshold}});
        }
        Json written = {{"from", transition.from},
                        {"to", transition.to},
                        {"conditions", std::move(conditions)},
                        {"cross_fade", transition.crossFadeSeconds}};
        if (transition.exitTime) {
            written["exit_time"] = *transition.exitTime;
        }
        transitions.push_back(std::move(written));
    }
    Json events = Json::array();
    for (const AnimatorEventMarker& marker : definition.events) {
        events.push_back({{"state", marker.state}, {"time", marker.time}, {"name", marker.name}});
    }
    return {{"parameters", std::move(parameters)},
            {"states", std::move(states)},
            {"transitions", std::move(transitions)},
            {"events", std::move(events)},
            {"entry_state", definition.entryState}};
}

SceneDocumentResult readAnimator(const Json& input, AnimatorDefinition& definition) {
    for (const Json& parameter : input.value("parameters", Json::array())) {
        AnimatorParameter read{.name = parameter.at("name").get<std::string>(),
                               .value = parameter.value("value", 0.0f)};
        if (!valueOf(parameterTypeNames, parameter.value("type", std::string("float")), read.type)) {
            return {.error = "Animator parameter '" + read.name + "' has an unknown type"};
        }
        definition.parameters.push_back(std::move(read));
    }
    for (const Json& state : input.value("states", Json::array())) {
        definition.states.push_back({.name = state.at("name").get<std::string>(),
                                     .clip = state.at("clip").get<std::string>(),
                                     .speed = state.value("speed", 1.0f),
                                     .looping = state.value("looping", true)});
    }
    for (const Json& transition : input.value("transitions", Json::array())) {
        AnimatorTransition read{.from = transition.value("from", std::string{}),
                                .to = transition.at("to").get<std::string>(),
                                .crossFadeSeconds = transition.value("cross_fade", 0.2f)};
        if (const auto exitTime = transition.find("exit_time"); exitTime != transition.end() && !exitTime->is_null()) {
            read.exitTime = exitTime->get<float>();
        }
        for (const Json& condition : transition.value("conditions", Json::array())) {
            AnimatorCondition readCondition{.parameter = condition.at("parameter").get<std::string>(),
                                            .threshold = condition.value("threshold", 0.0f)};
            if (!valueOf(conditionModeNames, condition.value("mode", std::string("true")), readCondition.mode)) {
                return {.error = "Animator condition on '" + readCondition.parameter + "' has an unknown mode"};
            }
            read.conditions.push_back(std::move(readCondition));
        }
        definition.transitions.push_back(std::move(read));
    }
    for (const Json& marker : input.value("events", Json::array())) {
        definition.events.push_back({.state = marker.at("state").get<std::string>(),
                                     .time = marker.value("time", 0.0f),
                                     .name = marker.at("name").get<std::string>()});
    }
    definition.entryState = input.value("entry_state", std::string{});
    return {.success = true};
}
} // namespace

ComponentType animatorComponentType() {
    return ComponentTypeBuilder<AnimatorComponent>("animator", "Animator")
        .category("Animation")
        .codec(
            [](const AnimatorComponent& animator, const ComponentSaveContext&, Json& output) {
                output = writeAnimator(animator.definition);
                return SceneDocumentResult{.success = true};
            },
            [](const Json& input, AnimatorComponent& animator, const ComponentLoadContext&) {
                return readAnimator(input, animator.definition);
            })
        .validate([](const AnimatorComponent& animator) { return validateAnimatorDefinition(animator.definition); })
        .build();
}
