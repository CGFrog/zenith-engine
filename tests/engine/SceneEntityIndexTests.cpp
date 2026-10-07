// Scene's id-to-entity index and dirty-transform bookkeeping: lookups stay
// correct across every way a scene gains, loses, or exchanges entities, and
// transform updates stay correct and proportional to what changed.
#include "engine/assets/Assets.hpp"
#include "engine/scene/Scene.hpp"
#include "engine/scene/SceneDocument.hpp"

#include "support/Doctest.hpp"

#include <glm/gtc/epsilon.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <string>
#include <vector>

TEST_SUITE_BEGIN("SceneEntityIndex");

namespace {

bool nearlyEqual(const glm::mat4& left, const glm::mat4& right) {
    for (glm::length_t column = 0; column < 4; ++column) {
        if (!glm::all(glm::epsilonEqual(left[column], right[column], 1.0e-5f))) {
            return false;
        }
    }
    return true;
}

glm::mat4 translation(float x, float y, float z) {
    return glm::translate(glm::mat4(1.0f), {x, y, z});
}

TEST_CASE("SceneEntityIndex: lookup after create and destroy") {
    Scene scene;
    const Entity root = scene.createEntity("Root");
    const Entity child = scene.createEntity("Child");
    const Entity grandchild = scene.createEntity("Grandchild");
    scene.setParent(child, root);
    scene.setParent(grandchild, child);
    const SceneEntityId rootId = scene.id(root);
    const SceneEntityId childId = scene.id(child);
    const SceneEntityId grandchildId = scene.id(grandchild);
    CHECK_MESSAGE(scene.findEntity(rootId) == root, "a created entity is found by id");
    CHECK_MESSAGE(scene.findEntity(grandchildId) == grandchild, "a nested entity is found by id");
    CHECK_MESSAGE((scene.findEntity(SceneEntityId{999}) == entt::null), "an unused id finds nothing");

    Entity explicitEntity = entt::null;
    CHECK_MESSAGE(scene.createEntityWithId(SceneEntityId{500}, "Explicit", explicitEntity).success,
                  "an explicit id is accepted");
    CHECK_MESSAGE(scene.findEntity(SceneEntityId{500}) == explicitEntity, "an explicit id is found");
    Entity duplicate = entt::null;
    CHECK_MESSAGE(!scene.createEntityWithId(SceneEntityId{500}, "Duplicate", duplicate).success,
                  "a duplicate id is refused");

    scene.destroyEntity(child);
    CHECK_MESSAGE((scene.findEntity(childId) == entt::null), "a destroyed entity is no longer found");
    CHECK_MESSAGE((scene.findEntity(grandchildId) == entt::null),
                  "a destroyed subtree's descendants are no longer found");
    CHECK_MESSAGE(scene.findEntity(rootId) == root, "destroying a child keeps its parent findable");

    // EnTT recycles the destroyed slots; the new entities have new ids.
    const Entity recycled = scene.createEntity("Recycled");
    CHECK_MESSAGE(scene.findEntity(scene.id(recycled)) == recycled, "an entity in a recycled slot is found");
    CHECK_MESSAGE((scene.findEntity(childId) == entt::null), "a recycled slot does not revive an old id");
}

TEST_CASE("SceneEntityIndex: lookup after duplicate") {
    Scene scene;
    const Entity root = scene.createEntity("Root");
    const Entity child = scene.createEntity("Child");
    scene.setParent(child, root);
    Entity copy = entt::null;
    CHECK_MESSAGE((scene.duplicateSubtree(root, entt::null, copy).success), "a subtree duplicates");
    CHECK_MESSAGE(scene.findEntity(scene.id(copy)) == copy, "a duplicated root is found by its new id");
    const auto& copiedChildren = scene.registry().get<RelationshipComponent>(copy).children;
    CHECK_MESSAGE(
        (copiedChildren.size() == 1 && scene.findEntity(scene.id(copiedChildren.front())) == copiedChildren.front()),
        "a duplicated child is found by its new id");
    CHECK_MESSAGE(scene.findEntity(scene.id(root)) == root, "the source stays findable after duplicating");
}

TEST_CASE("SceneEntityIndex: lookup after clear swap and replace") {
    Scene first;
    const SceneEntityId firstId = first.id(first.createEntity("In first"));
    Scene second;
    second.createEntity("Padding");
    const SceneEntityId secondId = second.id(second.createEntity("In second"));

    first.swap(second);
    CHECK_MESSAGE((first.findEntity(secondId) != entt::null &&
                   first.registry().get<NameComponent>(first.findEntity(secondId)).name == "In second"),
                  "swap moves the index with the registry");
    CHECK_MESSAGE((second.findEntity(firstId) != entt::null &&
                   second.registry().get<NameComponent>(second.findEntity(firstId)).name == "In first"),
                  "swap gives the other scene the other index");

    Scene replacement;
    const SceneEntityId replacementId = replacement.id(replacement.createEntity("Replacement"));
    first.replaceWith(std::move(replacement));
    CHECK_MESSAGE((first.findEntity(replacementId) != entt::null &&
                   first.registry().get<NameComponent>(first.findEntity(replacementId)).name == "Replacement"),
                  "replaceWith takes the replacement's index");
    CHECK_MESSAGE((replacement.findEntity(replacementId) == entt::null), "a replaced-from scene keeps no stale index");

    first.clear();
    CHECK_MESSAGE((first.findEntity(replacementId) == entt::null), "clear empties the index");
    CHECK_MESSAGE((first.findEntity(first.id(first.createEntity("After clear"))) != entt::null),
                  "entities created after clear are indexed");
}

TEST_CASE("SceneEntityIndex: lookup after document load") {
    Scene source;
    const Entity parent = source.createEntity("Parent");
    const Entity child = source.createEntity("Child");
    source.setParent(child, parent);
    const SceneEntityId childId = source.id(child);
    AssetManager assets;
    std::string document;
    CHECK_MESSAGE(SceneDocument::serialize(source, assets, {}, document).success, "the scene serializes");

    Scene loaded;
    loaded.createEntity("Replaced by the load");
    CHECK_MESSAGE(SceneDocument::deserialize(document, assets, {}, loaded).success, "the scene loads");
    const Entity loadedChild = loaded.findEntity(childId);
    CHECK_MESSAGE((loadedChild != entt::null && loaded.registry().get<NameComponent>(loadedChild).name == "Child"),
                  "a loaded entity is found by its document id");
}

TEST_CASE("SceneEntityIndex: many dirty entities recompute once") {
    constexpr uint32_t count = 5000;
    Scene scene;
    std::vector<Entity> entities;
    entities.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        entities.push_back(scene.createEntity("Body"));
    }
    scene.updateWorldTransforms();
    for (int pass = 0; pass < 2; ++pass) {
        for (Entity entity : entities) {
            scene.registry().get<TransformComponent>(entity).local[3].y += 1.0f;
            scene.markTransformDirty(entity);
        }
    }
    scene.updateWorldTransforms();
    CHECK_MESSAGE(scene.lastTransformUpdateStats().recomputed == count,
                  "marking each entity dirty twice recomputes each once");
    CHECK_MESSAGE(scene.lastTransformUpdateStats().visited == count, "each dirty entity is visited once");
    scene.updateWorldTransforms();
    CHECK_MESSAGE(scene.lastTransformUpdateStats().visited == 0, "nothing stays queued after an update");
}

