#include "editor/interaction/ModalTransform.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <iostream>
#include <string>
#include <utility>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

bool near(glm::vec3 left, glm::vec3 right, float tolerance = 0.001f) {
    return glm::length(left - right) <= tolerance;
}

bool near(float left, float right, float tolerance = 0.001f) {
    return std::abs(left - right) <= tolerance;
}

// A camera at +Z looking down -Z, matching the editor's Vulkan projection.
ModalTransformView frontView() {
    glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(60.0f), 800.0f / 600.0f, 0.1f, 1000.0f);
    projection[1][1] *= -1.0f;
    return {.view = glm::lookAtRH(glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
            .projection = projection,
            .viewportPosition = {0.0f, 0.0f},
            .viewportSize = {800.0f, 600.0f}};
}

glm::mat4 at(glm::vec3 position) {
    return glm::translate(glm::mat4(1.0f), position);
}

glm::vec3 position(const glm::mat4& world) {
    return glm::vec3(world[3]);
}

// Starts a transform with the mouse offset from the pivot's screen position.
ModalTransform start(TransformOperation operation, std::vector<glm::mat4> world, glm::vec2 mouseOffset,
                     glm::mat3 localAxes = glm::mat3(1.0f)) {
    const ModalTransform probe(operation, world, localAxes, {0.0f, 0.0f}, frontView());
    return ModalTransform(operation, std::move(world), localAxes, probe.pivotOnScreen() + mouseOffset, frontView());
}

void type(ModalTransform& transform, const std::string& text) {
    for (char character : text) {
        transform.typeCharacter(character);
    }
}

void testTranslation() {
    ModalTransform grab = start(TransformOperation::Translate, {at({1.0f, 2.0f, 0.0f})}, {0.0f, 0.0f});
    const glm::vec2 origin = grab.pivotOnScreen();
    grab.pressAxis(TransformAxis::X, false);
    grab.moveMouse(origin + glm::vec2(80.0f, 40.0f), false);
    const glm::vec3 moved = position(grab.evaluate({}).world.front());
    expect(moved.x > 1.5f && near(moved.y, 2.0f) && near(moved.z, 0.0f),
           "an X constraint should move only along X, following the mouse to the right");

    ModalTransform free = start(TransformOperation::Translate, {at({0.0f, 0.0f, 0.0f})}, {0.0f, 0.0f});
    free.moveMouse(free.pivotOnScreen() + glm::vec2(0.0f, -50.0f), false);
    const glm::vec3 up = position(free.evaluate({}).world.front());
    expect(up.y > 0.1f && near(up.z, 0.0f), "unconstrained moves follow the mouse in the view plane");

    ModalTransform typed = start(TransformOperation::Translate, {at({1.0f, 0.0f, 0.0f})}, {0.0f, 0.0f});
    typed.pressAxis(TransformAxis::Y, false);
    type(typed, "-3");
    expect(typed.numericInputActive(), "typing digits should switch to numeric input");
    const ModalTransformResult result = typed.evaluate({});
    expect(near(position(result.world.front()), {1.0f, -3.0f, 0.0f}), "typed values move exactly along the axis");
    expect(result.description.find("along global Y") != std::string::npos,
           "the header should name the constraint like Blender");
    typed.eraseCharacter();
    typed.eraseCharacter();
    expect(!typed.numericInputActive(), "erasing every character should return control to the mouse");

    ModalTransform snapped = start(TransformOperation::Translate, {at({0.0f, 0.0f, 0.0f})}, {0.0f, 0.0f});
    snapped.moveMouse(snapped.pivotOnScreen() + glm::vec2(37.0f, 0.0f), false);
    const glm::vec3 snappedPosition = position(snapped.evaluate({.enabled = true, .translation = 1.0f}).world.front());
    expect(near(snappedPosition.x, std::round(snappedPosition.x)), "snapping should land on whole increments");

    ModalTransform precise = start(TransformOperation::Translate, {at({0.0f, 0.0f, 0.0f})}, {0.0f, 0.0f});
    ModalTransform coarse = start(TransformOperation::Translate, {at({0.0f, 0.0f, 0.0f})}, {0.0f, 0.0f});
    precise.pressAxis(TransformAxis::X, false);
    coarse.pressAxis(TransformAxis::X, false);
    precise.moveMouse(precise.pivotOnScreen() + glm::vec2(100.0f, 0.0f), true);
    coarse.moveMouse(coarse.pivotOnScreen() + glm::vec2(10.0f, 0.0f), false);
    expect(near(position(precise.evaluate({}).world.front()), position(coarse.evaluate({}).world.front())),
           "holding Shift should move ten times slower");
}

void testAxisCycling() {
    // Local X of an object turned 90 degrees about Y points along world -Z.
    const glm::mat3 turned(glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f)));
    ModalTransform grab = start(TransformOperation::Translate, {at({0.0f, 0.0f, 0.0f})}, {0.0f, 0.0f}, turned);
    grab.pressAxis(TransformAxis::X, false);
    grab.pressAxis(TransformAxis::X, false);
    type(grab, "2");
    expect(near(position(grab.evaluate({}).world.front()), {0.0f, 0.0f, -2.0f}),
           "pressing X twice should constrain to the active object's local X");
    expect(grab.evaluate({}).description.find("local X") != std::string::npos, "the header should say local");
    grab.pressAxis(TransformAxis::X, false);
    expect(grab.constraintGuides().empty(), "a third press should remove the constraint");

    ModalTransform plane = start(TransformOperation::Translate, {at({0.0f, 0.0f, 0.0f})}, {0.0f, 0.0f});
    plane.pressAxis(TransformAxis::Y, true);
    expect(plane.constraintGuides().size() == 2, "Shift+axis should constrain to the plane of the other two axes");
}

