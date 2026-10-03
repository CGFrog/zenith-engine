// CPU contract of GPU picking (engine/graphics/RenderPicking.hpp): rectangle
// clamping, the pick-ticket lifecycle through RenderRuntime and the request
// book (the renderer side is driven by hand, standing in for the readback),
// and depth-to-world reconstruction.

#include "engine/graphics/Rendering.hpp"

#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

bool near(const glm::vec3& first, const glm::vec3& second, float tolerance) {
    return glm::length(first - second) <= tolerance;
}

void testClamping() {
    expect(clampPickRect({.x = 10, .y = 20, .width = 5, .height = 6}, 100, 100) ==
               PickRect{.x = 10, .y = 20, .width = 5, .height = 6},
           "a rectangle inside the view is kept");
    expect(clampPickRect({.x = -3, .y = 95, .width = 10, .height = 10}, 100, 100) ==
               PickRect{.x = 0, .y = 95, .width = 7, .height = 5},
           "a rectangle past the view's edges is clamped");
    expect(!clampPickRect({.x = 100, .y = 0, .width = 4, .height = 4}, 100, 100),
           "a rectangle beside the view is empty");
    expect(!clampPickRect({.x = 0, .y = 0, .width = 0, .height = 4}, 100, 100), "an empty rectangle stays empty");
}

void testTicketLifecycle() {
    RenderRuntime rendering;
    const RenderViewId view = rendering.createView();
    const PickTicket ticket = rendering.requestPick(view, {.x = 4, .y = 4, .width = 3, .height = 3});
    expect(static_cast<bool>(ticket), "a pick on a live view is accepted");
    expect(rendering.pickStatus(ticket) == PickStatus::Pending, "a new pick is pending");
    expect(rendering.pickResult(ticket).status == PickStatus::Pending, "a pending result carries only its status");
    expect(rendering.pickStatus(ticket) == PickStatus::Pending, "reading a pending result keeps the ticket");
    expect(!rendering.requestPick(view, {.x = 0, .y = 0, .width = 0, .height = 1}), "an empty rectangle is refused");
    expect(!rendering.requestPick(RenderViewId{.index = 7, .generation = 3}, {}), "a stale view is refused");

    // Destroying the view resolves its picks to Cancelled.
    expect(rendering.destroyView(view), "the view is destroyed");
    expect(rendering.pickStatus(ticket) == PickStatus::Cancelled, "destroying a view cancels its picks");
    expect(rendering.pickResult(ticket).status == PickStatus::Cancelled, "the cancelled result is handed over");
    expect(rendering.pickStatus(ticket) == PickStatus::Unknown, "a taken ticket is forgotten");

    // The caller cancels a pick.
    const PickTicket abandoned = rendering.requestPick(rendering.defaultView(), {});
    expect(rendering.cancelPick(abandoned), "a pending pick can be cancelled");
    expect(!rendering.cancelPick(abandoned), "a finished pick cannot be cancelled again");
    expect(rendering.pickStatus(abandoned) == PickStatus::Cancelled, "a cancelled pick reports Cancelled");
    expect(!rendering.cancelPick(PickTicket{}), "an empty ticket cannot be cancelled");
}

void testRendererSide() {
    const RenderViewId view{.index = 2, .generation = 5};
    const RenderViewId other{.index = 3, .generation = 1};
    PickRequestBook book;
    const PickTicket first = book.request(view, {.x = 1, .y = 2, .width = 3, .height = 4});
    const PickTicket second = book.request(view, {.x = 0, .y = 0, .width = 1, .height = 1});
    const PickTicket elsewhere = book.request(other, {});
    expect(book.startRecording(other).size() == 1, "only the rendered view's picks start recording");
    const std::vector<PickRequestBook::Recording> recording = book.startRecording(view);
    expect(recording.size() == 2 && recording[0].ticket == first && recording[0].rect.width == 3,
           "recording hands out every queued pick of the view in request order");
    expect(book.startRecording(view).empty(), "a pick is recorded once");
    expect(book.status(first) == PickStatus::Pending && book.awaitingResult(first),
           "a recorded pick waits for its frame");

    // The frame completed: the stand-in readback delivers ids.
    book.complete(first, {.region = {.x = 1, .y = 2, .width = 3, .height = 4}, .ids = {0, 1}});
    expect(book.status(first) == PickStatus::Ready && !book.awaitingResult(first), "a completed pick is ready");
    const PickResult result = book.take(first);
    expect(result.status == PickStatus::Ready && result.view == view && result.ids.size() == 2,
           "a ready result carries its view and data");

    // A view resized while the pick was in flight drops the late result.
    book.cancelView(view);
    expect(book.status(second) == PickStatus::Cancelled, "a resized view cancels picks in flight");
    book.complete(second, {.ids = {1}});
    expect(book.take(second).ids.empty(), "a result arriving after cancellation is ignored");

    book.fail(elsewhere, "readback failed");
    const PickResult failed = book.take(elsewhere);
    expect(failed.status == PickStatus::Failed && failed.error == "readback failed", "a failed pick keeps its error");
    expect(book.outstanding() == 0, "taken tickets leave the book");
}

