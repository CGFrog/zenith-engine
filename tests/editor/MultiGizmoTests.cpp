// The viewport gizmo with several objects selected (editor/viewport/MultiGizmo)
// and the pivoted-transform maths it shares with the modal transforms.

#include "EditorTestFixture.hpp"

#include "editor/interaction/ModalTransform.hpp"
#include "editor/viewport/MultiGizmo.hpp"

#include "support/Doctest.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <iostream>
#include <vector>

TEST_SUITE_BEGIN("MultiGizmo");

namespace {

bool near(glm::vec3 left, glm::vec3 right, float tolerance = 0.001f) {
    return glm::length(left - right) <= tolerance;
}

bool near(float left, float right, float tolerance = 0.001f) {
    return std::abs(left - right) <= tolerance;
}

glm::mat4 at(glm::vec3 position) {
    return glm::translate(glm::mat4(1.0f), position);
}

glm::vec3 position(const glm::mat4& world) {
    return glm::vec3(world[3]);
}

glm::mat3 turnAboutZ(float degrees) {
    return glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(degrees), glm::vec3(0.0f, 0.0f, 1.0f)));
}

bool perpendicularAxes(const glm::mat4& world) {
    return near(glm::dot(glm::vec3(world[0]), glm::vec3(world[1])), 0.0f) &&
           near(glm::dot(glm::vec3(world[1]), glm::vec3(world[2])), 0.0f) &&
           near(glm::dot(glm::vec3(world[0]), glm::vec3(world[2])), 0.0f);
}

TEST_CASE("MultiGizmo: pivot modes") {
    const std::vector<glm::mat4> start{at({1.0f, 0.0f, 0.0f}), at({3.0f, 0.0f, 0.0f})};
    const PivotedTransform halfTurn{.rotation = turnAboutZ(180.0f)};

    const glm::vec3 median = transformPivotPoint({.mode = TransformPivot::MedianPoint}, start);
    CHECK_MESSAGE(near(median, {2.0f, 0.0f, 0.0f}), "the median pivot is the mean of the origins");
    const std::vector<glm::mat4> aboutMedian =
        applyPivotedTransform(start, TransformPivot::MedianPoint, median, glm::mat3(1.0f), halfTurn);
    CHECK_MESSAGE(
        (near(position(aboutMedian[0]), {3.0f, 0.0f, 0.0f}) && near(position(aboutMedian[1]), {1.0f, 0.0f, 0.0f})),
        "a half turn about the median swaps the two objects");

    const std::vector<glm::mat4> individual =
        applyPivotedTransform(start, TransformPivot::IndividualOrigins, median, glm::mat3(1.0f), halfTurn);
    CHECK_MESSAGE(
        (near(position(individual[0]), {1.0f, 0.0f, 0.0f}) && near(position(individual[1]), {3.0f, 0.0f, 0.0f})),
        "individual origins turn each object in place");
    CHECK_MESSAGE(near(glm::vec3(individual[0][0]), {-1.0f, 0.0f, 0.0f}), "individual origins still turn each object");

    const glm::vec3 cursor(0.0f, 0.0f, 0.0f);
    const std::vector<glm::mat4> aboutCursor =
        applyPivotedTransform(start, TransformPivot::Cursor, cursor, glm::mat3(1.0f), {.scale = glm::vec3(2.0f)});
    CHECK_MESSAGE(
        (near(position(aboutCursor[0]), {2.0f, 0.0f, 0.0f}) && near(position(aboutCursor[1]), {6.0f, 0.0f, 0.0f})),
        "scaling about the cursor spreads the objects from it");
    CHECK_MESSAGE(near(glm::length(glm::vec3(aboutCursor[0][1])), 2.0f), "uniform scaling grows every axis");

    const std::vector<glm::mat4> moved = applyPivotedTransform(start, TransformPivot::IndividualOrigins, median,
                                                               glm::mat3(1.0f), {.translation = {0.0f, 1.0f, 0.0f}});
    CHECK_MESSAGE((near(position(moved[0]), {1.0f, 1.0f, 0.0f}) && near(position(moved[1]), {3.0f, 1.0f, 0.0f})),
                  "translation moves every object whatever the pivot");
}

