// Viewport picking: a whole-view GPU pick of the active viewport finds a
// selectable object; view3d.select at one of its pixels must select it
// through the id buffer; view3d.localview must isolate it (the render view
// and a second pick hold only the isolated objects); leaving local view must
// restore the full view. A scene with nothing pickable skips.

#include "editor/interaction/Selection.hpp"
#include "editor/panels/ViewportPanel.hpp"
#include "editor/panels/ViewportRegistry.hpp"
#include "editor/smoke/EditorSmokeHooks.hpp"
#include "editor/smoke/EditorSmokeSteps.hpp"
#include "engine/scene/SceneVisibility.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iostream>
#include <limits>
#include <optional>
#include <vector>

namespace {

// The selectable object nearest the centre of a finished whole-view pick.
struct PickTarget {
    SceneEntityId entity;
    RenderObjectId object{0};
    glm::vec2 viewPoint{0.0f};
};

std::optional<PickTarget> centralSelectableObject(const PickResult& result, const Scene& scene) {
    const SceneVisibility visibility(scene, SceneVisibilityMode::Editor);
    const glm::vec2 center(static_cast<float>(result.region.width) * 0.5f,
                           static_cast<float>(result.region.height) * 0.5f);
    std::optional<PickTarget> best;
    float bestDistance = std::numeric_limits<float>::max();
    for (uint32_t y = 0; y < result.region.height; ++y) {
        for (uint32_t x = 0; x < result.region.width; ++x) {
            const uint32_t id = result.ids[static_cast<size_t>(y) * result.region.width + x];
            if (id == 0 || id > result.objects.size()) {
                continue;
            }
            const PickedObject& object = result.objects[id - 1];
            const Entity entity = object.entity ? scene.findEntity(object.entity) : entt::null;
            const glm::vec2 point(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
            const float distance = glm::length(point - center);
            if (entity == entt::null || !visibility.selectable(entity) || distance >= bestDistance) {
                continue;
            }
            // Only pixels whose whole click neighbourhood shows the object, so
            // the click cannot land on a neighbour.
            bool interior = true;
            for (int32_t dy = -1; dy <= 1 && interior; ++dy) {
                for (int32_t dx = -1; dx <= 1 && interior; ++dx) {
                    const int64_t nx = static_cast<int64_t>(x) + dx;
                    const int64_t ny = static_cast<int64_t>(y) + dy;
                    interior =
                        nx >= 0 && ny >= 0 && nx < result.region.width && ny < result.region.height &&
                        result.ids[static_cast<size_t>(ny) * result.region.width + static_cast<size_t>(nx)] == id;
                }
            }
            if (interior) {
                best = PickTarget{.entity = object.entity,
                                  .object = object.object,
                                  .viewPoint = point + glm::vec2(static_cast<float>(result.region.x),
                                                                 static_cast<float>(result.region.y))};
                bestDistance = distance;
            }
        }
    }
    return best;
}

class PickingSmokeStep final : public SmokeStep {
  public:
    std::string_view name() const override {
        return "viewport picking";
    }

    SmokeStepResult update(SmokeStepContext& context, uint32_t) override {
        ViewportPanel* viewport = context.editor.viewports().active();
        if (viewport == nullptr || !viewport->renderView()) {
            return SmokeStepResult::failed("no viewport with a render view is open");
        }
        Frame frame{.context = context, .viewport = *viewport, .view = viewport->renderView()};
        switch (m_phase) {
        case Phase::Idle:
            return requestFirstPick(frame);
        case Phase::WholeView:
            return clickCentralObject(frame);
        case Phase::Click:
            return enterLocalView(frame);
        case Phase::LocalView:
            return requestLocalViewPick(frame);
        case Phase::LocalViewPick:
            return leaveLocalView(frame);
        case Phase::LeftLocalView:
            return checkFullView(frame);
        }
        return SmokeStepResult::failed("unknown phase");
    }

    uint32_t frameBudget() const override {
        // GPU picks resolve a few frames after they are requested.
        return 150;
    }

  private:
    enum class Phase : uint8_t {
        Idle,
        WholeView,
        Click,
        LocalView,
        LocalViewPick,
        LeftLocalView,
    };

    struct Frame {
        SmokeStepContext& context;
        ViewportPanel& viewport;
        RenderViewId view;
    };

    bool requestWholeView(Frame& frame) {
        const glm::vec2 size = frame.viewport.size();
        m_ticket = frame.context.app.rendering.requestPick(
            frame.view, {.width = static_cast<uint32_t>(size.x), .height = static_cast<uint32_t>(size.y)});
        return static_cast<bool>(m_ticket);
    }

    // A finished whole-view pick, or nullopt while it is pending (error stays
    // empty) or when it failed (error says why).
    std::optional<PickResult> takeWholeView(Frame& frame, std::string& error) {
        RenderRuntime& rendering = frame.context.app.rendering;
        const PickStatus status = rendering.pickStatus(m_ticket);
        if (status == PickStatus::Pending) {
            return std::nullopt;
        }
        PickResult result = rendering.pickResult(m_ticket);
        if (status != PickStatus::Ready) {
            error = "a whole-view pick ended without a result: " + result.error;
            return std::nullopt;
        }
        return result;
    }

    SmokeStepResult requestFirstPick(Frame& frame) {
        if (!frame.context.app.rendering.viewTexture(frame.view)) {
            return SmokeStepResult::failed("the active viewport has not rendered");
        }
        if (!requestWholeView(frame)) {
            return SmokeStepResult::failed("the renderer refused a whole-view pick");
        }
        m_phase = Phase::WholeView;
        return SmokeStepResult::running();
    }

    SmokeStepResult clickCentralObject(Frame& frame) {
        std::string error;
        const std::optional<PickResult> result = takeWholeView(frame, error);
        if (!result) {
            return error.empty() ? SmokeStepResult::running() : SmokeStepResult::failed(error);
        }
        const std::optional<PickTarget> target = centralSelectableObject(*result, frame.context.app.scene);
        if (!target) {
            std::cout << "Picking smoke skipped: nothing selectable is in view\n";
            return SmokeStepResult::done();
        }
        m_entity = target->entity;
        m_object = target->object;
        frame.context.editor.selection().clear();
        frame.context.editor.invokeOperator(
            "view3d.select", nlohmann::json{{"x", target->viewPoint.x}, {"y", target->viewPoint.y}}.dump(),
            KeymapRegion::Viewport);
        m_phase = Phase::Click;
        return SmokeStepResult::running();
    }

    SmokeStepResult enterLocalView(Frame& frame) {
        if (frame.viewport.pickPending()) {
            return SmokeStepResult::running();
        }
        if (!frame.viewport.lastPickOutcome() || !frame.viewport.lastPickOutcome()->fromGpu) {
            return SmokeStepResult::failed("the click was not answered by the GPU id buffer");
        }
        if (frame.context.editor.selection().ids() != std::vector<SceneEntityId>{m_entity}) {
            return SmokeStepResult::failed("the click did not select the object the id buffer shows at that pixel");
        }
        if (!frame.context.editor.invokeOperator("view3d.localview", {}, KeymapRegion::Viewport)) {
            return SmokeStepResult::failed("view3d.localview did not enter local view");
        }
        m_phase = Phase::LocalView;
        return SmokeStepResult::running();
    }

    SmokeStepResult requestLocalViewPick(Frame& frame) {
        const std::optional<std::vector<RenderObjectId>> isolated =
            frame.context.app.rendering.viewVisibleEntities(frame.view);
        if (!isolated || !std::ranges::binary_search(*isolated, m_object)) {
            return SmokeStepResult::failed("local view did not isolate the selection in the render view");
        }
        if (!requestWholeView(frame)) {
            return SmokeStepResult::failed("the renderer refused a local-view pick");
        }
        m_phase = Phase::LocalViewPick;
        return SmokeStepResult::running();
    }

    SmokeStepResult leaveLocalView(Frame& frame) {
        std::string error;
        const std::optional<PickResult> result = takeWholeView(frame, error);
        if (!result) {
            return error.empty() ? SmokeStepResult::running() : SmokeStepResult::failed(error);
        }
        const std::optional<std::vector<RenderObjectId>> isolated =
            frame.context.app.rendering.viewVisibleEntities(frame.view);
        const bool onlyIsolated = isolated && std::ranges::all_of(result->objects, [&](const PickedObject& object) {
                                      return std::ranges::binary_search(*isolated, object.object);
                                  });
        if (!onlyIsolated || result->objects.empty()) {
            return SmokeStepResult::failed("a pick in local view saw objects outside it");
        }
        if (!frame.context.editor.invokeOperator("view3d.localview", {}, KeymapRegion::Viewport)) {
            return SmokeStepResult::failed("view3d.localview did not leave local view");
        }
        m_phase = Phase::LeftLocalView;
        return SmokeStepResult::running();
    }

    SmokeStepResult checkFullView(Frame& frame) {
        if (frame.context.app.rendering.viewVisibleEntities(frame.view) || frame.viewport.localViewActive()) {
            return SmokeStepResult::failed("leaving local view did not restore the full view");
        }
        frame.context.editor.selection().clear();
        std::cout << "Picking smoke passed: a GPU click and local view in the active viewport\n";
        return SmokeStepResult::done();
    }

    Phase m_phase{Phase::Idle};
    // The whole-view pick the step waits for, and the object it clicked.
    PickTicket m_ticket;
    SceneEntityId m_entity;
    RenderObjectId m_object{0};
};

} // namespace

std::unique_ptr<SmokeStep> makePickingSmokeStep() {
    return std::make_unique<PickingSmokeStep>();
}
