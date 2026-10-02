#include "ModalTransform.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <utility>

namespace {
constexpr float precisionFactor = 0.1f;
constexpr float trackballRadiansPerPixel = 0.01f;
constexpr float minimumScaleRadiusPixels = 1.0f;
constexpr std::array<const char*, 3> axisNames{"X", "Y", "Z"};

struct Ray {
    glm::vec3 origin{0.0f};
    glm::vec3 direction{0.0f, 0.0f, -1.0f};
};

struct CameraAxes {
    glm::vec3 right{1.0f, 0.0f, 0.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    // Points from the camera into the scene.
    glm::vec3 forward{0.0f, 0.0f, -1.0f};
};

CameraAxes cameraAxes(const ModalTransformView& view) {
    const glm::mat4 cameraWorld = glm::inverse(view.view);
    return {.right = glm::normalize(glm::vec3(cameraWorld[0])),
            .up = glm::normalize(glm::vec3(cameraWorld[1])),
            .forward = -glm::normalize(glm::vec3(cameraWorld[2]))};
}

glm::vec2 worldToScreen(const ModalTransformView& view, const glm::vec3& point) {
    const glm::vec4 clip = view.projection * view.view * glm::vec4(point, 1.0f);
    const float w = std::abs(clip.w) > 1e-6f ? clip.w : 1e-6f;
    const glm::vec2 ndc = glm::vec2(clip) / w;
    return view.viewportPosition + (ndc * 0.5f + 0.5f) * view.viewportSize;
}

Ray screenRay(const ModalTransformView& view, const glm::vec2& screen) {
    const glm::vec2 ndc = (screen - view.viewportPosition) / view.viewportSize * 2.0f - 1.0f;
    const glm::mat4 inverseViewProjection = glm::inverse(view.projection * view.view);
    glm::vec4 nearPoint = inverseViewProjection * glm::vec4(ndc, 0.0f, 1.0f);
    glm::vec4 farPoint = inverseViewProjection * glm::vec4(ndc, 1.0f, 1.0f);
    nearPoint /= nearPoint.w;
    farPoint /= farPoint.w;
    return {.origin = glm::vec3(nearPoint), .direction = glm::normalize(glm::vec3(farPoint - nearPoint))};
}

std::optional<glm::vec3> intersectPlane(const Ray& ray, const glm::vec3& point, const glm::vec3& normal) {
    const float denominator = glm::dot(ray.direction, normal);
    if (std::abs(denominator) < 1e-6f) {
        return std::nullopt;
    }
    return ray.origin + ray.direction * (glm::dot(point - ray.origin, normal) / denominator);
}

// Parameter along the line point + t * direction closest to the ray.
float closestLineParameter(const Ray& ray, const glm::vec3& point, const glm::vec3& direction) {
    const glm::vec3 offset = point - ray.origin;
    const float alignment = glm::dot(direction, ray.direction);
    const float denominator = 1.0f - alignment * alignment;
    if (denominator < 1e-6f) {
        return 0.0f;
    }
    return (alignment * glm::dot(ray.direction, offset) - glm::dot(direction, offset)) / denominator;
}

float snapTo(float value, float increment) {
    return increment > 0.0f ? std::round(value / increment) * increment : value;
}

std::string format(const char* pattern, auto... values) {
    std::array<char, 256> buffer{};
    std::snprintf(buffer.data(), buffer.size(), pattern, values...);
    return buffer.data();
}

const char* spaceName(ConstraintSpace space) {
    return space == ConstraintSpace::Global ? "global" : "local";
}
} // namespace

ModalTransform::ModalTransform(TransformOperation operation, std::vector<glm::mat4> startWorld, glm::mat3 localAxes,
                               glm::vec2 mouse, ModalTransformView view)
    : m_operation(operation)
    , m_startWorld(std::move(startWorld))
    , m_localAxes(localAxes)
    , m_view(view)
    , m_lastMouse(mouse)
    , m_virtualMouse(mouse)
    , m_originMouse(mouse) {
    // Blender's default pivot: the median point of the transformed origins.
    for (const glm::mat4& world : m_startWorld) {
        m_pivot += glm::vec3(world[3]);
    }
    if (!m_startWorld.empty()) {
        m_pivot /= static_cast<float>(m_startWorld.size());
    }
}

TransformOperation ModalTransform::operation() const {
    return m_operation;
}

void ModalTransform::setOperation(TransformOperation operation) {
    if (operation == TransformOperation::Rotate && m_operation == TransformOperation::Rotate) {
        operation = TransformOperation::Trackball;
    } else if (operation == TransformOperation::Rotate && m_operation == TransformOperation::Trackball) {
        operation = TransformOperation::Rotate;
    }
    m_operation = operation;
    m_constraint = TransformConstraint::None;
    m_space = ConstraintSpace::Global;
    resetMouseOrigin();
}

void ModalTransform::pressAxis(TransformAxis axis, bool plane) {
    if (m_operation == TransformOperation::Trackball) {
        return;
    }
    const TransformConstraint requested = plane ? TransformConstraint::Plane : TransformConstraint::Axis;
    if (m_constraint == requested && m_axis == axis) {
        // Repeated presses cycle global -> local -> unconstrained.
        if (m_space == ConstraintSpace::Global) {
            m_space = ConstraintSpace::Local;
        } else {
            m_constraint = TransformConstraint::None;
            m_space = ConstraintSpace::Global;
        }
        return;
    }
    m_constraint = requested;
    m_axis = axis;
    m_space = ConstraintSpace::Global;
}

void ModalTransform::moveMouse(glm::vec2 position, bool precise) {
    const glm::vec2 delta = position - m_lastMouse;
    m_lastMouse = position;
    const glm::vec2 previous = m_virtualMouse;
    m_virtualMouse += delta * (precise ? precisionFactor : 1.0f);
    if (m_operation == TransformOperation::Rotate) {
        const glm::vec2 center = pivotOnScreen();
        const glm::vec2 from = previous - center;
        const glm::vec2 to = m_virtualMouse - center;
        if (glm::length(from) > 1.0f && glm::length(to) > 1.0f) {
            // Screen Y points down, so a positive cross product is clockwise.
            m_rotationRadians += std::atan2(from.x * to.y - from.y * to.x, glm::dot(from, to));
        }
    }
}

bool ModalTransform::typeCharacter(char character) {
    if (character == '-') {
        m_numericNegated = !m_numericNegated;
        return true;
    }
    if (character == '.' && m_numericText.find('.') == std::string::npos) {
        m_numericText.push_back(character);
        return true;
    }
    if (character >= '0' && character <= '9') {
        m_numericText.push_back(character);
        return true;
    }
    return false;
}

void ModalTransform::eraseCharacter() {
    if (!m_numericText.empty()) {
        m_numericText.pop_back();
    } else {
        m_numericNegated = false;
    }
}

bool ModalTransform::numericInputActive() const {
    return !m_numericText.empty() || m_numericNegated;
}

ModalTransformResult ModalTransform::evaluate(const TransformSnapping& snapping) const {
    switch (m_operation) {
    case TransformOperation::Translate:
        return translate(snapping);
    case TransformOperation::Rotate:
        return rotate(snapping);
    case TransformOperation::Trackball:
        return trackball(snapping);
    case TransformOperation::Scale:
        return scale(snapping);
    }
    return {.world = m_startWorld};
}

glm::vec3 ModalTransform::pivot() const {
    return m_pivot;
}

glm::vec2 ModalTransform::pivotOnScreen() const {
    return worldToScreen(m_view, m_pivot);
}

glm::vec2 ModalTransform::toScreen(const glm::vec3& point) const {
    return worldToScreen(m_view, point);
}

glm::vec2 ModalTransform::mouse() const {
    return m_lastMouse;
}

std::vector<ConstraintGuide> ModalTransform::constraintGuides() const {
    if (m_constraint == TransformConstraint::Axis) {
        return {{.axis = m_axis, .direction = axisDirection(m_axis)}};
    }
    if (m_constraint == TransformConstraint::Plane) {
        std::vector<ConstraintGuide> guides;
        for (TransformAxis axis : {TransformAxis::X, TransformAxis::Y, TransformAxis::Z}) {
            if (axis != m_axis) {
                guides.push_back({.axis = axis, .direction = axisDirection(axis)});
            }
        }
        return guides;
    }
    return {};
}

glm::mat3 ModalTransform::constraintFrame() const {
    return m_space == ConstraintSpace::Local ? m_localAxes : glm::mat3(1.0f);
}

glm::vec3 ModalTransform::axisDirection(TransformAxis axis) const {
    return glm::normalize(constraintFrame()[static_cast<int>(axis)]);
}

float ModalTransform::numericValue() const {
    float value = m_operation == TransformOperation::Scale ? 1.0f : 0.0f;
    if (!m_numericText.empty() && m_numericText != ".") {
        value = std::strtof(m_numericText.c_str(), nullptr);
    }
    return m_numericNegated ? -value : value;
}

void ModalTransform::resetMouseOrigin() {
    m_originMouse = m_virtualMouse;
    m_rotationRadians = 0.0f;
    m_numericText.clear();
    m_numericNegated = false;
}

ModalTransformResult ModalTransform::translate(const TransformSnapping& snapping) const {
    const glm::mat3 frame = constraintFrame();
    glm::vec3 delta(0.0f);
    if (numericInputActive()) {
        // Typed values apply along the constrained axis, or the first free one.
        TransformAxis axis = TransformAxis::X;
        if (m_constraint == TransformConstraint::Axis) {
            axis = m_axis;
        } else if (m_constraint == TransformConstraint::Plane && m_axis == TransformAxis::X) {
            axis = TransformAxis::Y;
        }
        delta = axisDirection(axis) * numericValue();
    } else {
        const Ray origin = screenRay(m_view, m_originMouse);
        const Ray current = screenRay(m_view, m_virtualMouse);
        if (m_constraint == TransformConstraint::Axis) {
            const glm::vec3 axis = axisDirection(m_axis);
            delta = axis * (closestLineParameter(current, m_pivot, axis) - closestLineParameter(origin, m_pivot, axis));
        } else {
            const glm::vec3 normal =
                m_constraint == TransformConstraint::Plane ? axisDirection(m_axis) : cameraAxes(m_view).forward;
            const auto from = intersectPlane(origin, m_pivot, normal);
            const auto to = intersectPlane(current, m_pivot, normal);
            if (from && to) {
                delta = *to - *from;
            }
        }
        if (snapping.enabled) {
            glm::vec3 components = glm::transpose(frame) * delta;
            for (int index = 0; index < 3; ++index) {
                components[index] = snapTo(components[index], snapping.translation);
            }
            delta = frame * components;
        }
    }

    ModalTransformResult result;
    const glm::mat4 translation = glm::translate(glm::mat4(1.0f), delta);
    for (const glm::mat4& world : m_startWorld) {
        result.world.push_back(translation * world);
    }
    const glm::vec3 local = glm::transpose(frame) * delta;
    const std::string typed = "[" + std::string(m_numericNegated ? "-" : "") + m_numericText + "|]";
    if (m_constraint == TransformConstraint::Axis) {
        const float value = local[static_cast<int>(m_axis)];
        result.description =
            numericInputActive()
                ? format("D: %s m along %s %s", typed.c_str(), spaceName(m_space), axisNames[static_cast<int>(m_axis)])
                : format("D: %.4f m along %s %s", value, spaceName(m_space), axisNames[static_cast<int>(m_axis)]);
    } else {
        result.description =
            format("Dx: %.4f m  Dy: %.4f m  Dz: %.4f m (%.4f m)", local.x, local.y, local.z, glm::length(delta));
        if (m_constraint == TransformConstraint::Plane) {
            result.description += format(" locking %s %s", spaceName(m_space), axisNames[static_cast<int>(m_axis)]);
        }
        if (numericInputActive()) {
            result.description = "D: " + typed + " m";
        }
    }
    return result;
}

ModalTransformResult ModalTransform::rotate(const TransformSnapping& snapping) const {
    const glm::vec3 forward = cameraAxes(m_view).forward;
    // Rotation is right-handed about axis; unconstrained rotation turns about
    // the axis toward the viewer so positive angles are counter-clockwise.
    glm::vec3 axis = -forward;
    float angle = -m_rotationRadians;
    if (m_constraint != TransformConstraint::None) {
        axis = axisDirection(m_axis);
        // Follow the mouse visually whichever way the axis faces the camera.
        angle = glm::dot(axis, forward) >= 0.0f ? m_rotationRadians : -m_rotationRadians;
    }
    if (numericInputActive()) {
        angle = glm::radians(numericValue());
    } else if (snapping.enabled) {
        angle = glm::radians(snapTo(glm::degrees(angle), snapping.rotationDegrees));
    }

    ModalTransformResult result;
    const glm::mat4 rotation = glm::translate(glm::mat4(1.0f), m_pivot) * glm::rotate(glm::mat4(1.0f), angle, axis) *
                               glm::translate(glm::mat4(1.0f), -m_pivot);
    for (const glm::mat4& world : m_startWorld) {
        result.world.push_back(rotation * world);
    }
    const std::string value = numericInputActive()
                                  ? "[" + std::string(m_numericNegated ? "-" : "") + m_numericText + "|]"
                                  : format("%.2f", glm::degrees(angle));
    result.description = "Rot: " + value + "\xC2\xB0";
    if (m_constraint != TransformConstraint::None) {
        result.description += format(" along %s %s", spaceName(m_space), axisNames[static_cast<int>(m_axis)]);
    }
    return result;
}

ModalTransformResult ModalTransform::trackball(const TransformSnapping& snapping) const {
    const CameraAxes camera = cameraAxes(m_view);
    const glm::vec2 travel = m_virtualMouse - m_originMouse;
    float yaw = travel.x * trackballRadiansPerPixel;
    float pitch = travel.y * trackballRadiansPerPixel;
    if (snapping.enabled) {
        yaw = glm::radians(snapTo(glm::degrees(yaw), snapping.rotationDegrees));
        pitch = glm::radians(snapTo(glm::degrees(pitch), snapping.rotationDegrees));
    }
    // Dragging right turns the near surface right; dragging down turns it down.
    const glm::mat4 rotation =
        glm::rotate(glm::mat4(1.0f), yaw, camera.up) * glm::rotate(glm::mat4(1.0f), pitch, camera.right);
    const glm::mat4 aboutPivot =
        glm::translate(glm::mat4(1.0f), m_pivot) * rotation * glm::translate(glm::mat4(1.0f), -m_pivot);

    ModalTransformResult result;
    for (const glm::mat4& world : m_startWorld) {
        result.world.push_back(aboutPivot * world);
    }
    result.description = format("Trackball: %.2f %.2f", glm::degrees(yaw), glm::degrees(pitch));
    return result;
}

ModalTransformResult ModalTransform::scale(const TransformSnapping& snapping) const {
    float factor = 1.0f;
    if (numericInputActive()) {
        factor = numericValue();
    } else {
        const glm::vec2 center = pivotOnScreen();
        const float startRadius = glm::length(m_originMouse - center);
        if (startRadius >= minimumScaleRadiusPixels) {
            factor = glm::length(m_virtualMouse - center) / startRadius;
        }
        if (snapping.enabled) {
            factor = snapTo(factor, snapping.scale);
        }
    }

    glm::vec3 factors(factor);
    if (m_constraint == TransformConstraint::Axis) {
        factors = glm::vec3(1.0f);
        factors[static_cast<int>(m_axis)] = factor;
    } else if (m_constraint == TransformConstraint::Plane) {
        factors[static_cast<int>(m_axis)] = 1.0f;
    }
    const glm::mat3 frame = constraintFrame();
    const glm::mat3 scaling = frame * glm::mat3(glm::scale(glm::mat4(1.0f), factors)) * glm::transpose(frame);

    ModalTransformResult result;
    for (const glm::mat4& world : m_startWorld) {
        glm::mat4 scaled = world;
        scaled[3] = glm::vec4(m_pivot + scaling * (glm::vec3(world[3]) - m_pivot), 1.0f);
        // Scale each object along its own axes so rotated objects never shear.
        for (int column = 0; column < 3; ++column) {
            const glm::vec3 basis(world[column]);
            const float length = glm::length(basis);
            if (length > 1e-8f) {
                const glm::vec3 direction = basis / length;
                scaled[column] = glm::vec4(basis * glm::dot(direction, scaling * direction), 0.0f);
            }
        }
        result.world.push_back(scaled);
    }

    if (numericInputActive()) {
        result.description = "Scale: [" + std::string(m_numericNegated ? "-" : "") + m_numericText + "|]";
    } else if (m_constraint == TransformConstraint::Axis) {
        result.description = format("Scale %s: %.4f along %s %s", axisNames[static_cast<int>(m_axis)], factor,
                                    spaceName(m_space), axisNames[static_cast<int>(m_axis)]);
    } else {
        result.description = format("Scale: %.4f %.4f %.4f", factors.x, factors.y, factors.z);
        if (m_constraint == TransformConstraint::Plane) {
            result.description += format(" locking %s %s", spaceName(m_space), axisNames[static_cast<int>(m_axis)]);
        }
    }
    return result;
}

glm::mat4 clearTransformPart(const glm::mat4& local, TransformPart part) {
    glm::mat4 cleared = local;
    if (part == TransformPart::Location) {
        cleared[3] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        return cleared;
    }
    for (int column = 0; column < 3; ++column) {
        const glm::vec3 basis(local[column]);
        const float length = glm::length(basis);
        if (part == TransformPart::Rotation) {
            glm::vec3 axis(0.0f);
            axis[column] = length;
            cleared[column] = glm::vec4(axis, 0.0f);
        } else if (length > 1e-8f) {
            cleared[column] = glm::vec4(basis / length, 0.0f);
        }
    }
    return cleared;
}
