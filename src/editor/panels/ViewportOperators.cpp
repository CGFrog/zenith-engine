#include "ViewportOperators.hpp"

#include "editor/interaction/Selection.hpp"
#include "editor/panels/ViewportPanel.hpp"
#include "editor/viewport/ViewFraming.hpp"
#include "editor/viewport/ViewportSelectOperators.hpp"
#include "engine/assets/Assets.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace {
// Blender orbits 15 degrees per numpad press.
constexpr float orbitStepRadians = 0.261799388f;
constexpr float quarterTurn = glm::half_pi<float>();
constexpr float halfTurn = glm::pi<float>();
constexpr const char* noViewportError = "No viewport is open";

enum class ViewAxis : uint8_t {
    Front,
    Back,
    Right,
    Left,
    Top,
    Bottom,
};

struct ViewAxisProperties {
    ViewAxis type{ViewAxis::Front};
};

enum class OrbitDirection : uint8_t {
    Left,
    Right,
    Up,
    Down,
};

struct OrbitProperties {
    OrbitDirection direction{OrbitDirection::Left};
};

// Yaw and pitch, in radians, of Blender's numpad viewpoints.
std::pair<float, float> viewAxisAngles(ViewAxis axis) {
    switch (axis) {
    case ViewAxis::Front:
        return {0.0f, 0.0f};
    case ViewAxis::Back:
        return {halfTurn, 0.0f};
    case ViewAxis::Right:
        return {-quarterTurn, 0.0f};
    case ViewAxis::Left:
        return {quarterTurn, 0.0f};
    case ViewAxis::Top:
        return {0.0f, -quarterTurn};
    case ViewAxis::Bottom:
        return {0.0f, quarterTurn};
    }
    return {0.0f, 0.0f};
}

// Yaw and pitch change of one numpad orbit step.
glm::vec2 orbitStep(OrbitDirection direction) {
    switch (direction) {
    case OrbitDirection::Left:
        return {orbitStepRadians, 0.0f};
    case OrbitDirection::Right:
        return {-orbitStepRadians, 0.0f};
    case OrbitDirection::Up:
        return {0.0f, -orbitStepRadians};
    case OrbitDirection::Down:
        return {0.0f, orbitStepRadians};
    }
    return glm::vec2(0.0f);
}

// The camera Numpad 0 looks through: the active object when it is a camera,
// otherwise the scene's primary camera, otherwise any camera.
std::optional<SceneEntityId> cameraToLookThrough(const Scene& scene, const Selection& selection) {
    if (const std::optional<SceneEntityId> active = selection.active()) {
        const Entity entity = scene.findEntity(*active);
        if (entity != entt::null && scene.registry().all_of<CameraComponent>(entity)) {
            return active;
        }
    }
    std::optional<SceneEntityId> fallback;
    for (const auto [entity, camera] : scene.registry().view<const CameraComponent>().each()) {
        if (camera.primary) {
            return scene.id(entity);
        }
        if (!fallback) {
            fallback = scene.id(entity);
        }
    }
    return fallback;
}

// An operator acting on the active viewport. run receives it and the
// operator's context and properties.
template <typename Run>
Operator viewportOperator(ViewportRegistry& viewports, Operator op, Run run,
                          std::function<bool(const OperatorContext&)> extraPoll = {}) {
    op.poll = [&viewports, extraPoll = std::move(extraPoll)](const OperatorContext& context) {
        return viewports.active() != nullptr && (!extraPoll || extraPoll(context));
    };
    op.execute = [&viewports, run = std::move(run)](OperatorContext& context, OperatorProperties& properties) {
        ViewportPanel* viewport = viewports.active();
        if (viewport == nullptr) {
            return OperatorResult::failed(noViewportError);
        }
        return run(*viewport, context, properties);
    };
    return op;
}
} // namespace

