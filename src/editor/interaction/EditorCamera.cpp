#include "EditorCamera.hpp"

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>

namespace {
bool intersectSphere(glm::vec3 origin, glm::vec3 direction, glm::vec3 center, float radius, float& distance) {
    const glm::vec3 offset = origin - center;
    const float halfB = glm::dot(offset, direction);
    const float discriminant = halfB * halfB - (glm::dot(offset, offset) - radius * radius);
    if (discriminant < 0.0f) {
        return false;
    }
    const float root = std::sqrt(discriminant);
    const float nearDistance = -halfB - root;
    const float farDistance = -halfB + root;
    distance = nearDistance >= 0.0f ? nearDistance : farDistance;
    return distance >= 0.0f;
}

bool intersectTriangle(glm::vec3 origin, glm::vec3 direction, glm::vec3 a, glm::vec3 b, glm::vec3 c, float& distance) {
    constexpr float epsilon = 0.000001f;
    const glm::vec3 edgeAB = b - a;
    const glm::vec3 edgeAC = c - a;
    const glm::vec3 perpendicular = glm::cross(direction, edgeAC);
    const float determinant = glm::dot(edgeAB, perpendicular);
    if (std::abs(determinant) < epsilon) {
        return false;
    }
    const float inverseDeterminant = 1.0f / determinant;
    const glm::vec3 offset = origin - a;
    const float u = glm::dot(offset, perpendicular) * inverseDeterminant;
    if (u < 0.0f || u > 1.0f) {
        return false;
    }
    const glm::vec3 crossOffset = glm::cross(offset, edgeAB);
    const float v = glm::dot(direction, crossOffset) * inverseDeterminant;
    if (v < 0.0f || u + v > 1.0f) {
        return false;
    }
    distance = glm::dot(edgeAC, crossOffset) * inverseDeterminant;
    return distance >= 0.0f;
}
} // namespace

glm::mat4 lensViewMatrix(const RenderView& lens) {
    return glm::lookAtRH(lens.position, lens.position + lens.forward, lens.up);
}

glm::mat4 lensProjectionMatrix(const RenderView& lens, float aspectRatio) {
    glm::mat4 projection = glm::perspectiveRH_ZO(lens.verticalFovRadians, aspectRatio, lens.nearPlane, lens.farPlane);
    projection[1][1] *= -1.0f;
    return projection;
}

glm::mat4 lensGizmoProjectionMatrix(const RenderView& lens, float aspectRatio) {
    return glm::perspectiveRH_NO(lens.verticalFovRadians, aspectRatio, lens.nearPlane, lens.farPlane);
}

Entity pickThroughLens(const Scene& scene, const AssetManager& assets, const RenderView& lens,
                       glm::vec2 normalizedViewportPosition, float aspectRatio) {
    const glm::mat4 inverseViewProjection =
        glm::inverse(lensProjectionMatrix(lens, aspectRatio) * lensViewMatrix(lens));
    const glm::vec2 ndc = normalizedViewportPosition * 2.0f - 1.0f;
    glm::vec4 nearPoint = inverseViewProjection * glm::vec4(ndc, 0.0f, 1.0f);
    glm::vec4 farPoint = inverseViewProjection * glm::vec4(ndc, 1.0f, 1.0f);
    nearPoint /= nearPoint.w;
    farPoint /= farPoint.w;
    const glm::vec3 direction = glm::normalize(glm::vec3(farPoint - nearPoint));
    const glm::vec3 origin = lens.position;

    Entity closest = entt::null;
    float closestDistance = std::numeric_limits<float>::max();
    auto renderables = scene.registry().view<const MeshRendererComponent, const TransformComponent>();
    for (Entity entity : renderables) {
        const MeshAsset* mesh = assets.mesh(renderables.get<const MeshRendererComponent>(entity).mesh);
        if (mesh == nullptr) {
            continue;
        }
        const glm::mat4& world = renderables.get<const TransformComponent>(entity).world;
        const glm::vec3 center = glm::vec3(world * glm::vec4(mesh->bounds.center, 1.0f));
        const float scale = std::max(
            {glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])), glm::length(glm::vec3(world[2]))});
        float boundsDistance = 0.0f;
        if (!intersectSphere(origin, direction, center, mesh->bounds.radius * scale, boundsDistance)) {
            continue;
        }
        const glm::mat3 linearWorld(world);
        if (std::abs(glm::determinant(linearWorld)) < 0.000001f) {
            continue;
        }
        const glm::mat4 inverseWorld = glm::inverse(world);
        const glm::vec3 localOrigin = glm::vec3(inverseWorld * glm::vec4(origin, 1.0f));
        const glm::vec3 localDirection = glm::vec3(inverseWorld * glm::vec4(direction, 0.0f));
        // Pick against full-detail ranges only; simplified LOD ranges follow
        // them in the same index buffer.
        for (const MeshPrimitive& primitive : mesh->primitives) {
            const size_t end = std::min<size_t>(static_cast<size_t>(primitive.firstIndex) + primitive.indexCount,
                                                mesh->indices.size());
            for (size_t index = primitive.firstIndex; index + 2 < end; index += 3) {
                const uint32_t a = mesh->indices[index];
                const uint32_t b = mesh->indices[index + 1];
                const uint32_t c = mesh->indices[index + 2];
                if (a >= mesh->vertices.size() || b >= mesh->vertices.size() || c >= mesh->vertices.size()) {
                    continue;
                }
                float triangleDistance = 0.0f;
                if (intersectTriangle(localOrigin, localDirection, mesh->vertices[a].position,
                                      mesh->vertices[b].position, mesh->vertices[c].position, triangleDistance) &&
                    triangleDistance < closestDistance) {
                    closest = entity;
                    closestDistance = triangleDistance;
                }
            }
        }
    }
    return closest;
}

