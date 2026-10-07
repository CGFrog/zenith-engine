#include "engine/scene/Scene.hpp"

#include "support/Doctest.hpp"

#include <glm/gtc/epsilon.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace {

bool nearlyEqual(const glm::mat4& left, const glm::mat4& right) {
    for (glm::length_t column = 0; column < 4; ++column) {
        if (!glm::all(glm::epsilonEqual(left[column], right[column], 1.0e-5f))) {
            return false;
        }
    }
    return true;
}

// Two translated parents and an unparented, translated child.
struct HierarchyFixture {
    HierarchyFixture() {
        scene.registry().get<TransformComponent>(firstParent).local =
            glm::translate(glm::mat4(1.0f), {5.0f, 0.0f, 0.0f});
        scene.registry().get<TransformComponent>(secondParent).local =
            glm::translate(glm::mat4(1.0f), {-4.0f, 2.0f, 0.0f});
        scene.registry().get<TransformComponent>(child).local = glm::translate(glm::mat4(1.0f), {1.0f, 2.0f, 3.0f});
    }

    // Parents the child under the first parent, moves it, then reparents it
    // under the second parent keeping its world transform.
    void moveChildToSecondParent() {
        scene.reparent(child, firstParent);
        scene.updateWorldTransforms();
        scene.updateWorldTransforms();
        scene.registry().get<TransformComponent>(child).local[3].x += 1.0f;
        scene.markTransformDirty(child);
        scene.updateWorldTransforms();
        scene.reparent(child, secondParent, ReparentMode::PreserveWorldTransform);
    }

    Scene scene;
    const Entity firstParent = scene.createEntity("First Parent");
    const Entity secondParent = scene.createEntity("Second Parent");
    const Entity child = scene.createEntity("Child");
};

} // namespace

TEST_SUITE_BEGIN("SceneHierarchy");

TEST_CASE("SceneHierarchy: cached world transforms update only dirty branches and survive world-preserving reparent") {
    HierarchyFixture fixture;
    Scene& scene = fixture.scene;
    const Entity child = fixture.child;
    CHECK_MESSAGE(scene.reparent(child, fixture.firstParent).success, "valid reparent should succeed");
    scene.updateWorldTransforms();
    scene.updateWorldTransforms();
    CHECK_MESSAGE(scene.lastTransformUpdateStats().recomputed == 0,
                  "an unchanged hierarchy should reuse cached world transforms");
    scene.registry().get<TransformComponent>(child).local[3].x += 1.0f;
    scene.markTransformDirty(child);
    scene.updateWorldTransforms();
    CHECK_MESSAGE(scene.lastTransformUpdateStats().recomputed == 1,
                  "a local transform change should update only that branch");
    const glm::mat4 originalWorld = scene.registry().get<TransformComponent>(child).world;
    CHECK_MESSAGE(scene.reparent(child, fixture.secondParent, ReparentMode::PreserveWorldTransform).success,
                  "world-preserving reparent should succeed");
    CHECK_MESSAGE(nearlyEqual(scene.registry().get<TransformComponent>(child).world, originalWorld),
                  "world-preserving reparent should retain the world transform");
}

TEST_CASE("SceneHierarchy: reparent rejects hierarchy cycles without mutating the hierarchy") {
    HierarchyFixture fixture;
    fixture.moveChildToSecondParent();
    Scene& scene = fixture.scene;
    CHECK_MESSAGE(!scene.reparent(fixture.secondParent, fixture.child).success,
                  "reparent should reject hierarchy cycles");
    CHECK_MESSAGE((scene.registry().get<RelationshipComponent>(fixture.secondParent).parent == entt::null),
                  "failed cycle should not mutate hierarchy");
}

TEST_CASE("SceneHierarchy: world-preserving reparent rejects singular parent transforms") {
    HierarchyFixture fixture;
    fixture.moveChildToSecondParent();
    Scene& scene = fixture.scene;
    const Entity singularParent = scene.createEntity("Singular");
    scene.registry().get<TransformComponent>(singularParent).local = glm::scale(glm::mat4(1.0f), {0.0f, 1.0f, 1.0f});
    CHECK_MESSAGE(!scene.reparent(fixture.child, singularParent, ReparentMode::PreserveWorldTransform).success,
                  "world-preserving reparent should reject singular parent transforms");
    CHECK_MESSAGE(scene.registry().get<RelationshipComponent>(fixture.child).parent == fixture.secondParent,
                  "failed world-preserving reparent should retain the old parent");
}

TEST_CASE("SceneHierarchy: subtree duplication copies components and descendants with new ids") {
    HierarchyFixture fixture;
    fixture.moveChildToSecondParent();
    Scene& scene = fixture.scene;
    const Entity child = fixture.child;
    scene.registry().emplace<CameraComponent>(child);
    const Entity grandchild = scene.createEntity("Grandchild");
    scene.reparent(grandchild, child);
    scene.registry().emplace<ColliderComponent>(grandchild, BoxCollider{{2.0f, 3.0f, 4.0f}});
    Entity duplicate = entt::null;
    CHECK_MESSAGE(scene.duplicateSubtree(child, fixture.firstParent, duplicate).success,
                  "subtree duplication should succeed");
    CHECK_MESSAGE(scene.id(duplicate) != scene.id(child), "duplicate should receive a new stable ID");
    CHECK_MESSAGE(scene.registry().all_of<CameraComponent>(duplicate), "duplicate should copy optional components");
    const auto& duplicateChildren = scene.registry().get<RelationshipComponent>(duplicate).children;
    CHECK_MESSAGE(duplicateChildren.size() == 1, "duplicate should copy descendants");
    if (!duplicateChildren.empty()) {
        CHECK_MESSAGE(scene.registry().all_of<ColliderComponent>(duplicateChildren.front()),
                      "duplicate descendants should copy their optional components");
        CHECK_MESSAGE(scene.id(duplicateChildren.front()) != scene.id(grandchild),
                      "duplicate descendants should receive new stable IDs");
    }

    scene.destroyEntity(duplicate);
    CHECK_MESSAGE(scene.findEntity(scene.id(child)) == child, "deleting a duplicate should not affect its source");
}

TEST_SUITE_END();