void testRotation() {
    ModalTransform rotate =
        start(TransformOperation::Rotate, {at({1.0f, 0.0f, 0.0f}), at({-1.0f, 0.0f, 0.0f})}, {100.0f, 0.0f});
    expect(near(rotate.pivot(), glm::vec3(0.0f)), "the pivot should be the median of the selected origins");
    rotate.pressAxis(TransformAxis::Z, false);
    type(rotate, "90");
    const ModalTransformResult result = rotate.evaluate({});
    expect(near(position(result.world[0]), {0.0f, 1.0f, 0.0f}) && near(position(result.world[1]), {0.0f, -1.0f, 0.0f}),
           "a typed rotation should turn objects about the median point");
    expect(near(glm::vec3(result.world[0][0]), {0.0f, 1.0f, 0.0f}), "objects should also turn in place");

    ModalTransform mouse = start(TransformOperation::Rotate, {at({0.0f, 0.0f, 0.0f})}, {100.0f, 0.0f});
    // Clockwise on screen (Y down) from the right side to below the pivot.
    mouse.moveMouse(mouse.pivotOnScreen() + glm::vec2(0.0f, 100.0f), false);
    const glm::vec3 xAxis(mouse.evaluate({}).world.front()[0]);
    expect(near(xAxis, {0.0f, -1.0f, 0.0f}), "dragging clockwise should turn the object clockwise on screen");
    mouse.moveMouse(mouse.pivotOnScreen() + glm::vec2(-100.0f, 0.0f), false);
    mouse.moveMouse(mouse.pivotOnScreen() + glm::vec2(0.0f, -100.0f), false);
    const ModalTransformResult past = mouse.evaluate({});
    expect(past.description.find("-270") != std::string::npos, "rotation should keep accumulating past half a turn");

    ModalTransform trackball = start(TransformOperation::Rotate, {at({0.0f, 0.0f, 0.0f})}, {100.0f, 0.0f});
    trackball.setOperation(TransformOperation::Rotate);
    expect(trackball.operation() == TransformOperation::Trackball, "pressing R twice should start the trackball");
    trackball.setOperation(TransformOperation::Rotate);
    expect(trackball.operation() == TransformOperation::Rotate, "pressing R again should return to rotation");
}

void testScale() {
    ModalTransform scale =
        start(TransformOperation::Scale, {at({1.0f, 0.0f, 0.0f}), at({3.0f, 0.0f, 0.0f})}, {100.0f, 0.0f});
    scale.moveMouse(scale.pivotOnScreen() + glm::vec2(200.0f, 0.0f), false);
    const ModalTransformResult doubled = scale.evaluate({});
    expect(near(position(doubled.world[0]), {0.0f, 0.0f, 0.0f}) && near(position(doubled.world[1]), {4.0f, 0.0f, 0.0f}),
           "moving the mouse twice as far from the pivot should double the spread");
    expect(near(glm::length(glm::vec3(doubled.world[0][1])), 2.0f), "objects should grow uniformly");

    ModalTransform axis = start(TransformOperation::Scale, {at({0.0f, 0.0f, 0.0f})}, {100.0f, 0.0f});
    axis.pressAxis(TransformAxis::X, false);
    type(axis, "3");
    const glm::mat4 stretched = axis.evaluate({}).world.front();
    expect(near(glm::length(glm::vec3(stretched[0])), 3.0f) && near(glm::length(glm::vec3(stretched[1])), 1.0f),
           "an axis constraint should scale only that axis");

    // A rotated object scaled on global X must not shear.
    const glm::mat4 rotated = glm::rotate(glm::mat4(1.0f), glm::radians(30.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    ModalTransform shear = start(TransformOperation::Scale, {rotated}, {100.0f, 0.0f});
    shear.pressAxis(TransformAxis::X, false);
    type(shear, "2");
    const glm::mat4 result = shear.evaluate({}).world.front();
    expect(near(glm::dot(glm::vec3(result[0]), glm::vec3(result[1])), 0.0f), "scaled axes should stay perpendicular");
}

void testClearTransform() {
    const glm::mat4 local = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 2.0f, 3.0f)) *
                            glm::rotate(glm::mat4(1.0f), glm::radians(45.0f), glm::vec3(0.0f, 1.0f, 0.0f)) *
                            glm::scale(glm::mat4(1.0f), glm::vec3(2.0f));
    const glm::mat4 noLocation = clearTransformPart(local, TransformPart::Location);
    expect(near(position(noLocation), glm::vec3(0.0f)) && near(glm::vec3(noLocation[0]), glm::vec3(local[0])),
           "Alt+G should clear only the location");
    const glm::mat4 noRotation = clearTransformPart(local, TransformPart::Rotation);
    expect(near(glm::vec3(noRotation[0]), {2.0f, 0.0f, 0.0f}) && near(position(noRotation), {1.0f, 2.0f, 3.0f}),
           "Alt+R should clear rotation but keep scale and location");
    const glm::mat4 noScale = clearTransformPart(local, TransformPart::Scale);
    expect(near(glm::length(glm::vec3(noScale[0])), 1.0f) && near(glm::vec3(noScale[0]), glm::vec3(local[0]) * 0.5f),
           "Alt+S should clear scale but keep rotation");
}
} // namespace

int main() {
    testTranslation();
    testAxisCycling();
    testRotation();
    testScale();
    testClearTransform();
    return failures == 0 ? 0 : 1;
}
