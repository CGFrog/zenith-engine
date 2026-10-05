// Scene's gameplay transform API: setters mark entities dirty, world readers
// resolve dirty ancestors on demand, the TRS view round-trips, and the audit
// reports transforms changed without a mark.

#include "engine/diagnostics/Logger.hpp"
#include "engine/scene/Scene.hpp"

#include <cmath>
#include <glm/gtc/epsilon.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <string>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

bool nearlyEqual(const glm::mat4& left, const glm::mat4& right, float tolerance = 1.0e-4f) {
    for (glm::length_t column = 0; column < 4; ++column) {
        if (!glm::all(glm::epsilonEqual(left[column], right[column], tolerance))) {
            return false;
        }
    }
    return true;
}

bool nearlyEqual(const glm::vec3& left, const glm::vec3& right, float tolerance = 1.0e-4f) {
    return glm::all(glm::epsilonEqual(left, right, tolerance));
}

// q and -q are the same rotation.
bool sameRotation(const glm::quat& left, const glm::quat& right) {
    return std::abs(glm::dot(left, right)) > 1.0f - 1.0e-5f;
}

const glm::mat4& storedWorld(const Scene& scene, Entity entity) {
    return scene.registry().get<TransformComponent>(entity).world;
}

void testSettersMarkDirty() {
    Scene scene;
    const Entity parent = scene.createEntity("Parent");
    const Entity child = scene.createEntity("Child");
    scene.setParent(child, parent);
    scene.updateWorldTransforms();

    scene.setPosition(parent, {1.0f, 2.0f, 3.0f});
    scene.updateWorldTransforms();
    expect(scene.lastTransformUpdateStats().recomputed == 2, "setPosition updates the entity's branch");
    expect(nearlyEqual(glm::vec3(storedWorld(scene, child)[3]), {1.0f, 2.0f, 3.0f}),
           "a child follows its parent's setPosition");

    scene.translate(child, {0.0f, 1.0f, 0.0f});
    scene.updateWorldTransforms();
    expect(scene.lastTransformUpdateStats().recomputed == 1, "translate updates only the moved entity");
    expect(nearlyEqual(glm::vec3(storedWorld(scene, child)[3]), {1.0f, 3.0f, 3.0f}),
           "translate offsets in the parent's space");

    const glm::quat quarterTurn = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    scene.setRotation(parent, quarterTurn);
    scene.updateWorldTransforms();
    expect(nearlyEqual(glm::vec3(storedWorld(scene, parent)[3]), {1.0f, 2.0f, 3.0f}), "setRotation keeps the position");
    expect(sameRotation(scene.localRotation(parent), quarterTurn), "setRotation sets the rotation");

    scene.setScale(parent, {2.0f, 2.0f, 2.0f});
    scene.updateWorldTransforms();
    expect(nearlyEqual(scene.localScale(parent), {2.0f, 2.0f, 2.0f}) &&
               sameRotation(scene.localRotation(parent), quarterTurn),
           "setScale sets the scale and keeps the rotation");
    // The child sits one unit up in the parent's space, scaled by two.
    expect(nearlyEqual(glm::vec3(storedWorld(scene, child)[3]), {1.0f, 4.0f, 3.0f}),
           "a child follows its parent's scale");

    const glm::mat4 local = glm::translate(glm::mat4(1.0f), {-5.0f, 0.0f, 0.0f});
    scene.setLocalTransform(parent, local);
    scene.updateWorldTransforms();
    expect(storedWorld(scene, parent) == local, "setLocalTransform marks the entity dirty");

    const glm::mat4 world = glm::translate(glm::mat4(1.0f), {7.0f, 8.0f, 9.0f});
    scene.setWorldTransform(child, world);
    scene.updateWorldTransforms();
    expect(nearlyEqual(storedWorld(scene, child), world), "setWorldTransform places the child in world space");
    expect(nearlyEqual(scene.localPosition(child), {12.0f, 8.0f, 9.0f}),
           "setWorldTransform stores the parent-relative transform");

    // Setters and readers tolerate stale handles.
    const Entity destroyed = scene.createEntity("Destroyed");
    scene.destroyEntity(destroyed);
    scene.setPosition(destroyed, {1.0f, 1.0f, 1.0f});
    expect(scene.worldTransform(destroyed) == glm::mat4(1.0f) && scene.localPosition(destroyed) == glm::vec3(0.0f),
           "an invalid entity reads as identity");
}

void testWorldTransformResolvesDirtyAncestors() {
    Scene scene;
    const Entity root = scene.createEntity("Root");
    const Entity middle = scene.createEntity("Middle");
    const Entity leaf = scene.createEntity("Leaf");
    scene.setParent(middle, root);
    scene.setParent(leaf, middle);
    scene.setPosition(middle, {0.0f, 1.0f, 0.0f});
    scene.setPosition(leaf, {0.0f, 0.0f, 1.0f});
    scene.updateWorldTransforms();

    scene.setLocalTransform(root, glm::rotate(glm::translate(glm::mat4(1.0f), {10.0f, 0.0f, 0.0f}), glm::radians(90.0f),
                                              glm::vec3(0.0f, 1.0f, 0.0f)));
    scene.translate(middle, {0.0f, 1.0f, 0.0f});
    const glm::mat4 staleWorld = storedWorld(scene, leaf);
    const glm::mat4 resolved = scene.worldTransform(leaf);
    expect(staleWorld == glm::translate(glm::mat4(1.0f), {0.0f, 1.0f, 1.0f}),
           "the stored world transform is stale until the update");
    // Rotating +Z a quarter turn about +Y gives +X.
    expect(nearlyEqual(scene.worldPosition(leaf), {11.0f, 2.0f, 0.0f}),
           "worldPosition resolves dirty ancestors without an update");
    expect(storedWorld(scene, leaf) == staleWorld, "resolving a world transform changes nothing");

    const Scene& constScene = scene;
    expect(nearlyEqual(constScene.worldTransform(leaf), resolved), "a const scene resolves world transforms");
    scene.updateWorldTransforms();
    expect(nearlyEqual(storedWorld(scene, leaf), resolved), "the resolved world transform matches the update");
}

