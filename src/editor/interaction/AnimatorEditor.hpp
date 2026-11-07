#pragma once

// The Animator component's Inspector: parameters, states, transitions and events, with renames that follow references.

#include "engine/animation/Animator.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct AnimationClip;

// Authoring edits of an AnimatorDefinition that keep its references whole,
// plus the inspector that uses them. Names are references in an animator
// (transitions, conditions and events name states and parameters), so a
// rename or removal here updates or drops everything naming the old one.
namespace AnimatorEditor {
// base when no state (or parameter) has that name, otherwise "base 2",
// "base 3", and so on.
std::string unusedStateName(const AnimatorDefinition& definition, std::string_view base);
std::string unusedParameterName(const AnimatorDefinition& definition, std::string_view base);

// Renames the state at index and every transition, event and entry state
// naming it.
void renameState(AnimatorDefinition& definition, size_t index, const std::string& name);
// Renames the parameter at index and every condition naming it.
void renameParameter(AnimatorDefinition& definition, size_t index, const std::string& name);
// Removes the state at index with the transitions to or from it and its
// events; an entry state naming it falls back to the first state.
void removeState(AnimatorDefinition& definition, size_t index);
// Removes the parameter at index and the conditions testing it. A transition
// left with neither a condition nor an exit time could never fire and is
// removed too.
void removeParameter(AnimatorDefinition& definition, size_t index);
// Changes the parameter's type, resets its value to zero, and gives each
// condition testing it that no longer fits the type's default mode.
void setParameterType(AnimatorDefinition& definition, size_t index, AnimatorParameterType type);

// Whether a condition with mode may test a parameter of type (see
// validateAnimatorDefinition), and the mode a new condition on it starts with.
bool conditionModeFits(AnimatorConditionMode mode, AnimatorParameterType type);
AnimatorConditionMode defaultConditionMode(AnimatorParameterType type);

// The names of the states whose clip clips lacks, in state order. Such an
// animator does nothing at run time (updateAnimator refuses it), so the
// inspector marks them; a renamed or removed clip is the usual cause.
std::vector<std::string> statesWithMissingClips(const AnimatorDefinition& definition,
                                                std::span<const AnimationClip> clips);

// What the inspector keeps between frames: the text of the name field being
// typed in (committed when typing ends, so a half-typed name never reaches
// the animator) and why the last edit was refused.
struct AnimatorEditorState {
    // Whose animator the state belongs to (a scene entity id); the caller
    // resets the state when it changes.
    uint64_t owner{0};
    uint32_t typingId{0};
    std::string typingText;
    std::string refusal;
};

struct AnimatorEditResult {
    // The definition changed (and is valid).
    bool changed{false};
    // The state whose Preview button was pressed.
    std::optional<std::string> previewState;
};

// Draws the animator's definition: the entry state, parameters (name, type,
// value), states (name, clip chosen from clips, speed, looping, Preview),
// transitions (from, to, cross-fade, optional exit time, conditions), and
// events. Edits are made on a copy and kept only when the result passes
// validateAnimatorDefinition, since documents (and so undo) refuse an invalid
// animator; a refused edit leaves the component unchanged and shows why.
// Without clips, state clips are edited as text. In Play it also shows the
// current state.
AnimatorEditResult drawAnimator(AnimatorComponent& component, std::span<const AnimationClip> clips,
                                AnimatorEditorState& state);
} // namespace AnimatorEditor
