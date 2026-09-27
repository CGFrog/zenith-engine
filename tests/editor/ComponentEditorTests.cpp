#include "editor/interaction/ComponentEditors.hpp"

#include <glm/gtc/epsilon.hpp>
#include <iostream>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}
} // namespace

int main() {
    EditableTransform source;
    source.translation = {3.0f, -2.0f, 8.0f};
    source.rotationDegrees = {20.0f, -35.0f, 10.0f};
    source.scale = {2.0f, 0.5f, 4.0f};
    const glm::mat4 matrix = ComponentEditors::composeTransform(source);
    EditableTransform restored;
    expect(ComponentEditors::decomposeTransform(matrix, restored), "composed transform should decompose");
    expect(glm::all(glm::epsilonEqual(source.translation, restored.translation, 1.0e-4f)),
           "transform adapter should preserve translation");
    expect(glm::all(glm::epsilonEqual(source.scale, restored.scale, 1.0e-4f)),
           "transform adapter should preserve scale");
    expect(glm::all(glm::epsilonEqual(glm::vec3(matrix[3]), source.translation, 1.0e-4f)),
           "transform adapter should use the scene translation convention");

    Scene scene;
    const Entity entity = scene.createEntity("Components");
    scene.registry().emplace<CameraComponent>(entity);
    expect(scene.registry().all_of<CameraComponent>(entity), "optional component should be addable");
    scene.registry().remove<CameraComponent>(entity);
    expect(!scene.registry().any_of<CameraComponent>(entity), "optional component should be removable");
    scene.registry().emplace<ColliderComponent>(entity);
    scene.registry().emplace<RigidBodyComponent>(entity);
    scene.registry().remove<ColliderComponent, RigidBodyComponent>(entity);
    expect(!scene.registry().any_of<ColliderComponent, RigidBodyComponent>(entity),
           "multiple optional component lifecycles should remain independent");
    return failures == 0 ? 0 : 1;
}
