#pragma once

#include "RenderIds.hpp"
#include "engine/scene/SceneEntityId.hpp"

#include <cstdint>
#include <glm/glm.hpp>
#include <optional>
#include <string>
#include <vector>

// GPU picking: a caller asks for the object ids under a rectangle of a render
// view (RenderRuntime::requestPick) and receives them a few frames later. The
// renderer draws the view's frustum-visible objects into an id buffer only on
// frames with a pending request, copies the rectangle to host memory, and
// publishes the result once that frame's fence signalled; it never waits for
// the device.

// A rectangle of a view's output pixels (the size requestViewSize set, which
// the view's texture has), origin at the top-left. May extend past the view;
// the renderer reads the overlap.
struct PickRect {
    int32_t x{0};
    int32_t y{0};
    uint32_t width{1};
    uint32_t height{1};

    bool operator==(const PickRect&) const = default;
};

// The part of `rect` inside a view of the given size; empty when they do not
// overlap or either is empty.
std::optional<PickRect> clampPickRect(const PickRect& rect, uint32_t viewWidth, uint32_t viewHeight);

// Handle of one pick request. Zero is never issued.
struct PickTicket {
    uint64_t value{0};

    explicit operator bool() const {
        return value != 0;
    }

    bool operator==(const PickTicket&) const = default;
};

enum class PickStatus : uint8_t {
    // Never issued, already taken, or evicted.
    Unknown,
    // Waiting for its view to render, or for that frame to complete.
    Pending,
    Ready,
    // Cancelled by the caller, or its view was destroyed or resized first.
    Cancelled,
    // The renderer could not record or read it; PickResult::error says why.
    Failed,
};

// One entry of a result's id table: id n in PickResult::ids names objects[n-1].
struct PickedObject {
    // The entity's stable id; empty for an entity without one.
    SceneEntityId entity;
    // The entity's integral value, as RenderRuntime::setViewOutlinedObjects takes it.
    RenderObjectId object{0};

    bool operator==(const PickedObject&) const = default;
};

// What a finished pick saw. Every vector covers `region` row by row from its
// top-left pixel. Objects are whatever the view drew that frame: frustum-
// visible mesh renderers, skinned meshes in their animated pose; alpha-masked
// cut-outs pick as solid surfaces and particles are not pickable.
struct PickResult {
    PickStatus status{PickStatus::Unknown};
    std::string error;
    RenderViewId view;
    // The requested rectangle clamped to the view.
    PickRect region;
    uint32_t viewWidth{0};
    uint32_t viewHeight{0};
    // 0 where no object covers the pixel; n > 0 is objects[n-1].
    std::vector<uint32_t> ids;
    // Reversed-Z device depth (DepthConvention.hpp) of the front-most surface:
    // 1 at the near plane, 0 where ids is 0.
    std::vector<float> depth;
    // One entry per distinct entity drawn, in no particular order.
    std::vector<PickedObject> objects;
    // The camera the id buffer was drawn with: unjittered, at the view's
    // output aspect ratio, reversed Z, Vulkan clip space (Y down).
    glm::mat4 viewProjection{1.0f};
    glm::mat4 inverseViewProjection{1.0f};
    // RenderStats::submittedFrame of the frame that drew the id buffer.
    uint64_t submittedFrame{0};
};

// The world position of a point of a view at reversed-Z device depth `depth`.
// `viewPixel` is continuous: the centre of pixel (x, y) is (x + 0.5, y + 0.5).
glm::vec3 reconstructWorldPosition(glm::vec2 viewPixel, float depth, glm::uvec2 viewSize,
                                   const glm::mat4& inverseViewProjection);

// The world position of the surface under view pixel (x, y), when the result
// covers that pixel and an object was drawn there.
std::optional<glm::vec3> pickWorldPosition(const PickResult& result, uint32_t viewX, uint32_t viewY);

// Pick requests and finished results, owned by RenderRuntime; the Renderer
// moves requests through it. Lifecycle of a ticket:
//   request -> queued (Pending) -> startRecording (still Pending) ->
//   complete (Ready) | fail (Failed); cancel or cancelView before completion
//   resolves it to Cancelled. take() hands a finished result to the caller and
//   forgets the ticket.
class PickRequestBook {
  public:
    // Tickets alive at once. When full, a request evicts the oldest finished
    // result nobody took; with every ticket unfinished it is refused.
    static constexpr size_t maximumOutstanding = 64;

    struct Recording {
        PickTicket ticket;
        PickRect rect;
    };

    // Empty when `view` is empty, the rectangle is empty, or the book is full
    // of unfinished requests.
    PickTicket request(RenderViewId view, const PickRect& rect);
    PickStatus status(PickTicket ticket) const;
    // A finished ticket's result, after which the ticket is Unknown. A
    // pending or unknown ticket returns a result holding only its status.
    PickResult take(PickTicket ticket);
    // Resolves an unfinished ticket to Cancelled. False for a finished or
    // unknown ticket.
    bool cancel(PickTicket ticket);
    // Cancels every unfinished ticket of `view`, for a destroyed or resized view.
    void cancelView(RenderViewId view);

    // Renderer side. Queued tickets of `view` now being recorded.
    std::vector<Recording> startRecording(RenderViewId view);
    // Whether a recorded ticket still waits for its result.
    bool awaitingResult(PickTicket ticket) const;
    // Publishes a recorded ticket's result; ignored once the ticket was
    // cancelled or forgotten.
    void complete(PickTicket ticket, PickResult result);
    void fail(PickTicket ticket, std::string error);

    size_t outstanding() const {
        return m_entries.size();
    }

  private:
    enum class State : uint8_t {
        Queued,
        Recording,
        Finished,
    };

    struct Entry {
        PickTicket ticket;
        RenderViewId view;
        PickRect rect;
        State state{State::Queued};
        PickResult result;
    };

    Entry* find(PickTicket ticket);
    const Entry* find(PickTicket ticket) const;
    void finish(Entry& entry, PickStatus status, std::string error);

    // In request order.
    std::vector<Entry> m_entries;
    uint64_t m_nextTicket{1};
};
