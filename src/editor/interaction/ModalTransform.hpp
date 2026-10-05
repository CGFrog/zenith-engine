#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Blender-style modal transforms (G, R, S): the mouse drives the operation
// until it is confirmed or cancelled. This class only computes transforms; the
// caller owns input routing, scene updates, and undo.

enum class TransformOperation : uint8_t {
    Translate,
    Rotate,
    // R pressed twice: free rotation about the view's right and up axes.
    Trackball,
    Scale,
};

enum class TransformAxis : uint8_t {
    X,
    Y,
    Z,
};

enum class TransformConstraint : uint8_t {
    None,
    // Restricts the operation to one axis.
    Axis,
    // Shift+axis: the plane perpendicular to the axis.
    Plane,
};

enum class ConstraintSpace : uint8_t {
    Global,
    // The active object's own axes.
    Local,
};

// Camera and viewport rectangle the transform was started in. projection maps
// to Vulkan clip space with Y pointing down, matching screen coordinates.
struct ModalTransformView {
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    glm::vec2 viewportPosition{0.0f};
    glm::vec2 viewportSize{1.0f};
};

// What rotation and scaling turn about (Blender's Transform Pivot Point).
enum class TransformPivot : uint8_t {
    // The mean of the transformed origins.
    MedianPoint,
    // The active object's origin.
    ActiveElement,
    // Each object about its own origin; the median point is the mouse's
    // reference on screen. Translation is unaffected.
    IndividualOrigins,
    // The scene's 3D cursor.
    Cursor,
};

// What snapping moves toward (Blender's Snap To).
enum class SnapTarget : uint8_t {
    // Whole increments of the snap values (TransformSnapping).
    Increment,
    // The nearest surface under the mouse.
    Surface,
    // The mesh vertex nearest the mouse on screen.
    Vertex,
};

// How a transform pivots: the mode and the points it may need.
struct ModalTransformPivot {
    TransformPivot mode{TransformPivot::MedianPoint};
    // The active object's world origin, for ActiveElement; without it the
    // median point is used.
    std::optional<glm::vec3> activeOrigin;
    // The 3D cursor's world position, for Cursor.
    glm::vec3 cursor{0.0f};
};

// The point mode pivots about for objects at the origins of `world`: the
// median of the origins (also for IndividualOrigins, whose per-object pivots
// the transform applies itself), the active origin, or the cursor. The
// origin for no objects.
glm::vec3 transformPivotPoint(const ModalTransformPivot& pivot, std::span<const glm::mat4> world);

// The unit-length columns of world's upper 3x3 (its axes without scale); a
// degenerate column stays the matching world axis.
glm::mat3 normalizedAxes(const glm::mat4& world);

// One change applied to several objects at once, as the modal transforms
// and the viewport gizmo do: scaling by `scale` along the columns of the
// caller's `axes`, then `rotation` (world space), both about the pivot, then
// `translation` (world space, in metres).
struct PivotedTransform {
    glm::vec3 translation{0.0f};
    glm::mat3 rotation{1.0f};
    glm::vec3 scale{1.0f};
};

// change applied to each of startWorld (world matrices): about pivot, or
// about each object's own origin for IndividualOrigins. axes (orthonormal
// columns) is the frame scale works in. Each object is scaled along its own
// axes by how much the scaling stretches them, so rotated objects never
// shear. Returns the new world matrices in startWorld's order.
std::vector<glm::mat4> applyPivotedTransform(std::span<const glm::mat4> startWorld, TransformPivot pivotMode,
                                             const glm::vec3& pivot, const glm::mat3& axes,
                                             const PivotedTransform& change);

// Splits a gizmo's change, gizmoNow * inverse(gizmoStart), into a
// PivotedTransform whose scale is along gizmoStart's axes (pass
// normalizedAxes(gizmoStart) as applyPivotedTransform's axes): the
// translation of its origin, then the rotation and per-axis scale of its
// frame (a rotation times a scale in that frame, as ImGuizmo produces).
// gizmoStart must be invertible; nullopt when either matrix is degenerate.
std::optional<PivotedTransform> decomposeGizmoChange(const glm::mat4& gizmoStart, const glm::mat4& gizmoNow);