TEST_CASE("MultiGizmo: no shear") {
    // A rotated, non-uniformly scaled object scaled on world X must keep
    // perpendicular axes.
    const glm::mat4 rotated = at({1.0f, 0.0f, 0.0f}) * glm::mat4(turnAboutZ(30.0f)) *
                              glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 2.0f, 1.0f));
    const std::vector<glm::mat4> start{rotated, at({-1.0f, 0.0f, 0.0f})};
    const std::vector<glm::mat4> scaled =
        applyPivotedTransform(start, TransformPivot::MedianPoint, glm::vec3(0.0f), glm::mat3(1.0f),
                              {.rotation = turnAboutZ(45.0f), .scale = {3.0f, 1.0f, 1.0f}});
    CHECK_MESSAGE(perpendicularAxes(scaled[0]), "rotated objects scaled together never shear");
    CHECK_MESSAGE(perpendicularAxes(scaled[1]), "unrotated objects scaled together never shear");
    CHECK_MESSAGE(near(glm::length(glm::vec3(scaled[1][0])), 3.0f), "an aligned object stretches by the full factor");
}

TEST_CASE("MultiGizmo: gizmo decomposition") {
    const glm::mat3 axes = turnAboutZ(30.0f);
    glm::mat4 gizmoStart(axes);
    gizmoStart[3] = glm::vec4(2.0f, 0.0f, 0.0f, 1.0f);
    // ImGuizmo's local scale multiplies the frame's columns.
    const glm::mat4 scaledGizmo = gizmoStart * glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 1.0f, 1.0f));
    const std::optional<PivotedTransform> scale = decomposeGizmoChange(gizmoStart, scaledGizmo);
    CHECK_MESSAGE((scale && near(scale->scale, {2.0f, 1.0f, 1.0f}) && near(scale->translation, glm::vec3(0.0f))),
                  "a local gizmo scale is a scale along the gizmo's axes");
    if (scale) {
        CHECK_MESSAGE(near(scale->rotation * glm::vec3(1.0f, 0.0f, 0.0f), {1.0f, 0.0f, 0.0f}), "scaling does not turn");
    }

    glm::mat4 turnedGizmo = glm::mat4(turnAboutZ(90.0f)) * gizmoStart;
    turnedGizmo[3] = gizmoStart[3] + glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
    const std::optional<PivotedTransform> turn = decomposeGizmoChange(gizmoStart, turnedGizmo);
    CHECK_MESSAGE((turn && near(turn->scale, glm::vec3(1.0f)) && near(turn->translation, {0.0f, 0.0f, 1.0f})),
                  "a turned and moved gizmo has no scale");
    if (turn) {
        CHECK_MESSAGE(near(turn->rotation * glm::vec3(1.0f, 0.0f, 0.0f), {0.0f, 1.0f, 0.0f}),
                      "the decomposed rotation is the gizmo's world rotation");
    }

    const std::vector<glm::mat4> objects{at({1.0f, 0.0f, 0.0f}), at({3.0f, 0.0f, 0.0f})};
    glm::mat4 turnedInPlace = glm::mat4(turnAboutZ(90.0f)) * gizmoStart;
    turnedInPlace[3] = gizmoStart[3];
    const std::vector<glm::mat4> turned =
        applyGizmoChange(objects, TransformPivot::MedianPoint, gizmoStart, turnedInPlace);
    CHECK_MESSAGE((near(position(turned[0]), {2.0f, -1.0f, 0.0f}) && near(position(turned[1]), {2.0f, 1.0f, 0.0f})),
                  "turning the gizmo turns the objects about its origin");
    CHECK_MESSAGE(!decomposeGizmoChange(glm::mat4(0.0f), gizmoStart), "a degenerate start frame cannot be decomposed");
}

