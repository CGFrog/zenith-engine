#pragma once

// animation.preview_state: plays an animator state on its object, for the Animator inspector's Preview.

#include "editor/operators/OperatorRegistry.hpp"
#include "engine/scene/SceneEntityId.hpp"

#include <string>

// Properties of animation.preview_state.
struct AnimatorPreviewProperties {
    // The object whose animator and animation play the state.
    SceneEntityId entity;
    // The animator state's name.
    std::string state;
};

// Animation operators, which need only the document and so run headless:
//   animation.preview_state  puts an object's animator in a state at once
//                            (enterAnimatorState): its Animation component
//                            plays the state's clip from the start, at the
//                            state's speed and looping, as one undo step.
//                            In Edit mode the clip plays as the Animation
//                            component's Preview does; in Play the running
//                            animator continues from the state.
OperatorRegistrationResult registerAnimationOperators(OperatorRegistry& registry);

struct EditorOperatorServices;
// This family's entry in editorOperatorFamilies() (EditorOperatorFamilies.hpp).
OperatorRegistrationResult registerAnimationOperatorFamily(OperatorRegistry& registry,
                                                           const EditorOperatorServices& services);