std::optional<RenderView> sceneCameraLens(const Scene& scene, Entity entity) {
    if (entity == entt::null || !scene.registry().valid(entity)) {
        return std::nullopt;
    }
    const auto* camera = scene.registry().try_get<CameraComponent>(entity);
    const auto* transform = scene.registry().try_get<TransformComponent>(entity);
    if (camera == nullptr || transform == nullptr) {
        return std::nullopt;
    }
    // The renderer views the scene through the inverse of the camera's world
    // matrix, so the lens follows its translation and rotation axes.
    const glm::mat4& world = transform->world;
    const auto axis = [](const glm::vec4& column, glm::vec3 fallback) {
        const glm::vec3 direction(column);
        return glm::length(direction) > 1e-8f ? glm::normalize(direction) : fallback;
    };
    return RenderView{.position = glm::vec3(world[3]),
                      .forward = -axis(world[2], {0.0f, 0.0f, 1.0f}),
                      .up = axis(world[1], {0.0f, 1.0f, 0.0f}),
                      .verticalFovRadians = camera->verticalFovRadians,
                      .nearPlane = camera->nearPlane,
                      .farPlane = camera->farPlane};
}

EditorCamera::EditorCamera(EditorCameraConfiguration configuration)
    : m_configuration(configuration) {}

void EditorCamera::update(const InputState& input, float deltaSeconds, bool inputActive) {
    if (!inputActive) {
        return;
    }

    const bool middleDown = input.mouseButtonDown(MouseButton::Middle);
    const bool rightDown = input.mouseButtonDown(MouseButton::Right);
    const bool shiftDown = input.keyDown(Key::LeftShift) || input.keyDown(Key::RightShift);
    const bool ctrlDown = input.keyDown(Key::LeftControl) || input.keyDown(Key::RightControl);

    if (rightDown) {
        m_yawRadians += input.mouseDelta.x * m_configuration.lookRadiansPerPixel;
        m_pitchRadians = std::clamp(m_pitchRadians - input.mouseDelta.y * m_configuration.lookRadiansPerPixel,
                                    -m_configuration.maximumPitchRadians, m_configuration.maximumPitchRadians);
    } else if (middleDown && !shiftDown && !ctrlDown) {
        orbit(input.mouseDelta.x * m_configuration.lookRadiansPerPixel,
              -input.mouseDelta.y * m_configuration.lookRadiansPerPixel);
    } else if (middleDown && shiftDown) {
        const float scale = std::max(m_distance, 0.1f) * m_configuration.panDistancePerPixel;
        pan(-input.mouseDelta.x * scale, input.mouseDelta.y * scale);
    } else if (middleDown && ctrlDown) {
        zoom(input.mouseDelta.y * m_configuration.dragZoomPerPixel);
    }

    if (input.mouseWheelDelta.y != 0.0f) {
        if (rightDown) {
            m_speed = std::clamp(m_speed * std::pow(m_configuration.speedWheelMultiplier, input.mouseWheelDelta.y),
                                 m_configuration.minimumSpeed, m_configuration.maximumSpeed);
        } else {
            zoom(-input.mouseWheelDelta.y * m_configuration.wheelZoomStep);
        }
    }

    if (rightDown) {
        glm::vec3 movement(0.0f);
        if (input.keyDown(Key::W)) {
            movement += forward();
        }
        if (input.keyDown(Key::S)) {
            movement -= forward();
        }
        if (input.keyDown(Key::D)) {
            movement += right();
        }
        if (input.keyDown(Key::A)) {
            movement -= right();
        }
        if (input.keyDown(Key::E)) {
            movement += up();
        }
        if (input.keyDown(Key::Q)) {
            movement -= up();
        }
        if (glm::dot(movement, movement) > 0.0f) {
            m_position += glm::normalize(movement) * m_speed * deltaSeconds;
        }
        m_pivot = m_position + forward() * m_distance;
    }
}

void EditorCamera::focus(glm::vec3 worldPoint, float radius) {
    m_pivot = worldPoint;
    m_distance = std::max(radius * 2.5f, 0.5f);
    m_position = m_pivot - forward() * m_distance;
}

