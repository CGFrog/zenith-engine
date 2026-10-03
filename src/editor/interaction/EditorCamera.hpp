#pragma once

#include "engine/assets/Assets.hpp"
#include "engine/graphics/Rendering.hpp"
#include "engine/input/InputManager.hpp"
#include "engine/scene/Scene.hpp"

#include <glm/glm.hpp>

#include <functional>
#include <optional>
#include <vector>

// Camera math for any lens described as a RenderView (the editor camera's, or
// a scene camera a viewport looks through). The view space is right-handed
// and looks along -Z; aspectRatio is width over height.
glm::mat4 lensViewMatrix(const RenderView& lens);
// Zero-to-one depth with Y flipped for Vulkan, as picking and modal
// transforms project points.
glm::mat4 lensProjectionMatrix(const RenderView& lens, float aspectRatio);
// The OpenGL-style projection ImGuizmo expects.
glm::mat4 lensGizmoProjectionMatrix(const RenderView& lens, float aspectRatio);
// The closest mesh hit by the ray through normalizedViewportPosition (0..1,
// top-left origin), or entt::null. Only entities the editor may select
// (SceneVisibility::selectable) are hit; a collection instance's draw picks
// its instancing entity. isolatedObjects, when not null, holds the sorted
// render objects a local view isolates (as RenderRuntime::
// setViewVisibleEntities takes them) and limits picks to those.
Entity pickThroughLens(const Scene& scene, const AssetManager& assets, const RenderView& lens,
                       glm::vec2 normalizedViewportPosition, float aspectRatio,
                       const std::vector<RenderObjectId>* isolatedObjects = nullptr);

// One entity a lens ray hits; distance is in world units along the ray.
struct LensHit {
    Entity entity{entt::null};
    float distance{0.0f};
};

// Every entity pickThroughLens could return for the ray, nearest first, each
// once at its nearest hit, so Alt+click cycling reaches occluded objects.
std::vector<LensHit> pickAllThroughLens(const Scene& scene, const AssetManager& assets, const RenderView& lens,
                                        glm::vec2 normalizedViewportPosition, float aspectRatio,
                                        const std::vector<RenderObjectId>* isolatedObjects = nullptr);
// Entities pickThroughLens could return whose mesh bounds centre (the origin
// for a mesh not loaded) projects in front of the lens to a normalized
// viewport position `contains` accepts, each once, in scene order. The CPU
// stand-in for box and lasso selection when no GPU pick is available.
std::vector<Entity> pickInsideLens(const Scene& scene, const AssetManager& assets, const RenderView& lens,
                                   float aspectRatio, const std::function<bool(glm::vec2)>& contains,
                                   const std::vector<RenderObjectId>* isolatedObjects = nullptr);
// The lens of entity's CameraComponent at its world transform (scale is
// ignored), or nullopt when the entity has no camera. World transforms must
// be current.
std::optional<RenderView> sceneCameraLens(const Scene& scene, Entity entity);

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
    // Lens limits, in radians and world units.
    float minimumVerticalFovRadians{0.0174532925f};
    float maximumVerticalFovRadians{2.96705973f};
    float minimumNearPlane{0.0001f};
    float maximumFarPlane{1000000.0f};
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
    // The lens shared by renderView, both projections, and picking.
    float verticalFovRadians() const;
    // Clamps to the configured field-of-view range.
    void setVerticalFovRadians(float verticalFovRadians);
    float nearPlane() const;
    float farPlane() const;
    // Clamps nearPlane to at least minimumNearPlane and farPlane to at most
    // maximumFarPlane, then keeps farPlane beyond nearPlane, so the depth
    // range is never empty or inverted.
    void setClipPlanes(float nearPlane, float farPlane);
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
    float m_verticalFovRadians{1.04719755f};
    float m_nearPlane{0.1f};
    float m_farPlane{1000.0f};
    EditorCameraConfiguration m_configuration;
};