void testTrsRoundTrip() {
    const TransformTrs trs{.position = {1.0f, -2.0f, 3.5f},
                           .rotation = glm::normalize(glm::quat(0.9f, 0.1f, -0.3f, 0.2f)),
                           .scale = {2.0f, 0.5f, 3.0f}};
    const glm::mat4 composed = composeTrs(trs);
    const TransformTrs decomposed = decomposeTrs(composed);
    expect(nearlyEqual(decomposed.position, trs.position) && nearlyEqual(decomposed.scale, trs.scale) &&
               sameRotation(decomposed.rotation, trs.rotation),
           "TRS decomposition round-trips");
    expect(nearlyEqual(composeTrs(decomposed), composed), "TRS composition round-trips");

    const glm::mat4 mirrored = glm::scale(composed, {-1.0f, 1.0f, 1.0f});
    const TransformTrs mirroredTrs = decomposeTrs(mirrored);
    expect(mirroredTrs.scale.x < 0.0f && nearlyEqual(composeTrs(mirroredTrs), mirrored),
           "a mirrored basis decomposes to a negative X scale");

    // Shear: Y leans toward X. The X axis survives; the shear is dropped.
    glm::mat4 sheared(1.0f);
    sheared[1] = glm::vec4(0.5f, 1.0f, 0.0f, 0.0f);
    const TransformTrs shearedTrs = decomposeTrs(sheared);
    expect(sameRotation(shearedTrs.rotation, glm::quat(1.0f, 0.0f, 0.0f, 0.0f)) &&
               nearlyEqual(shearedTrs.scale, {1.0f, glm::length(glm::vec3(0.5f, 1.0f, 0.0f)), 1.0f}),
           "a sheared basis keeps its X axis and column lengths");

    Scene scene;
    const Entity entity = scene.createEntity("Entity");
    scene.setLocalTransform(entity, composed);
    const TransformTrs view = scene.localTrs(entity);
    expect(scene.localPosition(entity) == trs.position && nearlyEqual(view.scale, trs.scale) &&
               sameRotation(scene.localRotation(entity), trs.rotation),
           "the scene's TRS view reads the local transform");
}

void testAuditReportsMissedMarks() {
    Logger logger({.echoToStderr = false});
    Scene scene;
    scene.setLogChannel({&logger, "scene"});
    expect(scene.transformAuditInterval() == Scene::defaultTransformAuditInterval,
           "the audit interval defaults to the same value in every configuration");
    scene.setTransformAuditInterval(3);
    const Entity entity = scene.createEntity("Forgotten");
    scene.updateWorldTransforms();
    expect(scene.lastTransformUpdateStats().unmarked == 0, "marked changes are not reported");

    const auto writeWithoutMark = [&](float x) {
        scene.registry().get<TransformComponent>(entity).local = glm::translate(glm::mat4(1.0f), {x, 0.0f, 0.0f});
    };
    writeWithoutMark(4.0f);
    // The first call audited; two more pass before the next audit.
    scene.updateWorldTransforms();
    scene.updateWorldTransforms();
    expect(storedWorld(scene, entity)[3].x == 0.0f, "an unmarked write waits for the audit");
    expect(scene.lastTransformUpdateStats().unmarked == 0, "calls between audits report nothing");
    scene.updateWorldTransforms();
    expect(scene.lastTransformUpdateStats().unmarked == 1, "the audit counts the unmarked entity");
    expect(storedWorld(scene, entity)[3].x == 4.0f, "the audit updates the unmarked entity");
    std::vector<LogEntry> warnings = logger.entries();
    expect(warnings.size() == 1 && warnings.front().level == LogLevel::Warning &&
               warnings.front().category == "scene" &&
               warnings.front().message.find("'Forgotten'") != std::string::npos,
           "the audit warns once, naming the entity");

    writeWithoutMark(6.0f);
    for (int call = 0; call < 3; ++call) {
        scene.updateWorldTransforms();
    }
    expect(scene.lastTransformUpdateStats().unmarked == 1 && storedWorld(scene, entity)[3].x == 6.0f,
           "a repeated missed mark is still repaired");
    expect(logger.entries().size() == 1, "each entity is reported once");

    scene.setTransformAuditInterval(0);
    writeWithoutMark(8.0f);
    for (int call = 0; call < 10; ++call) {
        scene.updateWorldTransforms();
    }
    expect(storedWorld(scene, entity)[3].x == 6.0f, "an interval of zero turns the audit off");

    scene.setTransformAuditInterval(1);
    scene.updateWorldTransforms();
    expect(scene.lastTransformUpdateStats().unmarked == 1 && storedWorld(scene, entity)[3].x == 8.0f,
           "an interval of one audits every call");
    scene.setPosition(entity, {9.0f, 0.0f, 0.0f});
    scene.updateWorldTransforms();
    expect(scene.lastTransformUpdateStats().unmarked == 0, "the setters never trip the audit");
}
} // namespace

int main() {
    testSettersMarkDirty();
    testWorldTransformResolvesDirtyAncestors();
    testTrsRoundTrip();
    testAuditReportsMissedMarks();
    return failures == 0 ? 0 : 1;
}