std::vector<Operator> viewportOperators(ViewportRegistry& viewports) {
    std::vector<Operator> operators = viewportChromeOperators([&viewports]() -> ViewportChromeState* {
        ViewportPanel* viewport = viewports.active();
        return viewport != nullptr ? &viewport->chrome() : nullptr;
    });

    operators.push_back(viewportOperator(viewports,
                                         {.id = "view3d.toggle_snapping",
                                          .label = "Toggle Snapping",
                                          .description = "Turn transform snapping on or off"},
                                         [](ViewportPanel& viewport, OperatorContext&, OperatorProperties&) {
                                             viewport.tools().snapping = !viewport.tools().snapping;
                                             return OperatorResult::finished();
                                         }));
    operators.push_back(viewportOperator(
        viewports,
        {.id = "view3d.view_axis",
         .label = "View Axis",
         .description = "Look along a world axis",
         .properties = OperatorProperties::make(
             ViewAxisProperties{},
             std::move(Fields<ViewAxisProperties>()
                           .add("type", &ViewAxisProperties::type)
                           .options({{"front", "Front", static_cast<int64_t>(ViewAxis::Front)},
                                     {"back", "Back", static_cast<int64_t>(ViewAxis::Back)},
                                     {"right", "Right", static_cast<int64_t>(ViewAxis::Right)},
                                     {"left", "Left", static_cast<int64_t>(ViewAxis::Left)},
                                     {"top", "Top", static_cast<int64_t>(ViewAxis::Top)},
                                     {"bottom", "Bottom", static_cast<int64_t>(ViewAxis::Bottom)}})))},
        [](ViewportPanel& viewport, OperatorContext&, OperatorProperties& properties) {
            const ViewAxisProperties* values = properties.get<ViewAxisProperties>();
            const auto [yaw, pitch] = viewAxisAngles(values != nullptr ? values->type : ViewAxis::Front);
            viewport.exitCameraView();
            viewport.camera().setViewAngles(yaw, pitch);
            return OperatorResult::finished();
        }));
    operators.push_back(viewportOperator(
        viewports,
        {.id = "view3d.view_opposite", .label = "View Opposite", .description = "Look from the opposite side"},
        [](ViewportPanel& viewport, OperatorContext&, OperatorProperties&) {
            viewport.exitCameraView();
            EditorCamera& camera = viewport.camera();
            camera.setViewAngles(camera.yaw() + halfTurn, -camera.pitch());
            return OperatorResult::finished();
        }));
    operators.push_back(viewportOperator(
        viewports,
        {.id = "view3d.view_orbit",
         .label = "Orbit View",
         .description = "Orbit the view 15 degrees around its pivot",
         .properties = OperatorProperties::make(
             OrbitProperties{},
             std::move(Fields<OrbitProperties>()
                           .add("direction", &OrbitProperties::direction)
                           .options({{"left", "Left", static_cast<int64_t>(OrbitDirection::Left)},
                                     {"right", "Right", static_cast<int64_t>(OrbitDirection::Right)},
                                     {"up", "Up", static_cast<int64_t>(OrbitDirection::Up)},
                                     {"down", "Down", static_cast<int64_t>(OrbitDirection::Down)}})))},
        [](ViewportPanel& viewport, OperatorContext&, OperatorProperties& properties) {
            const OrbitProperties* values = properties.get<OrbitProperties>();
            const glm::vec2 step = orbitStep(values != nullptr ? values->direction : OrbitDirection::Left);
            viewport.exitCameraView();
            viewport.camera().orbitAroundPivot(step.x, step.y);
            return OperatorResult::finished();
        }));
    operators.push_back(viewportOperator(
        viewports,
        {.id = "view3d.frame_selected",
         .label = "Frame Selected",
         .description = "Move the view to show the selected objects"},
        [](ViewportPanel& viewport, OperatorContext& context, OperatorProperties&) {
            viewport.exitCameraView();
            frameEntities(viewport.camera(), context.editing.scene(), context.editing.assets(),
                          context.selection.ids());
            return OperatorResult::finished();
        },
        [](const OperatorContext& context) { return !context.selection.empty(); }));
    operators.push_back(viewportOperator(
        viewports,
        {.id = "view3d.frame_all", .label = "Frame All", .description = "Move the view to show every object"},
        [](ViewportPanel& viewport, OperatorContext& context, OperatorProperties&) {
            Scene& scene = context.editing.scene();
            std::vector<SceneEntityId> all;
            for (const Entity entity : scene.registry().view<const TransformComponent>()) {
                all.push_back(scene.id(entity));
            }
            viewport.exitCameraView();
            frameEntities(viewport.camera(), scene, context.editing.assets(), all);
            return OperatorResult::finished();
        }));
    operators.push_back(viewportOperator(
        viewports,
        {.id = "view3d.view_camera",
         .label = "Camera View",
         .description = "Look through the active camera object or the scene's camera, or leave the camera view"},
        [](ViewportPanel& viewport, OperatorContext& context, OperatorProperties&) {
            if (viewport.cameraView()) {
                viewport.exitCameraView();
                return OperatorResult::finished();
            }
            Scene& scene = context.editing.scene();
            const std::optional<SceneEntityId> camera = cameraToLookThrough(scene, context.selection);
            if (!camera) {
                return OperatorResult::failed("The scene has no camera to look through");
            }
            scene.updateWorldTransforms();
            if (!viewport.lookThroughCamera(scene, *camera)) {
                return OperatorResult::failed("The camera object has no camera to look through");
            }
            return OperatorResult::finished();
        }));
    for (Operator& op : viewportSelectOperators(viewports)) {
        operators.push_back(std::move(op));
    }
    return operators;
}