TEST_CASE("SceneEntityIndex: dirty branches under dirty parents") {
    Scene scene;
    const Entity parent = scene.createEntity("Parent");
    const Entity child = scene.createEntity("Child");
    const Entity grandchild = scene.createEntity("Grandchild");
    scene.setParent(child, parent);
    scene.setParent(grandchild, child);
    scene.updateWorldTransforms();

    scene.registry().get<TransformComponent>(grandchild).local = translation(0.0f, 0.0f, 1.0f);
    scene.markTransformDirty(grandchild);
    scene.registry().get<TransformComponent>(parent).local = translation(2.0f, 0.0f, 0.0f);
    scene.markTransformDirty(parent);
    scene.updateWorldTransforms();
    CHECK_MESSAGE(
        nearlyEqual(scene.registry().get<TransformComponent>(grandchild).world, translation(2.0f, 0.0f, 1.0f)),
        "a dirty descendant of a dirty root gets both changes");

    // A child added under a parent that is still dirty.
    scene.registry().get<TransformComponent>(parent).local = translation(3.0f, 0.0f, 0.0f);
    scene.markTransformDirty(parent);
    const Entity late = scene.createEntity("Late child");
    scene.registry().get<TransformComponent>(late).local = translation(0.0f, 1.0f, 0.0f);
    scene.registry().get<RelationshipComponent>(late).parent = parent;
    scene.registry().get<RelationshipComponent>(parent).children.push_back(late);
    scene.updateWorldTransforms();
    CHECK_MESSAGE(nearlyEqual(scene.registry().get<TransformComponent>(late).world, translation(3.0f, 1.0f, 0.0f)),
                  "a child added under a dirty parent follows it");
}

TEST_CASE("SceneEntityIndex: reparent between updates") {
    Scene scene;
    const Entity left = scene.createEntity("Left");
    const Entity right = scene.createEntity("Right");
    const Entity child = scene.createEntity("Child");
    scene.registry().get<TransformComponent>(left).local = translation(-5.0f, 0.0f, 0.0f);
    scene.registry().get<TransformComponent>(right).local = translation(5.0f, 0.0f, 0.0f);
    scene.registry().get<TransformComponent>(child).local = translation(0.0f, 1.0f, 0.0f);
    scene.setParent(child, left);
    scene.updateWorldTransforms();
    CHECK_MESSAGE(nearlyEqual(scene.registry().get<TransformComponent>(child).world, translation(-5.0f, 1.0f, 0.0f)),
                  "the child follows its first parent");

    scene.registry().get<TransformComponent>(left).local = translation(-6.0f, 0.0f, 0.0f);
    scene.markTransformDirty(left);
    scene.setParent(child, right);
    scene.registry().get<TransformComponent>(right).local = translation(6.0f, 0.0f, 0.0f);
    scene.markTransformDirty(right);
    scene.updateWorldTransforms();
    CHECK_MESSAGE(nearlyEqual(scene.registry().get<TransformComponent>(child).world, translation(6.0f, 1.0f, 0.0f)),
                  "a child reparented between updates follows its new parent");
    CHECK_MESSAGE((scene.registry().get<RelationshipComponent>(left).children.empty()),
                  "the old parent lost the child");
}
} // namespace

TEST_SUITE_END();
