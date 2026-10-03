#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Immediate-mode debug geometry: lines and wire shapes plus world-anchored
// text labels, recorded on the CPU for one frame. This header is plain data
// with no graphics API types, so any subsystem (physics, gameplay, tools) can
// record into a DebugDraw it is handed without depending on the renderer.
//
// Units and conventions: positions are world-space meters, angles radians,
// right-handed with +Y up. Colours are display-referred (sRGB-encoded) RGBA in
// [0, 1], like UI colours: the renderer draws lines over the tone-mapped image,
// so a colour appears exactly as given whatever the exposure or grade.

// Whether scene surfaces hide a debug primitive.
enum class DebugDepth : uint8_t {
    // Hidden where the scene's depth is closer than the line.
    Tested,
    // Drawn over every surface.
    AlwaysOnTop,
};

struct DebugStyle {
    glm::vec4 color{1.0f};
    DebugDepth depth{DebugDepth::Tested};
};

// One end of a line segment. Colour is RGBA8 with red in the lowest byte, the
// memory order of an R8G8B8A8 vertex attribute.
struct DebugLineVertex {
    glm::vec3 position{0.0f};
    uint32_t color{0};
};

static_assert(sizeof(DebugLineVertex) == 16, "DebugLineVertex is uploaded as-is");

// A text label anchored at a world position. The engine projects labels into
// each view (projectDebugLabels); drawing the text is up to the UI layer.
struct DebugLabel {
    glm::vec3 position{0.0f};
    std::string text;
    glm::vec4 color{1.0f};
};

// A label projected into one view: position in pixels from the view's top-left
// corner and distance along the camera's forward axis in meters.
struct ProjectedDebugLabel {
    glm::vec2 position{0.0f};
    float distance{0.0f};
    std::string text;
    glm::vec4 color{1.0f};
};

// A frame's debug primitives. Shapes are tessellated into line segments when
// recorded; segment counts are fixed per call so recordings are deterministic.
// Not thread-safe: record from one thread at a time.
class DebugDraw {
  public:
    // Line segments used for a full circle when a call does not choose.
    static constexpr uint32_t defaultSegments = 32;

    void line(const glm::vec3& from, const glm::vec3& to, const DebugStyle& style = {});
    // A line with a four-line arrowhead at `to`. A zero head length uses a
    // fifth of the arrow's length.
    void arrow(const glm::vec3& from, const glm::vec3& to, const DebugStyle& style = {}, float headLength = 0.0f);
    // An axis-aligned box from its minimum and maximum corners.
    void aabb(const glm::vec3& minimum, const glm::vec3& maximum, const DebugStyle& style = {});
    // An oriented box: centre, half extents along its local axes, rotation.
    void box(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation,
             const DebugStyle& style = {});
    // A box of `halfExtents` around the origin of `transform`'s local space
    // (which may scale or shear it).
    void box(const glm::mat4& transform, const glm::vec3& halfExtents, const DebugStyle& style = {});
    // Three great circles in the planes of the world axes.
    void sphere(const glm::vec3& center, float radius, const DebugStyle& style = {},
                uint32_t segments = defaultSegments);
    // A circle in the plane through `center` perpendicular to `normal`.
    void circle(const glm::vec3& center, const glm::vec3& normal, float radius, const DebugStyle& style = {},
                uint32_t segments = defaultSegments);
    // The eight corners and twelve edges of a view volume, from the camera's
    // view-projection matrix (clip depth 0..1, either depth direction).
    void frustum(const glm::mat4& viewProjection, const DebugStyle& style = {});
    // A cone from `apex` along `direction` (normalized here) for `length`
    // meters with the given half-angle: the base circle and four slant edges.
    void cone(const glm::vec3& apex, const glm::vec3& direction, float length, float halfAngleRadians,
              const DebugStyle& style = {}, uint32_t segments = defaultSegments);
    // A capsule whose hemisphere centres are `from` and `to`: end circles,
    // four side lines, and two half-circle arcs over each cap.
    void capsule(const glm::vec3& from, const glm::vec3& to, float radius, const DebugStyle& style = {},
                 uint32_t segments = defaultSegments);
    void label(const glm::vec3& position, std::string text, const glm::vec4& color = glm::vec4(1.0f));

    // Discards every primitive and keeps the storage for the next frame.
    void clear();
    bool empty() const;

    // Line segments as consecutive vertex pairs.
    std::span<const DebugLineVertex> lineVertices(DebugDepth depth) const {
        return m_lines[static_cast<size_t>(depth)];
    }

    size_t lineCount() const {
        return (m_lines[0].size() + m_lines[1].size()) / 2;
    }

    std::span<const DebugLabel> labels() const {
        return m_labels;
    }

  private:
    void addLine(const glm::vec3& from, const glm::vec3& to, uint32_t color, DebugDepth depth);
    void addArc(const glm::vec3& center, const glm::vec3& axisX, const glm::vec3& axisY, float radius,
                float beginRadians, float endRadians, uint32_t segments, uint32_t color, DebugDepth depth);

    std::array<std::vector<DebugLineVertex>, 2> m_lines;
    std::vector<DebugLabel> m_labels;
};

// Packs a display-referred colour into DebugLineVertex::color.
uint32_t packDebugColor(const glm::vec4& color);

// World-space corners of the view volume of `inverseViewProjection`: the four
// corners at clip depth 0, then the four at clip depth 1, each counter-
// clockwise in NDC from (-1, -1).
std::array<glm::vec3, 8> frustumCorners(const glm::mat4& inverseViewProjection);

// Pixel position (from the top-left corner of a viewport of `viewportSize`
// pixels, Vulkan clip conventions) and forward distance of a world point.
// Empty when the point is behind the camera or outside the view's sides.
struct ProjectedDebugPoint {
    glm::vec2 position{0.0f};
    float distance{0.0f};
};

std::optional<ProjectedDebugPoint> projectDebugPoint(const glm::mat4& viewProjection, const glm::vec2& viewportSize,
                                                     const glm::vec3& world);

// Appends the labels projectDebugPoint keeps to `output`, in recording order.
void projectDebugLabels(std::span<const DebugLabel> labels, const glm::mat4& viewProjection,
                        const glm::vec2& viewportSize, std::vector<ProjectedDebugLabel>& output);
