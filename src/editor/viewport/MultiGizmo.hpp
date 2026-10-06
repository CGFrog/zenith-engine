#pragma once

// The transform gizmo over several selected objects and its pivot.

#include "editor/interaction/ModalTransform.hpp"
#include "engine/scene/Scene.hpp"

#include <glm/glm.hpp>

#include <span>
#include <vector>

// The viewport gizmo's maths for any number of selected objects, kept apart
// from ImGuizmo so it can be tested without a window. The gizmo is a frame
// with unit axes at the pivot; each frame of a drag, the change between the
// frame at the start and the frame ImGuizmo returns is applied to the
// objects' start matrices through applyPivotedTransform, as the modal
// transforms do, so rotated objects never shear.

// What the gizmo turns about and how it is oriented.
struct GizmoPlacement {
    ModalTransformPivot pivot;
    // The gizmo's axes: the active object's (normalizedAxes) in Local
    // space, the identity in World space.
    glm::mat3 orientation{1.0f};
    // Translation places the gizmo at the median (or the active origin)
    // whatever the pivot, as ModalTransform::pivot does; rotation and scale
    // place it on the pivot point.
    bool translating{false};
};

// The gizmo's frame for objects at startWorld (world matrices).
glm::mat4 gizmoFrame(std::span<const glm::mat4> startWorld, const GizmoPlacement& placement);

// The objects' world matrices after the gizmo moved from gizmoStart to
// gizmoNow, pivoting as pivotMode says (each object's own origin for
// IndividualOrigins, otherwise gizmoStart's origin). startWorld when the
// change cannot be decomposed.
std::vector<glm::mat4> applyGizmoChange(std::span<const glm::mat4> startWorld, TransformPivot pivotMode,
                                        const glm::mat4& gizmoStart, const glm::mat4& gizmoNow);

// Gives each of entities the world matrix at the same index, through its
// local matrix relative to its parent, and updates world transforms. The
// entities must not be ancestors of one another (Selection::transformRoots).
void setWorldTransforms(Scene& scene, std::span<const Entity> entities, std::span<const glm::mat4> world);