void EditorCamera::focusOnBounds(const glm::vec3& center, float radius) {
    focus(center, radius);
}

RenderView EditorCamera::renderView() const {
    return {.position = m_position,
            .forward = forward(),
            .up = {0.0f, 1.0f, 0.0f},
            .verticalFovRadians = m_verticalFovRadians,
            .nearPlane = m_nearPlane,
            .farPlane = m_farPlane};
}

glm::mat4 EditorCamera::viewMatrix() const {
    return lensViewMatrix(renderView());
}

glm::mat4 EditorCamera::projectionMatrix(float aspectRatio) const {
    return lensProjectionMatrix(renderView(), aspectRatio);
}

glm::mat4 EditorCamera::gizmoProjectionMatrix(float aspectRatio) const {
    return lensGizmoProjectionMatrix(renderView(), aspectRatio);
}

Entity EditorCamera::pick(const Scene& scene, const AssetManager& assets, glm::vec2 normalizedViewportPosition,
                          float aspectRatio) const {
    return pickThroughLens(scene, assets, renderView(), normalizedViewportPosition, aspectRatio);
}

float EditorCamera::speed() const {
    return m_speed;
}

void EditorCamera::setSpeed(float speed) {
    m_speed = std::clamp(speed, m_configuration.minimumSpeed, m_configuration.maximumSpeed);
}

float EditorCamera::verticalFovRadians() const {
    return m_verticalFovRadians;
}

void EditorCamera::setVerticalFovRadians(float verticalFovRadians) {
    m_verticalFovRadians = std::clamp(verticalFovRadians, m_configuration.minimumVerticalFovRadians,
                                      m_configuration.maximumVerticalFovRadians);
}

float EditorCamera::nearPlane() const {
    return m_nearPlane;
}

float EditorCamera::farPlane() const {
    return m_farPlane;
}

void EditorCamera::setClipPlanes(float nearPlane, float farPlane) {
    // The smallest far/near ratio keeps the projection's depth range finite.
    constexpr float minimumDepthRatio = 1.001f;
    const float maximumNear = m_configuration.maximumFarPlane / minimumDepthRatio;
    m_nearPlane = std::clamp(nearPlane, m_configuration.minimumNearPlane, maximumNear);
    m_farPlane = std::clamp(farPlane, m_nearPlane * minimumDepthRatio, m_configuration.maximumFarPlane);
}

float EditorCamera::distance() const {
    return m_distance;
}

void EditorCamera::setDistance(float distance) {
    m_distance = std::clamp(distance, m_configuration.minimumDistance, m_configuration.maximumDistance);
    m_position = m_pivot - forward() * m_distance;
}

const glm::vec3& EditorCamera::pivot() const {
    return m_pivot;
}

void EditorCamera::setPivot(const glm::vec3& pivot) {
    m_pivot = pivot;
    m_position = m_pivot - forward() * m_distance;
}

float EditorCamera::yaw() const {
    return m_yawRadians;
}

float EditorCamera::pitch() const {
    return m_pitchRadians;
}

void EditorCamera::setViewAngles(float yawRadians, float pitchRadians) {
    m_yawRadians = yawRadians;
    m_pitchRadians =
        std::clamp(pitchRadians, -m_configuration.maximumPitchRadians, m_configuration.maximumPitchRadians);
    m_position = m_pivot - forward() * m_distance;
}

void EditorCamera::orbitAroundPivot(float deltaYawRadians, float deltaPitchRadians) {
    orbit(deltaYawRadians, deltaPitchRadians);
}

glm::vec3 EditorCamera::forward() const {
    return glm::normalize(glm::vec3(std::sin(m_yawRadians) * std::cos(m_pitchRadians), std::sin(m_pitchRadians),
                                    -std::cos(m_yawRadians) * std::cos(m_pitchRadians)));
}

glm::vec3 EditorCamera::right() const {
    return glm::normalize(glm::cross(forward(), glm::vec3(0.0f, 1.0f, 0.0f)));
}

glm::vec3 EditorCamera::up() const {
    return glm::normalize(glm::cross(right(), forward()));
}

void EditorCamera::orbit(float deltaYaw, float deltaPitch) {
    m_yawRadians += deltaYaw;
    m_pitchRadians = std::clamp(m_pitchRadians + deltaPitch, -m_configuration.maximumPitchRadians,
                                m_configuration.maximumPitchRadians);
    m_position = m_pivot - forward() * m_distance;
}

void EditorCamera::pan(float deltaX, float deltaY) {
    const glm::vec3 cameraRight = right();
    const glm::vec3 cameraUp = up();
    const glm::vec3 offset = cameraRight * deltaX + cameraUp * deltaY;
    m_position += offset;
    m_pivot += offset;
}

void EditorCamera::zoom(float delta) {
    m_distance =
        std::clamp(m_distance * std::exp(delta), m_configuration.minimumDistance, m_configuration.maximumDistance);
    m_position = m_pivot - forward() * m_distance;
}
