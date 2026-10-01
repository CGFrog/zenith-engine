#include "engine/scene/Scene.hpp"

#include <glm/gtc/epsilon.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

bool nearlyEqual(const glm::mat4& left, const glm::mat4& right) {
    for (glm::length_t column = 0; column < 4; ++column) {
        if (!glm::all(glm::epsilonEqual(left[column], right[column], 1.0e-5f))) {
            return false;
        }
    }
    return true;
}
} // namespace

int main() {
    Scene scene;
    const Entity firstParent = scene.createEntity("First Parent");
    const Entity secondParent = scene.createEntity("Second Parent");
    const Entity child = scene.createEntity("Child");
    scene.registry().get<TransformComponent>(firstParent).local = glm::translate(glm::mat4(1.0f), {5.0f, 0.0f, 0.0f});
    scene.registry().get<TransformComponent>(secondParent).local = glm::translate(glm::mat4(1.0f), {-4.0f, 2.0f, 0.0f});
    scene.registry().get<TransformComponent>(child).local = glm::translate(glm::mat4(1.0f), {1.0f, 2.0f, 3.0f});
    expect(scene.reparent(child, firstParent).success, "valid reparent should succeed");
    scene.updateWorldTransforms();
    scene.updateWorldTransforms();
    expect(scene.lastTransformUpdateStats().recomputed == 0,
           "an unchanged hierarchy should reuse cached world transforms");
    scene.registry().get<TransformComponent>(child).local[3].x += 1.0f;
    scene.markTransformDirty(child);
    scene.updateWorldTransforms();
    expect(scene.lastTransformUpdateStats().recomputed == 1, "a local transform change should update only that branch");
    const glm::mat4 originalWorld = scene.registry().get<TransformComponent>(child).world;
    expect(scene.reparent(child, secondParent, ReparentMode::PreserveWorldTransform).success,
           "world-preserving reparent should succeed");
    expect(nearlyEqual(scene.registry().get<TransformComponent>(child).world, originalWorld),
           "world-preserving reparent should retain the world transform");

    expect(!scene.reparent(secondParent, child).success, "reparent should reject hierarchy cycles");
    expect(scene.registry().get<RelationshipComponent>(secondParent).parent == entt::null,
           "failed cycle should not mutate hierarchy");

    const Entity singularParent = scene.createEntity("Singular");
    scene.registry().get<TransformComponent>(singularParent).local = glm::scale(glm::mat4(1.0f), {0.0f, 1.0f, 1.0f});
    expect(!scene.reparent(child, singularParent, ReparentMode::PreserveWorldTransform).success,
           "world-preserving reparent should reject singular parent transforms");
    expect(scene.registry().get<RelationshipComponent>(child).parent == secondParent,
           "failed world-preserving reparent should retain the old parent");

    scene.registry().emplace<CameraComponent>(child);
    const Entity grandchild = scene.createEntity("Grandchild");
    scene.reparent(grandchild, child);
    scene.registry().emplace<ColliderComponent>(grandchild, BoxCollider{{2.0f, 3.0f, 4.0f}});
    Entity duplicate = entt::null;
    expect(scene.duplicateSubtree(child, firstParent, duplicate).success, "subtree duplication should succeed");
    expect(scene.id(duplicate) != scene.id(child), "duplicate should receive a new stable ID");
    expect(scene.registry().all_of<CameraComponent>(duplicate), "duplicate should copy optional components");
    const auto& duplicateChildren = scene.registry().get<RelationshipComponent>(duplicate).children;
    expect(duplicateChildren.size() == 1, "duplicate should copy descendants");
    if (!duplicateChildren.empty()) {
        expect(scene.registry().all_of<ColliderComponent>(duplicateChildren.front()),
               "duplicate descendants should copy their optional components");
        expect(scene.id(duplicateChildren.front()) != scene.id(grandchild),
               "duplicate descendants should receive new stable IDs");
    }

    scene.destroyEntity(duplicate);
    expect(scene.findEntity(scene.id(child)) == child, "deleting a duplicate should not affect its source");
    return failures == 0 ? 0 : 1;
}
