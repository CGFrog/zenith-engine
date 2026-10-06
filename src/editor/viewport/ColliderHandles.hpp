#pragma once

// Draggable handles that resize collider shapes in the viewport.

#include "editor/application/SceneEditing.hpp"
#include "engine/graphics/Rendering.hpp"
#include "engine/physics/PhysicsComponents.hpp"
#include "engine/scene/Scene.hpp"

#include <cstdint>
#include <glm/glm.hpp>
#include <optional>
#include <vector>

struct EditorContext;
struct ImDrawList;

// Viewport handles that resize the active object's collider: a dot on each
// box face, and on a sphere's or capsule's radius and a capsule's ends,
// dragged along the handle's outward axis. Triangle-mesh colliders have none.

// One draggable point on a collider's shape.
struct ColliderHandle {
    enum class Dimension : uint8_t {
        // BoxCollider::halfExtents[axis].
        BoxHalfExtent,
        // SphereCollider::radius or CapsuleCollider::radius.
        Radius,
        // CapsuleCollider::halfHeight (the handle sits on the cap's tip).
        CapsuleHalfHeight,
    };

    Dimension dimension{Dimension::BoxHalfExtent};
    // The shape axis (0 x, 1 y, 2 z) the handle sits on, and its side (+1 or
    // -1).
    int axis{0};
    float side{1.0f};
    // World space; direction is unit length and points away from the shape.
    glm::vec3 position{0.0f};
    glm::vec3 direction{1.0f, 0.0f, 0.0f};
};

// The collider's handles on an entity at entityWorld, where the shape is
// placed as physics places it (colliderLocalTransform).
std::vector<ColliderHandle> colliderHandles(const ColliderComponent& collider, const glm::mat4& entityWorld);

enum class ColliderHandleDrag : uint8_t {
    // The opposite side moves too: the shape grows by the distance on both
    // sides and stays centred.
    Symmetric,
    // The opposite side stays put: the shape grows by half the distance and
    // its offset follows the dragged side (Ctrl while dragging).
    OneSided,
};

// start with handle dragged distance metres (world) along its direction, on
// an entity of the given world scale (absolute, per axis). Sizes stay at
// least 1 mm; a one-sided drag moves the offset by as much as the shape
// actually grew.
ColliderComponent dragColliderHandle(const ColliderComponent& start, const ColliderHandle& handle,
                                     const glm::vec3& worldScale, float distance, ColliderHandleDrag mode);

// How far along the line through origin in direction (unit) the point
// nearest the ray under mouse (view pixels, Y down) lies, in metres; nullopt
// when the line points along the ray, so it cannot be dragged from there.
std::optional<float> colliderHandleAxisParameter(const RenderView& lens, glm::vec2 viewSize, glm::vec2 mouse,
                                                 const glm::vec3& origin, const glm::vec3& direction);

// The mouse over one viewport for a frame, in view pixels.
struct ColliderHandleInput {
    glm::vec2 mouse{0.0f};
    // The mouse is over the viewport's image and nothing above it (chrome,
    // the transform gizmo) takes it.
    bool available{false};
    bool leftPressed{false};
    bool leftDown{false};
    // Cancels a drag: Escape or a right click.
    bool cancel{false};
    // One-sided dragging.
    bool ctrl{false};
};

// One viewport's collider handle interaction. Each frame it finds the active
// entity's collider handles, draws them, and drags one pressed with the left
// button; the whole drag is one undo step ("Edit Collider", captured at the
// press and committed at the release like the transform gizmo), and a
// cancelled drag restores the collider. In Play the edit is live and
// unrecorded, as every SceneEditing edit there.
class ColliderHandleTool {
  public:
    // Draws (into drawList, when not null, offset by viewPosition) and drags
    // the handles of context's active entity, seen through lens. Returns
    // whether a handle is hovered or being dragged, so the viewport does not
    // pick through it.
    bool update(EditorContext& context, const RenderView& lens, glm::vec2 viewSize, const ColliderHandleInput& input,
                ImDrawList* drawList, glm::vec2 viewPosition);
    // Ends a drag without recording it, restoring the collider.
    void cancel(EditorContext& context);
    bool dragging() const;
    // The handle under the mouse last frame, as an index into the active
    // entity's colliderHandles.
    std::optional<size_t> hovered() const;

    // Handles are hit within this many view pixels.
    static constexpr float hitRadius = 8.0f;

  private:
    struct Drag {
        SceneEntityId entity;
        ColliderHandle handle;
        ColliderComponent start;
        glm::vec3 scale{1.0f};
        float startParameter{0.0f};
        SceneEditing::EntitySnapshot before;
        bool changed{false};
    };

    void updateDrag(EditorContext& context, const RenderView& lens, glm::vec2 viewSize,
                    const ColliderHandleInput& input);

    std::optional<Drag> m_drag;
    std::optional<size_t> m_hovered;
};
