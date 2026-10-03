// EditorSession's viewport picking smoke: a GPU click pick resolves to the
// object the id buffer shows, and local view isolates the active viewport's
// render view and its picks.

#include "EditorSession.hpp"

#include "engine/scene/SceneVisibility.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iostream>
#include <limits>

namespace {
// The selectable object nearest the centre of a finished whole-view pick.
struct SmokePickTarget {
    SceneEntityId entity;
    RenderObjectId object{0};
    glm::vec2 viewPoint{0.0f};
};

std::optional<SmokePickTarget> centralSelectableObject(const PickResult& result, const Scene& scene) {
    const SceneVisibility visibility(scene, SceneVisibilityMode::Editor);
    const glm::vec2 center(static_cast<float>(result.region.width) * 0.5f,
                           static_cast<float>(result.region.height) * 0.5f);
    std::optional<SmokePickTarget> best;
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
                best = SmokePickTarget{.entity = object.entity,
                                       .object = object.object,
                                       .viewPoint = point + glm::vec2(static_cast<float>(result.region.x),
                                                                      static_cast<float>(result.region.y))};
                bestDistance = distance;
            }
        }
    }
    return best;
}
} // namespace

bool EditorSession::updateSmokePicking(ApplicationContext& context) {
    // From frame 40: a whole-view GPU pick of the active viewport finds a
    // selectable object; view3d.select at one of its pixels must select it
    // through the id buffer; view3d.localview must isolate it (the render
    // view and a second pick hold only the isolated objects); leaving local
    // view must restore the full view. A scene with nothing pickable skips.
    constexpr uint32_t firstFrame = 40;
    constexpr uint32_t lastFrame = 160;
    if (m_smokeFrame < firstFrame || m_smokePickPhase == SmokePickPhase::Done) {
        return m_smokePickPhase == SmokePickPhase::Done;
    }
    const auto finish = [this]() { m_smokePickPhase = SmokePickPhase::Done; };
    const auto fail = [&](const std::string& message) {
        const std::string error = "Picking smoke failed: " + message;
        std::cerr << error << '\n';
        m_diagnostics.reportError(error);
        context.control.requestSceneChange({std::numeric_limits<uint32_t>::max()});
        finish();
    };
    RenderRuntime& rendering = context.rendering;
    ViewportPanel* viewport = m_viewports.active();
    if (viewport == nullptr || !viewport->renderView()) {
        fail("no viewport with a render view is open");
        return true;
    }
    if (m_smokeFrame > lastFrame) {
        fail("the picks did not finish in time");
        return true;
    }
    const RenderViewId view = viewport->renderView();
    const auto requestWholeView = [&]() {
        const glm::vec2 size = viewport->size();
        m_smokePickTicket = rendering.requestPick(
            view, {.width = static_cast<uint32_t>(size.x), .height = static_cast<uint32_t>(size.y)});
        return static_cast<bool>(m_smokePickTicket);
    };
    // A finished whole-view pick, or nullopt while it is pending.
    const auto takeWholeView = [&]() -> std::optional<PickResult> {
        const PickStatus status = rendering.pickStatus(m_smokePickTicket);
        if (status == PickStatus::Pending) {
            return std::nullopt;
        }
        PickResult result = rendering.pickResult(m_smokePickTicket);
        if (status != PickStatus::Ready) {
            fail("a whole-view pick ended without a result: " + result.error);
            return std::nullopt;
        }
        return result;
    };

    switch (m_smokePickPhase) {
    case SmokePickPhase::Idle:
        if (!rendering.viewTexture(view)) {
            fail("the active viewport has not rendered");
        } else if (!requestWholeView()) {
            fail("the renderer refused a whole-view pick");
        } else {
            m_smokePickPhase = SmokePickPhase::WholeView;
        }
        break;
    case SmokePickPhase::WholeView: {
        const std::optional<PickResult> result = takeWholeView();
        if (!result) {
            break;
        }
        const std::optional<SmokePickTarget> target = centralSelectableObject(*result, context.scene);
        if (!target) {
            std::cout << "Picking smoke skipped: nothing selectable is in view\n";
            finish();
            break;
        }
        m_smokePickEntity = target->entity;
        m_smokePickObject = target->object;
        m_selection.clear();
        invokeOperator(context, "view3d.select",
                       nlohmann::json{{"x", target->viewPoint.x}, {"y", target->viewPoint.y}}.dump(),
                       KeymapRegion::Viewport);
        m_smokePickPhase = SmokePickPhase::Click;
        break;
    }
    case SmokePickPhase::Click:
        if (viewport->pickPending()) {
            break;
        }
        if (!viewport->lastPickOutcome() || !viewport->lastPickOutcome()->fromGpu) {
            fail("the click was not answered by the GPU id buffer");
        } else if (m_selection.ids() != std::vector<SceneEntityId>{m_smokePickEntity}) {
            fail("the click did not select the object the id buffer shows at that pixel");
        } else if (!invokeOperator(context, "view3d.localview", {}, KeymapRegion::Viewport)) {
            fail("view3d.localview did not enter local view");
        } else {
            m_smokePickPhase = SmokePickPhase::LocalView;
        }
        break;
    case SmokePickPhase::LocalView: {
        const std::optional<std::vector<RenderObjectId>> isolated = rendering.viewVisibleEntities(view);
        if (!isolated || !std::ranges::binary_search(*isolated, m_smokePickObject)) {
            fail("local view did not isolate the selection in the render view");
        } else if (!requestWholeView()) {
            fail("the renderer refused a local-view pick");
        } else {
            m_smokePickPhase = SmokePickPhase::LocalViewPick;
        }
        break;
    }
    case SmokePickPhase::LocalViewPick: {
        const std::optional<PickResult> result = takeWholeView();
        if (!result) {
            break;
        }
        const std::optional<std::vector<RenderObjectId>> isolated = rendering.viewVisibleEntities(view);
        const bool onlyIsolated = isolated && std::ranges::all_of(result->objects, [&](const PickedObject& object) {
                                      return std::ranges::binary_search(*isolated, object.object);
                                  });
        if (!onlyIsolated || result->objects.empty()) {
            fail("a pick in local view saw objects outside it");
        } else if (!invokeOperator(context, "view3d.localview", {}, KeymapRegion::Viewport)) {
            fail("view3d.localview did not leave local view");
        } else {
            m_smokePickPhase = SmokePickPhase::LeftLocalView;
        }
        break;
    }
    case SmokePickPhase::LeftLocalView:
        if (rendering.viewVisibleEntities(view) || viewport->localViewActive()) {
            fail("leaving local view did not restore the full view");
        } else {
            m_selection.clear();
            std::cout << "Picking smoke passed: a GPU click and local view in the active viewport\n";
            finish();
        }
        break;
    case SmokePickPhase::Done:
        break;
    }
    return m_smokePickPhase == SmokePickPhase::Done;
}
