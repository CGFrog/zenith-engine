#include "TransformTrs.hpp"

#include <cmath>

namespace {
constexpr float minimumAxisLength = 1.0e-8f;

// A unit vector perpendicular to the unit vector axis.
glm::vec3 anyPerpendicular(const glm::vec3& axis) {
    const glm::vec3 reference = std::abs(axis.x) < 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
    return glm::normalize(glm::cross(axis, reference));
}
} // namespace

TransformTrs decomposeTrs(const glm::mat4& transform) {
    const glm::vec3 xColumn(transform[0]);
    const glm::vec3 yColumn(transform[1]);
    const glm::vec3 zColumn(transform[2]);
    TransformTrs trs;
    trs.position = glm::vec3(transform[3]);
    trs.scale = glm::vec3(glm::length(xColumn), glm::length(yColumn), glm::length(zColumn));

    glm::vec3 xAxis = trs.scale.x > minimumAxisLength ? xColumn / trs.scale.x : glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 yAxis = yColumn - glm::dot(yColumn, xAxis) * xAxis;
    const float yLength = glm::length(yAxis);
    yAxis = yLength > minimumAxisLength ? yAxis / yLength : anyPerpendicular(xAxis);
    if (glm::determinant(glm::mat3(xColumn, yColumn, zColumn)) < 0.0f) {
        // Mirrored: fold the reflection into scale.x so the basis is a rotation.
        trs.scale.x = -trs.scale.x;
        xAxis = -xAxis;
    }
    const glm::vec3 zAxis = glm::cross(xAxis, yAxis);
    trs.rotation = glm::normalize(glm::quat_cast(glm::mat3(xAxis, yAxis, zAxis)));
    return trs;
}

glm::mat4 composeTrs(const TransformTrs& trs) {
    const glm::mat3 rotation = glm::mat3_cast(trs.rotation);
    glm::mat4 transform(1.0f);
    transform[0] = glm::vec4(rotation[0] * trs.scale.x, 0.0f);
    transform[1] = glm::vec4(rotation[1] * trs.scale.y, 0.0f);
    transform[2] = glm::vec4(rotation[2] * trs.scale.z, 0.0f);
    transform[3] = glm::vec4(trs.position, 1.0f);
    return transform;
}
