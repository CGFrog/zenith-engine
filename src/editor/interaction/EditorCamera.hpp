#pragma once

#include "engine/assets/Assets.hpp"
#include "engine/graphics/Rendering.hpp"
#include "engine/input/InputManager.hpp"
#include "engine/scene/Scene.hpp"

#include <glm/glm.hpp>

struct EditorCameraConfiguration {
    float lookRadiansPerPixel{0.003f};
    float panDistancePerPixel{0.0015f};
    float dragZoomPerPixel{0.01f};
    float wheelZoomStep{0.18f};
    float speedWheelMultiplier{1.2f};
    float minimumSpeed{0.05f};
    float maximumSpeed{1000.0f};
    float minimumDistance{0.05f};
    float maximumDistance{10000.0f};
    float maximumPitchRadians{1.55334306f};
};

class EditorCamera {
  public:
    explicit EditorCamera(EditorCameraConfiguration configuration = {});

    void update(const InputState& input, float deltaSeconds, bool inputActive);
    void focus(glm::vec3 worldPoint, float radius = 1.0f);
    void focusOnBounds(const glm::vec3& center, float radius);
    RenderView renderView() const;
    glm::mat4 viewMatrix() const;
    glm::mat4 projectionMatrix(float aspectRatio) const;
    glm::mat4 gizmoProjectionMatrix(float aspectRatio) const;
    Entity pick(const Scene& scene, const AssetManager& assets, glm::vec2 normalizedViewportPosition,
                float aspectRatio) const;

    float speed() const;
    void setSpeed(float speed);
    float distance() const;
    void setDistance(float distance);
    const glm::vec3& pivot() const;
    void setPivot(const glm::vec3& pivot);
    // Yaw 0 and pitch 0 look along -Z (Blender's front view); positive yaw
    // turns toward +X and positive pitch looks up. Both keep the pivot fixed.
    float yaw() const;
    float pitch() const;
    void setViewAngles(float yawRadians, float pitchRadians);
    void orbitAroundPivot(float deltaYawRadians, float deltaPitchRadians);

  private:
    glm::vec3 forward() const;
    glm::vec3 right() const;
    glm::vec3 up() const;

    void orbit(float deltaYaw, float deltaPitch);
    void pan(float deltaX, float deltaY);
    void zoom(float delta);

    glm::vec3 m_position{0.0f, 2.0f, 6.0f};
    glm::vec3 m_pivot{0.0f};
    float m_yawRadians{0.0f};
    float m_pitchRadians{-0.32175055f};
    float m_distance{6.32455532f};
    float m_speed{5.0f};
    EditorCameraConfiguration m_configuration;
};