void testCapacity() {
    const RenderViewId view{.index = 0, .generation = 1};
    PickRequestBook book;
    std::vector<PickTicket> tickets;
    for (size_t index = 0; index < PickRequestBook::maximumOutstanding; ++index) {
        tickets.push_back(book.request(view, {}));
    }
    expect(!book.request(view, {}), "a book full of unfinished picks refuses more");
    expect(book.cancel(tickets.front()), "a pick is cancelled");
    const PickTicket replacement = book.request(view, {});
    expect(static_cast<bool>(replacement) && book.status(tickets.front()) == PickStatus::Unknown,
           "a full book evicts the oldest finished result nobody took");
}

void testDepthReconstruction() {
    const glm::uvec2 size{200, 100};
    // The renderer's convention: reversed Z (far and near swapped), Y flipped.
    glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(60.0f), 2.0f, 500.0f, 0.1f);
    projection[1][1] *= -1.0f;
    const glm::mat4 view = glm::lookAtRH(glm::vec3(1.0f, 2.0f, 8.0f), glm::vec3(1.0f, 2.0f, 0.0f), {0, 1, 0});
    const glm::mat4 viewProjection = projection * view;
    const glm::mat4 inverseViewProjection = glm::inverse(viewProjection);

    const glm::vec3 point{1.6f, 1.5f, 0.5f};
    const glm::vec4 clip = viewProjection * glm::vec4(point, 1.0f);
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    const glm::vec2 pixel = (glm::vec2(ndc) * 0.5f + 0.5f) * glm::vec2(size);
    expect(pixel.x > 100.0f && pixel.y > 50.0f, "a point right of and below the camera lands right and low");
    expect(near(reconstructWorldPosition(pixel, ndc.z, size, inverseViewProjection), point, 1e-3f),
           "depth and pixel reconstruct the world position");

    // pickWorldPosition reads the result at a view pixel.
    const glm::uvec2 viewPixel{static_cast<uint32_t>(pixel.x), static_cast<uint32_t>(pixel.y)};
    PickResult result{.status = PickStatus::Ready,
                      .region = {.x = static_cast<int32_t>(viewPixel.x) - 1,
                                 .y = static_cast<int32_t>(viewPixel.y),
                                 .width = 2,
                                 .height = 1},
                      .viewWidth = size.x,
                      .viewHeight = size.y,
                      .ids = {0, 1},
                      .depth = {0.0f, ndc.z},
                      .viewProjection = viewProjection,
                      .inverseViewProjection = inverseViewProjection};
    const std::optional<glm::vec3> surface = pickWorldPosition(result, viewPixel.x, viewPixel.y);
    // The pixel centre is within half a pixel of the projected point.
    expect(surface && near(*surface, point, 0.05f), "a picked pixel's surface lies at the projected point");
    expect(!pickWorldPosition(result, viewPixel.x - 1, viewPixel.y), "an empty pixel has no surface");
    expect(!pickWorldPosition(result, viewPixel.x + 1, viewPixel.y), "pixels outside the region have no surface");
}
} // namespace

int main() {
    testClamping();
    testTicketLifecycle();
    testRendererSide();
    testCapacity();
    testDepthReconstruction();
    if (failures == 0) {
        std::cout << "Render picking tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