// Snapping for one evaluation. Ctrl inverts `enabled` while held.
//
// Translation snaps the selection's reference point (pivot-point snapping:
// the median, or the active origin with ActiveElement; ModalTransform::pivot
// while translating): Increment moves it in whole `translation` steps along the constraint frame's free axes, or
// with absoluteGrid onto multiples of `translation` in that frame (the world
// grid for global constraints); Surface and Vertex move it onto targetPoint,
// projected onto the axis or plane constraint, and move freely while
// targetPoint is empty (nothing under the mouse). Rotation and scale always
// snap their value in increments.
struct TransformSnapping {
    bool enabled{false};
    float translation{1.0f};
    float rotationDegrees{5.0f};
    float scale{0.1f};
    SnapTarget target{SnapTarget::Increment};
    bool absoluteGrid{false};
    // Surface and Vertex: the world point under the mouse the caller found
    // (editor/viewport/SnapQueries).
    std::optional<glm::vec3> targetPoint;
};

struct ModalTransformResult {
    // World matrices in the same order as the start matrices.
    std::vector<glm::mat4> world;
    // Header text in Blender's style, for example "D: 1.0000 m along global X".
    std::string description;
    // The surface or vertex point the pivot snapped to, for the overlay.
    std::optional<glm::vec3> snappedTo;
};

struct ConstraintGuide {
    TransformAxis axis{};
    glm::vec3 direction{0.0f};
};

class ModalTransform {
  public:
    // startWorld holds the world matrices of the transformed objects;
    // localAxes are the active object's axes used for local constraints.
    ModalTransform(TransformOperation operation, std::vector<glm::mat4> startWorld, glm::mat3 localAxes,
                   glm::vec2 mouse, ModalTransformView view, ModalTransformPivot pivot = {});

    TransformOperation operation() const;
    // Switches operation (G/R/S during a transform). Pressing R while rotating
    // toggles the trackball. The new operation starts from the current mouse.
    void setOperation(TransformOperation operation);
    // X/Y/Z cycle global, local, then no constraint; Shift selects the plane.
    void pressAxis(TransformAxis axis, bool plane);
    void moveMouse(glm::vec2 position, bool precise);
    // Digits, '.', and '-' (negate). Returns false for other characters.
    bool typeCharacter(char character);
    void eraseCharacter();
    bool numericInputActive() const;

    ModalTransformResult evaluate(const TransformSnapping& snapping) const;
    // The point the current operation works about: for rotation and scale
    // the pivot point (transformPivotPoint; for IndividualOrigins the median,
    // which the mouse turns around on screen); for translation, which the
    // pivot mode does not change, the median or active origin, which is also
    // the point translation snapping moves onto its target.
    glm::vec3 pivot() const;
    TransformPivot pivotMode() const;
    glm::vec2 pivotOnScreen() const;
    glm::vec2 toScreen(const glm::vec3& point) const;
    glm::vec2 mouse() const;
    std::vector<ConstraintGuide> constraintGuides() const;

  private:
    glm::mat3 constraintFrame() const;
    glm::vec3 axisDirection(TransformAxis axis) const;
    float numericValue() const;
    void resetMouseOrigin();
    // Restricts a world offset to the active axis or plane constraint.
    glm::vec3 constrain(const glm::vec3& offset) const;
    glm::vec3 snapTranslation(glm::vec3 delta, const TransformSnapping& snapping) const;
    // change applied to the start matrices about the pivot (applyPivotedTransform).
    std::vector<glm::mat4> aboutPivot(const PivotedTransform& change, const glm::mat3& axes) const;

    ModalTransformResult translate(const TransformSnapping& snapping) const;
    ModalTransformResult rotate(const TransformSnapping& snapping) const;
    ModalTransformResult trackball(const TransformSnapping& snapping) const;
    ModalTransformResult scale(const TransformSnapping& snapping) const;

    TransformOperation m_operation;
    std::vector<glm::mat4> m_startWorld;
    glm::mat3 m_localAxes;
    ModalTransformView m_view;
    glm::vec3 m_pivot{0.0f};
    TransformPivot m_pivotMode{TransformPivot::MedianPoint};
    glm::vec3 m_translationOrigin{0.0f};
    TransformConstraint m_constraint{TransformConstraint::None};
    TransformAxis m_axis{TransformAxis::X};
    ConstraintSpace m_space{ConstraintSpace::Global};
    // The mouse as seen by the operation: Shift slows it for precision.
    glm::vec2 m_lastMouse{0.0f};
    glm::vec2 m_virtualMouse{0.0f};
    glm::vec2 m_originMouse{0.0f};
    // Rotation accumulated across frames so turns past 180 degrees continue.
    float m_rotationRadians{0.0f};
    std::string m_numericText;
    bool m_numericNegated{false};
};

enum class TransformPart : uint8_t {
    Location,
    Rotation,
    Scale,
};

// Alt+G, Alt+R, Alt+S: resets one part of a local transform, keeping the others.
glm::mat4 clearTransformPart(const glm::mat4& local, TransformPart part);
