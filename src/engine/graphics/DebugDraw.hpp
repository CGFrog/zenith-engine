#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Immediate-mode debug geometry: lines and wire shapes plus world-anchored
// text labels, recorded on the CPU for one frame, for a duration, or until
// cleared, optionally in named channels. This header is plain data
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

// Debug primitives of one lifetime and channel: line segments as consecutive
// vertex pairs per depth mode, and labels.
struct DebugPrimitives {
    std::array<std::vector<DebugLineVertex>, 2> lines;
    std::vector<DebugLabel> labels;

    bool empty() const;
    // Keeps the storage.
    void clear();
    void append(const DebugPrimitives& other);
};

// How long recorded primitives last.
enum class DebugLifetime : uint8_t {
    // Until the frame they were recorded for has rendered (DebugDraw::endFrame).
    Frame,
    // For a duration in seconds (DebugDrawRecorder::forSeconds), and in at
    // least one frame.
    Timed,
    // Until their channel or the whole DebugDraw is cleared.
    Persistent,
};

// A named channel as the editor lists it (DebugDraw::channels).
struct DebugDrawChannelInfo {
    std::string name;
    bool visible{true};
    // Whether the channel holds primitives now or held one-frame primitives
    // in the frame that last ended, so a channel redrawn every frame stays
    // listed between frames.
    bool active{false};
};

class DebugDraw;

// Records shapes into one channel of a DebugDraw with one lifetime. A small
// value referring to its DebugDraw, which must outlive it and stay where it
// is: get one where it is used (debugDraw.channel("gen.rooms").persistent()).
// One-frame shapes recorded into a hidden channel are dropped; timed and
// persistent ones are kept and show once the channel is visible again.
class DebugDrawRecorder {
  public:
    // The same channel, with primitives that last `seconds` of the time the
    // DebugDraw is aged by (DebugDraw::endFrame) and are drawn in at least
    // one frame. A duration that is not positive lasts one frame.
    DebugDrawRecorder forSeconds(float seconds) const;
    // The same channel, with primitives that last until it is cleared.
    DebugDrawRecorder persistent() const;

    // The shapes of the same names on DebugDraw.
    void line(const glm::vec3& from, const glm::vec3& to, const DebugStyle& style = {}) const;
    void arrow(const glm::vec3& from, const glm::vec3& to, const DebugStyle& style = {}, float headLength = 0.0f) const;
    void aabb(const glm::vec3& minimum, const glm::vec3& maximum, const DebugStyle& style = {}) const;
    void box(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation,
             const DebugStyle& style = {}) const;
    void box(const glm::mat4& transform, const glm::vec3& halfExtents, const DebugStyle& style = {}) const;
    void sphere(const glm::vec3& center, float radius, const DebugStyle& style = {}, uint32_t segments = 32) const;
    void circle(const glm::vec3& center, const glm::vec3& normal, float radius, const DebugStyle& style = {},
                uint32_t segments = 32) const;
    void frustum(const glm::mat4& viewProjection, const DebugStyle& style = {}) const;
    void cone(const glm::vec3& apex, const glm::vec3& direction, float length, float halfAngleRadians,
              const DebugStyle& style = {}, uint32_t segments = 32) const;
    void capsule(const glm::vec3& from, const glm::vec3& to, float radius, const DebugStyle& style = {},
                 uint32_t segments = 32) const;
    void label(const glm::vec3& position, std::string text, const glm::vec4& color = glm::vec4(1.0f)) const;

  private:
    friend class DebugDraw;

    DebugDrawRecorder(DebugDraw& owner, uint32_t channel, DebugLifetime lifetime, float seconds)
        : m_owner(&owner)
        , m_channel(channel)
        , m_lifetime(lifetime)
        , m_seconds(seconds) {}

    // Where the next shape goes; null when it is dropped.
    DebugPrimitives* target() const;