TEST_CASE("MultiGizmo: gizmo frame") {
    const std::vector<glm::mat4> objects{at({0.0f, 0.0f, 0.0f}), at({4.0f, 0.0f, 0.0f})};
    const GizmoPlacement aboutCursor{.pivot = {.mode = TransformPivot::Cursor, .cursor = {0.0f, 5.0f, 0.0f}},
                                     .orientation = turnAboutZ(90.0f)};
    const glm::mat4 rotating = gizmoFrame(objects, aboutCursor);
    CHECK_MESSAGE(near(position(rotating), {0.0f, 5.0f, 0.0f}), "rotating about the cursor puts the gizmo on it");
    CHECK_MESSAGE(near(glm::vec3(rotating[0]), {0.0f, 1.0f, 0.0f}), "the gizmo takes the orientation it is given");
    GizmoPlacement translating = aboutCursor;
    translating.translating = true;
    CHECK_MESSAGE(near(position(gizmoFrame(objects, translating)), {2.0f, 0.0f, 0.0f}),
                  "translating with the cursor pivot moves the median");
    const GizmoPlacement active{
        .pivot = {.mode = TransformPivot::ActiveElement, .activeOrigin = glm::vec3(4.0f, 0.0f, 0.0f)}};
    CHECK_MESSAGE(near(position(gizmoFrame(objects, active)), {4.0f, 0.0f, 0.0f}),
                  "the active pivot is the active origin");
}

// A drag of several objects, parented and not, commits as one undo step.
TEST_CASE("MultiGizmo: drag commits one step") {
    EditorTestFixture fixture;
    Scene& scene = fixture.scene;
    const Entity parent = scene.createEntity("Parent");
    scene.registry().get<TransformComponent>(parent).local = at({0.0f, 2.0f, 0.0f});
    const Entity child = scene.createEntity("Child");
    scene.setParent(child, parent);
    scene.registry().get<TransformComponent>(child).local = at({1.0f, 0.0f, 0.0f});
    const Entity loose = scene.createEntity("Loose");
    scene.registry().get<TransformComponent>(loose).local = at({-1.0f, 0.0f, 0.0f});
    scene.markTransformDirty(parent);
    scene.markTransformDirty(loose);
    scene.updateWorldTransforms();

    SceneEditing editing = fixture.editing();
    const std::vector<Entity> dragged{child, loose};
    std::vector<glm::mat4> start;
    for (const Entity entity : dragged) {
        start.push_back(scene.registry().get<TransformComponent>(entity).world);
    }
    SceneEditing::EntitySnapshot before;
    CHECK_MESSAGE(editing.captureEntities(dragged, before), "the dragged entities should be captured");
    const glm::mat4 gizmoStart = gizmoFrame(start, {});
    glm::mat4 gizmoNow = gizmoStart;
    gizmoNow[3] += glm::vec4(0.0f, 0.0f, 3.0f, 0.0f);
    setWorldTransforms(scene, dragged, applyGizmoChange(start, TransformPivot::MedianPoint, gizmoStart, gizmoNow));
    CHECK_MESSAGE(near(position(scene.registry().get<TransformComponent>(child).world), {1.0f, 2.0f, 3.0f}),
                  "a parented object moves in world space");
    CHECK_MESSAGE(near(position(scene.registry().get<TransformComponent>(child).local), {1.0f, 0.0f, 3.0f}),
                  "a parented object keeps its parent, storing the move locally");
    const SceneEntityId childId = scene.id(child);
    const SceneEntityId looseId = scene.id(loose);
    const size_t steps = fixture.history.size();
    editing.commit("Transform Gizmo", {}, std::move(before));
    CHECK_MESSAGE(fixture.history.size() == steps + 1, "the drag records one undo step");
    editing.undo();
    scene.updateWorldTransforms();
    const Entity childAfter = scene.findEntity(childId);
    CHECK_MESSAGE((childAfter != entt::null &&
                   near(position(scene.registry().get<TransformComponent>(childAfter).world), {1.0f, 2.0f, 0.0f})),
                  "one undo puts the first object back");
    const Entity looseAfter = scene.findEntity(looseId);
    CHECK_MESSAGE((looseAfter != entt::null &&
                   near(position(scene.registry().get<TransformComponent>(looseAfter).world), {-1.0f, 0.0f, 0.0f})),
                  "the same undo puts the second object back");
}
} // namespace

TEST_SUITE_END();
