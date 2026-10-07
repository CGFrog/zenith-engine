#include "editor/interaction/ComponentEditors.hpp"

#include "support/Doctest.hpp"

#include <glm/gtc/epsilon.hpp>

TEST_SUITE_BEGIN("ComponentEditor");

TEST_CASE("ComponentEditor: the transform adapter round-trips a composed transform") {
    EditableTransform source;
    source.translation = {3.0f, -2.0f, 8.0f};
    source.rotationDegrees = {20.0f, -35.0f, 10.0f};
    source.scale = {2.0f, 0.5f, 4.0f};
    const glm::mat4 matrix = ComponentEditors::composeTransform(source);
    EditableTransform restored;
    CHECK_MESSAGE(ComponentEditors::decomposeTransform(matrix, restored), "composed transform should decompose");
    CHECK_MESSAGE(glm::all(glm::epsilonEqual(source.translation, restored.translation, 1.0e-4f)),
                  "transform adapter should preserve translation");
    CHECK_MESSAGE(glm::all(glm::epsilonEqual(source.scale, restored.scale, 1.0e-4f)),
                  "transform adapter should preserve scale");
    CHECK_MESSAGE(glm::all(glm::epsilonEqual(glm::vec3(matrix[3]), source.translation, 1.0e-4f)),
                  "transform adapter should use the scene translation convention");
}

TEST_CASE("ComponentEditor: optional components are added and removed independently") {
    Scene scene;
    const Entity entity = scene.createEntity("Components");
    scene.registry().emplace<CameraComponent>(entity);
    CHECK_MESSAGE(scene.registry().all_of<CameraComponent>(entity), "optional component should be addable");
    scene.registry().remove<CameraComponent>(entity);
    CHECK_MESSAGE(!scene.registry().any_of<CameraComponent>(entity), "optional component should be removable");
    scene.registry().emplace<ColliderComponent>(entity);
    scene.registry().emplace<RigidBodyComponent>(entity);
    scene.registry().remove<ColliderComponent, RigidBodyComponent>(entity);
    CHECK_MESSAGE((!scene.registry().any_of<ColliderComponent, RigidBodyComponent>(entity)),
                  "multiple optional component lifecycles should remain independent");
}

TEST_SUITE_END();