    DebugDraw* m_owner;
    uint32_t m_channel;
    DebugLifetime m_lifetime;
    float m_seconds;
};

// Debug primitives for the renderer to draw. Shapes are tessellated into line
// segments when recorded; segment counts are fixed per call so recordings are
// deterministic. Not thread-safe: record from one thread at a time.
//
// Lifetimes: DebugDraw's own shape functions record one-frame primitives into
// the default (unnamed) channel, which last until the frame renders. A
// recorder from channel(), forSeconds(), or persistent() records into a named
// channel, for a duration, or until cleared. The renderer ends every frame
// with endFrame and the wall-clock seconds since the previous frame
// (RendererOptions::fixedFrameSeconds when set), so durations are real time:
// they also elapse while the game is paused and in the editor's Edit mode.
//
// Channels: named groups ("gen.rooms") that can be hidden and cleared as a
// whole; the editor's viewport Overlays menu lists them as toggles. Hiding a
// channel hides its primitives in every view without discarding them.
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

    // A recorder of one-frame primitives in the channel called name, created
    // on first use; the empty name is the default channel.
    DebugDrawRecorder channel(std::string_view name);
    // Recorders into the default channel with a duration, or until cleared.
    DebugDrawRecorder forSeconds(float seconds);
    DebugDrawRecorder persistent();

    // Shows or hides a channel (created if needed) in every view.
    void setChannelVisible(std::string_view name, bool visible);
    // True for a channel never named.
    bool channelVisible(std::string_view name) const;
    // Discards every primitive of the channel, whatever its lifetime; the
    // channel stays listed with its visibility.
    void clearChannel(std::string_view name);
    // The named channels (not the default one), sorted by name.
    std::vector<DebugDrawChannelInfo> channels() const;

    // Ends a rendered frame: discards one-frame primitives and ages timed
    // ones by elapsedSeconds, discarding those whose time is up. The
    // renderer calls it after every frame, drawn or skipped.
    void endFrame(float elapsedSeconds = 0.0f);
    // Discards every primitive of every lifetime and channel and keeps the
    // storage; channels keep their visibility.
    void clear();
    // Whether no primitive of any lifetime is held, hidden or not.
    bool empty() const;

    // The primitives to draw now: every lifetime of every visible channel.
    // Line segments as consecutive vertex pairs. Valid until the next change.
    std::span<const DebugLineVertex> lineVertices(DebugDepth depth) const {
        return composed().lines[static_cast<size_t>(depth)];
    }

    size_t lineCount() const {
        const DebugPrimitives& visible = composed();
        return (visible.lines[0].size() + visible.lines[1].size()) / 2;
    }

    std::span<const DebugLabel> labels() const {
        return composed().labels;
    }

  private:
    friend class DebugDrawRecorder;

    struct Channel {
        std::string name;
        bool visible{true};
        // Had one-frame primitives when the last frame ended.
        bool drewLastFrame{false};
        DebugPrimitives frame;
        DebugPrimitives persistent;
    };

    struct TimedBatch {
        uint32_t channel{0};
        float remainingSeconds{0.0f};
        // Recorded since the last endFrame, so a frame's timed shapes of one
        // channel and duration share a batch.
        bool fresh{true};
        DebugPrimitives primitives;
    };

    DebugDrawRecorder recorder() {
        return {*this, 0, DebugLifetime::Frame, 0.0f};
    }

    uint32_t channelIndex(std::string_view name);
    const Channel* findChannel(std::string_view name) const;
    DebugPrimitives* target(uint32_t channel, DebugLifetime lifetime, float seconds);
    const DebugPrimitives& composed() const;

    // The default channel is always first.
    std::vector<Channel> m_channels{Channel{}};
    std::vector<TimedBatch> m_timed;
    // The visible primitives, rebuilt on demand after a change.
    mutable DebugPrimitives m_composed;
    mutable bool m_composedDirty{false};
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
