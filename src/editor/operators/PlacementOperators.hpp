#pragma once

// Headless operators that place objects quickly: object.place_model, object.array, object.scatter.

#include "editor/operators/OperatorRegistry.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Headless operators that place objects quickly:
//
//   object.place_model {asset, parent, location, rotation, normal, align_to_normal}
//       instantiates a model under a new entity named after the file and
//       selects it. Viewport drops pass the surface point and normal under
//       the mouse (placementUnderMouse), so Adjust Last Operation can toggle
//       the alignment.
//   object.array {count, offset, rotation_deg, scale_step, relative_to_bounds}
//       adds `count` copies of the selection, each stepped from the last.
//   object.scatter {count, radius, seed, align_to_surface, random_yaw, scale_jitter}
//       adds `count` copies of the selection on the surfaces below random
//       points of a disc around it, the same ones for the same seed.
//
// Array and scatter build every copy into one scene fragment and paste it
// once (paste rebuilds the scene), keep each copy under its original's
// parent, select the copies, and are one undo step that Adjust Last
// Operation re-runs.

struct PlaceModelProperties {
    // Path of the model: project-relative, or absolute (as an Asset Browser
    // drag carries it).
    std::string asset;
    // Stable id of the entity to parent the model to, in decimal; empty
    // places it at the scene root.
    std::string parent;
    // World position of the model's origin, in meters; an invocation starts
    // it at the 3D cursor.
    glm::vec3 location{0.0f};
    // Euler angles in degrees (as the transform inspector shows them),
    // applied in the frame the alignment leaves.
    glm::vec3 rotation{0.0f};
    // World surface normal at location (unit length), for align_to_normal.
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    // Turns the model's +Y onto normal before rotation applies.
    bool alignToNormal{false};
};

struct ArrayProperties {
    // Copies added (the originals stay).
    int32_t count{3};
    // Step from one copy to the next, in meters along world axes, or with
    // relativeToBounds in multiples of the selection's world bounds size.
    glm::vec3 offset{1.0f, 0.0f, 0.0f};
    // Turn of each step about the copies' median, in Euler degrees; the
    // offset turns with it, so a turning array curves.
    glm::vec3 rotationDegrees{0.0f};
    // Each step scales by 1 + scaleStep about the copies' median.
    float scaleStep{0.0f};
    bool relativeToBounds{true};
};

struct ScatterProperties {
    // Copies added (the originals stay).
    int32_t count{8};
    // Radius of the disc around the selection's median the copies land in, in meters.
    float radius{5.0f};
    uint32_t seed{1};
    // Turns each copy's +Y onto the surface normal under it.
    bool alignToSurface{false};
    // Turns each copy by a random angle about its up axis.
    bool randomYaw{true};
    // Each copy scales by a random factor in [1 - scaleJitter, 1 + scaleJitter].
    float scaleJitter{0.0f};
};

// The rotation placing an object: with alignToNormal the shortest turn
// taking +Y onto normal, then the Euler rotation (degrees) in that frame.
glm::mat3 placementRotation(const glm::vec3& rotationDegrees, const glm::vec3& normal, bool alignToNormal);

// The shortest rotation taking unit vector from onto unit vector to.
glm::mat3 rotationBetween(const glm::vec3& from, const glm::vec3& to);

// The world matrices of each array copy's roots (copy by copy, each listing
// the roots in start's order): copy n is copy n - 1 turned and scaled about
// its median and moved by the offset turned n - 1 times. offset is already
// in meters.
std::vector<std::vector<glm::mat4>> arrayCopyTransforms(std::span<const glm::mat4> start, int32_t count,
                                                        const glm::vec3& offset, const glm::vec3& rotationDegrees,
                                                        float scaleStep);

OperatorRegistrationResult registerPlacementOperators(OperatorRegistry& registry);

struct EditorOperatorServices;
// This family's entry in editorOperatorFamilies() (EditorOperatorFamilies.hpp).
OperatorRegistrationResult registerPlacementOperatorFamily(OperatorRegistry& registry,
                                                           const EditorOperatorServices& services);
