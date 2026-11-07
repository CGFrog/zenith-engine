#include "AnimatorEditor.hpp"

#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/EditorWidgets.hpp"
#include "engine/assets/Assets.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <utility>

namespace AnimatorEditor {
namespace {
using ParameterTypeLabel = std::pair<AnimatorParameterType, const char*>;
constexpr std::array<ParameterTypeLabel, 3> parameterTypeLabels{{{AnimatorParameterType::Float, "Float"},
                                                                 {AnimatorParameterType::Bool, "Bool"},
                                                                 {AnimatorParameterType::Trigger, "Trigger"}}};
using ConditionModeLabel = std::pair<AnimatorConditionMode, const char*>;
constexpr std::array<ConditionModeLabel, 5> conditionModeLabels{{{AnimatorConditionMode::Greater, "Greater than"},
                                                                 {AnimatorConditionMode::Less, "Less than"},
                                                                 {AnimatorConditionMode::True, "Is true"},
                                                                 {AnimatorConditionMode::False, "Is false"},
                                                                 {AnimatorConditionMode::Triggered, "Triggered"}}};

template <typename Value, size_t Count>
const char* labelOf(const std::array<std::pair<Value, const char*>, Count>& labels, Value value) {
    for (const auto& [candidate, label] : labels) {
        if (candidate == value) {
            return label;
        }
    }
    return labels.front().second;
}

template <typename Item> std::vector<std::string> namesOf(const std::vector<Item>& items) {
    std::vector<std::string> names;
    names.reserve(items.size());
    for (const Item& item : items) {
        names.push_back(item.name);
    }
    return names;
}

std::string unusedName(const std::vector<std::string>& names, std::string_view base) {
    std::string candidate(base);
    for (int suffix = 2; std::ranges::find(names, candidate) != names.end(); ++suffix) {
        candidate = std::string(base) + " " + std::to_string(suffix);
    }
    return candidate;
}

const AnimatorParameter* findParameter(const AnimatorDefinition& definition, std::string_view name) {
    const auto found = std::ranges::find(definition.parameters, name, &AnimatorParameter::name);
    return found != definition.parameters.end() ? &*found : nullptr;
}

const ImVec4 problemColor = toImVec4(palette::error);

// A text field whose text reaches the caller only when typing ends (Enter,
// or leaving the field), returned when it differs from value. While the field
// is active its text lives in state, so a rebuilt copy of the definition each
// frame does not reset what is being typed. Escape restores value.
std::optional<std::string> committedText(const char* label, const std::string& value, AnimatorEditorState& state) {
    const ImGuiID id = ImGui::GetID(label);
    std::string text = state.typingId == id ? state.typingText : value;
    ImGui::InputText(label, &text);
    if (ImGui::IsItemActive()) {
        state.typingId = id;
        state.typingText = text;
    }
    if (!ImGui::IsItemDeactivated()) {
        return std::nullopt;
    }
    state.typingId = 0;
    state.typingText.clear();
    if (ImGui::IsItemDeactivatedAfterEdit() && text != value) {
        return text;
    }
    return std::nullopt;
}

// A combo choosing value from names. emptyLabel, when not null, is a first
// entry meaning "none" (an empty value). A value no name matches shows as
// missing. Returns whether value changed.
bool nameCombo(const char* label, std::string& value, const std::vector<std::string>& names,
               const char* emptyLabel = nullptr) {
    const bool missing = !value.empty() && std::ranges::find(names, value) == names.end();
    const std::string preview = value.empty() && emptyLabel != nullptr ? std::string(emptyLabel)
                                : missing                              ? value + " (missing)"
                                                                       : value;
    bool changed = false;
    if (ImGui::BeginCombo(label, preview.c_str())) {
        if (emptyLabel != nullptr && ImGui::Selectable(emptyLabel, value.empty())) {
            changed = !value.empty();
            value.clear();
        }
        for (size_t index = 0; index < names.size(); ++index) {
            ImGui::PushID(static_cast<int>(index));
            const bool selected = names[index] == value;
            if (ImGui::Selectable(names[index].c_str(), selected)) {
                changed = !selected;
                value = names[index];
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (missing) {
        ImGui::SetItemTooltip("'%s' does not exist", value.c_str());
    }
    return changed;
}

// The names the combos offer, taken once per frame from the unedited
// definition and the model.
struct AnimatorNames {
    std::vector<std::string> states;
    std::vector<std::string> parameters;
    std::vector<std::string> clips;
};

// Draws an element's tree node with a Remove button beside it; sets remove
// when it is pressed. Returns whether the node is open (pop it then).
bool elementNode(const char* text, bool problem, bool& remove) {
    if (problem) {
        ImGui::PushStyleColor(ImGuiCol_Text, problemColor);
    }
    const bool open = ImGui::TreeNode("##element", "%s", text);
    if (problem) {
        ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    if (widgets::Button("Remove")) {
        remove = true;
    }
    return open;
}

bool drawParameters(AnimatorDefinition& definition, AnimatorEditorState& state) {
    bool changed = false;
    std::optional<size_t> removed;
    for (size_t index = 0; index < definition.parameters.size(); ++index) {
        ImGui::PushID(static_cast<int>(index));
        bool remove = false;
        AnimatorParameter& parameter = definition.parameters[index];
        const std::string text = parameter.name + " (" + labelOf(parameterTypeLabels, parameter.type) + ")";
        if (elementNode(text.c_str(), false, remove)) {
            if (std::optional<std::string> name = committedText("Name", parameter.name, state)) {
                renameParameter(definition, index, *name);
                changed = true;
            }
            if (ImGui::BeginCombo("Type", labelOf(parameterTypeLabels, parameter.type))) {
                for (const auto& [type, label] : parameterTypeLabels) {
                    if (ImGui::Selectable(label, type == parameter.type) && type != parameter.type) {
                        setParameterType(definition, index, type);
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            // The value gameplay changes; this is where an animator starts.
            if (parameter.type == AnimatorParameterType::Float) {
                changed |= ImGui::DragFloat("Value", &parameter.value, 0.01f);
            } else {
                bool set = parameter.value != 0.0f;
                if (ImGui::Checkbox(parameter.type == AnimatorParameterType::Bool ? "Value" : "Set", &set)) {
                    parameter.value = set ? 1.0f : 0.0f;
                    changed = true;
                }
            }
            ImGui::TreePop();
        }
        if (remove) {
            removed = index;
        }
        ImGui::PopID();
    }
    if (removed) {
        removeParameter(definition, *removed);
        changed = true;
    }
    if (widgets::Button("Add Parameter")) {
        definition.parameters.push_back({.name = unusedParameterName(definition, "parameter")});
        changed = true;
    }
    return changed;
}

bool drawStates(AnimatorDefinition& definition, const AnimatorNames& names, AnimatorEditorState& state,
                std::optional<std::string>& preview) {
    bool changed = false;
    std::optional<size_t> removed;
    for (size_t index = 0; index < definition.states.size(); ++index) {
        ImGui::PushID(static_cast<int>(index));
        bool remove = false;
        AnimatorState& animatorState = definition.states[index];
        const bool clipMissing =
            !names.clips.empty() && std::ranges::find(names.clips, animatorState.clip) == names.clips.end();
        const std::string text = animatorState.name + (definition.entryState == animatorState.name ? "  (entry)" : "") +
                                 (clipMissing ? "  - clip '" + animatorState.clip + "' missing" : "");
        const bool open = elementNode(text.c_str(), clipMissing, remove);
        ImGui::SameLine();
        ImGui::BeginDisabled(names.clips.empty() || clipMissing);
        if (widgets::Button("Preview")) {
            preview = animatorState.name;
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Play this state's clip on the object from its start");
        if (open) {
            if (std::optional<std::string> name = committedText("Name", animatorState.name, state)) {
                renameState(definition, index, *name);
                changed = true;
            }
            if (names.clips.empty()) {
                if (std::optional<std::string> clip = committedText("Clip", animatorState.clip, state)) {
                    animatorState.clip = std::move(*clip);
                    changed = true;
                }
            } else {
                changed |= nameCombo("Clip", animatorState.clip, names.clips);
            }
            changed |= ImGui::DragFloat("Speed", &animatorState.speed, 0.01f, -8.0f, 8.0f);
            changed |= ImGui::Checkbox("Loop", &animatorState.looping);
            ImGui::TreePop();
        }
        if (remove) {
            removed = index;
        }
        ImGui::PopID();
    }
    if (removed) {
        removeState(definition, *removed);
        changed = true;
    }
    ImGui::BeginDisabled(names.clips.empty());
    if (widgets::Button("Add State")) {
        definition.states.push_back({.name = unusedStateName(definition, "State"), .clip = names.clips.front()});
        changed = true;
    }
    ImGui::EndDisabled();
    if (names.clips.empty()) {
        ImGui::SetItemTooltip("The object's model has no animation clips");
    }
    return changed;
}

bool drawConditions(AnimatorTransition& transition, const AnimatorDefinition& definition, const AnimatorNames& names) {
    bool changed = false;
    std::optional<size_t> removed;
    for (size_t index = 0; index < transition.conditions.size(); ++index) {
        ImGui::PushID(static_cast<int>(index));
        AnimatorCondition& condition = transition.conditions[index];
        ImGui::SeparatorText(("Condition " + std::to_string(index + 1)).c_str());
        if (nameCombo("Parameter", condition.parameter, names.parameters)) {
            if (const AnimatorParameter* parameter = findParameter(definition, condition.parameter);
                parameter != nullptr && !conditionModeFits(condition.mode, parameter->type)) {
                condition.mode = defaultConditionMode(parameter->type);
            }
            changed = true;
        }
        const AnimatorParameter* parameter = findParameter(definition, condition.parameter);
        if (ImGui::BeginCombo("Test", labelOf(conditionModeLabels, condition.mode))) {
            for (const auto& [mode, label] : conditionModeLabels) {
                if (parameter != nullptr && !conditionModeFits(mode, parameter->type)) {
                    continue;
                }
                if (ImGui::Selectable(label, mode == condition.mode) && mode != condition.mode) {
                    condition.mode = mode;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        if (condition.mode == AnimatorConditionMode::Greater || condition.mode == AnimatorConditionMode::Less) {
            changed |= ImGui::DragFloat("Threshold", &condition.threshold, 0.01f);
        }
        if (widgets::Button("Remove Condition")) {
            removed = index;
        }
        ImGui::PopID();
    }
    if (removed) {
        transition.conditions.erase(transition.conditions.begin() + static_cast<std::ptrdiff_t>(*removed));
        changed = true;
    }
    ImGui::BeginDisabled(definition.parameters.empty());
    if (widgets::Button("Add Condition")) {
        const AnimatorParameter& first = definition.parameters.front();
        transition.conditions.push_back({.parameter = first.name, .mode = defaultConditionMode(first.type)});
        changed = true;
    }
    ImGui::EndDisabled();
    if (definition.parameters.empty()) {
        ImGui::SetItemTooltip("Add a parameter for conditions to test");
    }
    return changed;
}

bool drawTransitions(AnimatorDefinition& definition, const AnimatorNames& names) {
    bool changed = false;
    std::optional<size_t> removed;
    for (size_t index = 0; index < definition.transitions.size(); ++index) {
        ImGui::PushID(static_cast<int>(index));
        bool remove = false;
        AnimatorTransition& transition = definition.transitions[index];
        const std::string text =
            (transition.from.empty() ? std::string("Any state") : transition.from) + " -> " + transition.to;
        if (elementNode(text.c_str(), false, remove)) {
            changed |= nameCombo("From", transition.from, names.states, "Any state");
            changed |= nameCombo("To", transition.to, names.states);
            changed |= ImGui::DragFloat("Cross-fade", &transition.crossFadeSeconds, 0.01f, 0.0f, 10.0f, "%.2f s");
            bool hasExitTime = transition.exitTime.has_value();
            if (ImGui::Checkbox("Exit time", &hasExitTime)) {
                transition.exitTime = hasExitTime ? std::optional<float>(1.0f) : std::nullopt;
                changed = true;
            }
            ImGui::SetItemTooltip("Wait until the source state has played this many clip lengths");
            if (transition.exitTime) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
                changed |= ImGui::DragFloat("##ExitTime", &*transition.exitTime, 0.01f, 0.0f, 100.0f, "%.2f");
            }
            changed |= drawConditions(transition, definition, names);
            ImGui::TreePop();
        }
        if (remove) {
            removed = index;
        }
        ImGui::PopID();
    }
    if (removed) {
        definition.transitions.erase(definition.transitions.begin() + static_cast<std::ptrdiff_t>(*removed));
        changed = true;
    }
    ImGui::BeginDisabled(definition.states.empty());
    if (widgets::Button("Add Transition")) {
        // Waiting for the end of the first state's clip is valid on its own.
        definition.transitions.push_back(
            {.from = definition.states.front().name, .to = definition.states.back().name, .exitTime = 1.0f});
        changed = true;
    }
    ImGui::EndDisabled();
    return changed;
}

bool drawEvents(AnimatorDefinition& definition, const AnimatorNames& names, AnimatorEditorState& state) {
    bool changed = false;
    std::optional<size_t> removed;
    for (size_t index = 0; index < definition.events.size(); ++index) {
        ImGui::PushID(static_cast<int>(index));
        bool remove = false;
        AnimatorEventMarker& marker = definition.events[index];
        char text[256];
        std::snprintf(text, sizeof(text), "%s at %.2f s in %s", marker.name.c_str(), marker.time, marker.state.c_str());
        if (elementNode(text, false, remove)) {
            if (std::optional<std::string> name = committedText("Name", marker.name, state)) {
                marker.name = std::move(*name);
                changed = true;
            }
            changed |= nameCombo("State", marker.state, names.states);
            changed |= ImGui::DragFloat("Time", &marker.time, 0.005f, 0.0f, 1000.0f, "%.3f s");
            ImGui::TreePop();
        }
        if (remove) {
            removed = index;
        }
        ImGui::PopID();
    }
    if (removed) {
        definition.events.erase(definition.events.begin() + static_cast<std::ptrdiff_t>(*removed));
        changed = true;
    }
    ImGui::BeginDisabled(definition.states.empty());
    if (widgets::Button("Add Event")) {
        definition.events.push_back({.state = definition.states.front().name, .name = "event"});
        changed = true;
    }
    ImGui::EndDisabled();
    return changed;
}

// A section's tree node, open at first.
bool sectionNode(const char* id, const char* title, size_t count) {
    return ImGui::TreeNodeEx(id, ImGuiTreeNodeFlags_DefaultOpen, "%s (%zu)", title, count);
}
} // namespace

std::string unusedStateName(const AnimatorDefinition& definition, std::string_view base) {
    return unusedName(namesOf(definition.states), base);
}

std::string unusedParameterName(const AnimatorDefinition& definition, std::string_view base) {
    return unusedName(namesOf(definition.parameters), base);
}

void renameState(AnimatorDefinition& definition, size_t index, const std::string& name) {
    const std::string previous = std::exchange(definition.states.at(index).name, name);
    for (AnimatorTransition& transition : definition.transitions) {
        if (transition.from == previous) {
            transition.from = name;
        }
        if (transition.to == previous) {
            transition.to = name;
        }
    }
    for (AnimatorEventMarker& marker : definition.events) {
        if (marker.state == previous) {
            marker.state = name;
        }
    }
    if (definition.entryState == previous) {
        definition.entryState = name;
    }
}

void renameParameter(AnimatorDefinition& definition, size_t index, const std::string& name) {
    const std::string previous = std::exchange(definition.parameters.at(index).name, name);
    for (AnimatorTransition& transition : definition.transitions) {
        for (AnimatorCondition& condition : transition.conditions) {
            if (condition.parameter == previous) {
                condition.parameter = name;
            }
        }
    }
}

void removeState(AnimatorDefinition& definition, size_t index) {
    const std::string name = definition.states.at(index).name;
    definition.states.erase(definition.states.begin() + static_cast<std::ptrdiff_t>(index));
    std::erase_if(definition.transitions, [&name](const AnimatorTransition& transition) {
        return transition.from == name || transition.to == name;
    });
    std::erase_if(definition.events, [&name](const AnimatorEventMarker& marker) { return marker.state == name; });
    if (definition.entryState == name) {
        definition.entryState.clear();
    }
}

void removeParameter(AnimatorDefinition& definition, size_t index) {
    const std::string name = definition.parameters.at(index).name;
    definition.parameters.erase(definition.parameters.begin() + static_cast<std::ptrdiff_t>(index));
    for (AnimatorTransition& transition : definition.transitions) {
        std::erase_if(transition.conditions,
                      [&name](const AnimatorCondition& condition) { return condition.parameter == name; });
    }
    std::erase_if(definition.transitions, [](const AnimatorTransition& transition) {
        return transition.conditions.empty() && !transition.exitTime;
    });
}

void setParameterType(AnimatorDefinition& definition, size_t index, AnimatorParameterType type) {
    AnimatorParameter& parameter = definition.parameters.at(index);
    parameter.type = type;
    parameter.value = 0.0f;
    for (AnimatorTransition& transition : definition.transitions) {
        for (AnimatorCondition& condition : transition.conditions) {
            if (condition.parameter == parameter.name && !conditionModeFits(condition.mode, type)) {
                condition.mode = defaultConditionMode(type);
            }
        }
    }
}

bool conditionModeFits(AnimatorConditionMode mode, AnimatorParameterType type) {
    switch (mode) {
    case AnimatorConditionMode::Greater:
    case AnimatorConditionMode::Less:
        return type == AnimatorParameterType::Float;
    case AnimatorConditionMode::True:
    case AnimatorConditionMode::False:
        return type == AnimatorParameterType::Bool;
    case AnimatorConditionMode::Triggered:
        return type == AnimatorParameterType::Trigger;
    }
    return false;
}

AnimatorConditionMode defaultConditionMode(AnimatorParameterType type) {
    switch (type) {
    case AnimatorParameterType::Float:
        return AnimatorConditionMode::Greater;
    case AnimatorParameterType::Bool:
        return AnimatorConditionMode::True;
    case AnimatorParameterType::Trigger:
        return AnimatorConditionMode::Triggered;
    }
    return AnimatorConditionMode::True;
}

std::vector<std::string> statesWithMissingClips(const AnimatorDefinition& definition,
                                                std::span<const AnimationClip> clips) {
    std::vector<std::string> missing;
    for (const AnimatorState& state : definition.states) {
        if (std::ranges::find(clips, state.clip, &AnimationClip::name) == clips.end()) {
            missing.push_back(state.name);
        }
    }
    return missing;
}

AnimatorEditResult drawAnimator(AnimatorComponent& component, std::span<const AnimationClip> clips,
                                AnimatorEditorState& state) {
    AnimatorEditResult result;
    if (!component.currentState.empty()) {
        ImGui::TextDisabled("Current state: %s (%.2f s)", component.currentState.c_str(), component.stateTime);
    }
    AnimatorNames names{.states = namesOf(component.definition.states),
                        .parameters = namesOf(component.definition.parameters)};
    for (const AnimationClip& clip : clips) {
        names.clips.push_back(clip.name);
    }
    if (!clips.empty()) {
        if (const std::vector<std::string> missing = statesWithMissingClips(component.definition, clips);
            !missing.empty()) {
            ImGui::TextColored(problemColor,
                               "%zu state(s) name a clip the model does not have; the animator "
                               "will not run until they are fixed.",
                               missing.size());
        }
    }

    // Edits go to a copy, kept only when the result is a valid animator.
    AnimatorDefinition edited = component.definition;
    bool changed = nameCombo("Entry state", edited.entryState, names.states, "First state");
    ImGui::SetItemTooltip("The state the animator starts in");
    if (sectionNode("##parameters", "Parameters", edited.parameters.size())) {
        changed |= drawParameters(edited, state);
        ImGui::TreePop();
    }
    if (sectionNode("##states", "States", edited.states.size())) {
        changed |= drawStates(edited, names, state, result.previewState);
        ImGui::TreePop();
    }
    if (sectionNode("##transitions", "Transitions", edited.transitions.size())) {
        changed |= drawTransitions(edited, names);
        ImGui::TreePop();
    }
    if (sectionNode("##events", "Events", edited.events.size())) {
        changed |= drawEvents(edited, names, state);
        ImGui::TreePop();
    }

    if (changed) {
        if (std::string problem = validateAnimatorDefinition(edited); problem.empty()) {
            component.definition = std::move(edited);
            state.refusal.clear();
            result.changed = true;
        } else {
            state.refusal = "Edit not applied: " + problem;
        }
    }
    if (!state.refusal.empty()) {
        ImGui::TextColored(problemColor, "%s", state.refusal.c_str());
    }
    return result;
}
} // namespace AnimatorEditor
