#include "Cursor3D.hpp"

#include "editor/document/EditorDocument.hpp"
#include "editor/interaction/EditorCamera.hpp"
#include "editor/operators/EditorOperatorFamilies.hpp"
#include "editor/panels/ViewportPanel.hpp"
#include "editor/panels/ViewportRegistry.hpp"
#include "editor/theme/EditorPalette.hpp"
#include "engine/graphics/DebugDraw.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <utility>

namespace {
constexpr const char* noViewportError = "No viewport is open";
// How far in front of the lens a click over empty space puts the cursor when
// the cursor is behind the lens, in meters.
constexpr float fallbackCursorDepth = 5.0f;
constexpr uint32_t ringDashes = 16;
// Crosshair ticks run between these multiples of the ring's radius.
constexpr float tickStart = 1.4f;
constexpr float tickEnd = 2.2f;

struct PlaceCursorProperties {
    // View pixels from the image's top-left corner; negative uses the mouse.
    float x{-1.0f};
    float y{-1.0f};
};

struct SnapToCursorProperties {
    // Keep the selected objects' offsets from their median.
    bool offset{false};
};

glm::vec4 debugColor(ThemeColor color) {
    return {color.r, color.g, color.b, color.a};
}

// The world position of an entity's origin; world transforms must be current.
glm::vec3 worldOrigin(const Scene& scene, Entity entity) {
    return glm::vec3(scene.registry().get<TransformComponent>(entity).world[3]);
}

// Moves entity so its world origin is at location, keeping its rotation and
// scale.
void moveOriginTo(Scene& scene, Entity entity, const glm::vec3& location) {
    const Entity parent = scene.registry().get<RelationshipComponent>(entity).parent;
    const glm::vec4 local =
        parent == entt::null
            ? glm::vec4(location, 1.0f)
            : glm::inverse(scene.registry().get<TransformComponent>(parent).world) * glm::vec4(location, 1.0f);
    scene.registry().get<TransformComponent>(entity).local[3] = local;
    scene.markTransformDirty(entity);
}

std::optional<glm::vec3> selectionMedian(const Scene& scene, std::span<const SceneEntityId> entities) {
    glm::vec3 sum(0.0f);
    size_t count = 0;
    for (SceneEntityId id : entities) {
        if (const Entity entity = scene.findEntity(id); entity != entt::null) {
            sum += worldOrigin(scene, entity);
            ++count;
        }
    }
    return count == 0 ? std::nullopt : std::optional<glm::vec3>(sum / static_cast<float>(count));
}

// The active viewport, when it shows context's document.
const ViewportPanel* documentViewport(const ViewportRegistry& viewports, const OperatorContext& context) {
    const ViewportPanel* viewport = viewports.active();
    return viewport != nullptr && viewport->showsDocument(context.editing.document()) ? viewport : nullptr;
}

Operator placeCursorOperator(ViewportRegistry& viewports) {
    return {.id = "view3d.cursor3d_place",
            .label = "Set 3D Cursor",
            .description = "Place the 3D cursor on the surface under a point of the viewport",
            .flags = OperatorFlags::Internal,
            .properties =
                OperatorProperties::make(PlaceCursorProperties{}, std::move(Fields<PlaceCursorProperties>()
                                                                                .add("x", &PlaceCursorProperties::x)
                                                                                .add("y", &PlaceCursorProperties::y))),
            .poll = [&viewports](
                        const OperatorContext& context) { return documentViewport(viewports, context) != nullptr; },
            .execute =
                [&viewports](OperatorContext& context, OperatorProperties& properties) {
                    const ViewportPanel* viewport = documentViewport(viewports, context);
                    if (viewport == nullptr) {
                        return OperatorResult::failed(noViewportError);
                    }
                    const PlaceCursorProperties values = properties.get<PlaceCursorProperties>() != nullptr
                                                             ? *properties.get<PlaceCursorProperties>()
                                                             : PlaceCursorProperties{};
                    const glm::vec2 point =
                        values.x < 0.0f || values.y < 0.0f ? viewport->mouseInView() : glm::vec2(values.x, values.y);
                    Scene& scene = context.editing.scene();
                    scene.updateWorldTransforms();
                    const std::optional<std::vector<RenderObjectId>>& isolation = viewport->localViewObjects();
                    const SnapView view{.lens = viewport->lens(scene),
                                        .viewSize = viewport->size(),
                                        .isolatedObjects = isolation ? &*isolation : nullptr};
                    glm::vec3& cursor = context.editing.editorData().cursorLocation;
                    cursor = cursorPlacement(scene, context.editing.assets(), view, point, cursor);
                    return OperatorResult::finished();
                }};
}

Operator snapSelectedToCursorOperator() {
    return {.id = "view3d.snap_selected_to_cursor",
            .label = "Selection to Cursor",
            .description = "Move the selected objects to the 3D cursor",
            .flags = OperatorFlags::Undo | OperatorFlags::Register,
            .properties = OperatorProperties::make(
                SnapToCursorProperties{},
                std::move(Fields<SnapToCursorProperties>().add("offset", &SnapToCursorProperties::offset))),
            .poll = [](const OperatorContext& context) { return context.editMode() && !context.selection.empty(); },
            .execute =
                [](OperatorContext& context, OperatorProperties& properties) {
                    const SnapToCursorProperties* values = properties.get<SnapToCursorProperties>();
                    const bool keepOffsets = values != nullptr && values->offset;
                    Scene& scene = context.editing.scene();
                    scene.updateWorldTransforms();
                    const std::vector<SceneEntityId> roots = context.selection.transformRoots(scene);
                    const std::optional<glm::vec3> median = selectionMedian(scene, roots);
                    if (!median) {
                        return OperatorResult::cancelled("The selected objects no longer exist");
                    }
                    const glm::vec3 cursor = context.editing.editorData().cursorLocation;
                    // Every target is computed before anything moves.
                    std::vector<std::pair<Entity, glm::vec3>> targets;
                    for (SceneEntityId id : roots) {
                        if (const Entity entity = scene.findEntity(id); entity != entt::null) {
                            targets.emplace_back(entity,
                                                 keepOffsets ? cursor + worldOrigin(scene, entity) - *median : cursor);
                        }
                    }
                    for (const auto& [entity, location] : targets) {
                        moveOriginTo(scene, entity, location);
                    }
                    scene.updateWorldTransforms();
                    return OperatorResult::finished();
                },
            .scope = selectionScope()};
}
} // namespace

glm::vec3 cursorPlacement(const Scene& scene, const AssetManager& assets, const SnapView& view, glm::vec2 viewPoint,
                          const glm::vec3& currentCursor) {
    return placementUnderMouse(scene, assets, view, viewPoint, placementDepth(view.lens, currentCursor)).position;
}

float placementDepth(const RenderView& lens, const glm::vec3& currentCursor) {
    const float depth = glm::dot(currentCursor - lens.position, glm::normalize(lens.forward));
    return depth <= lens.nearPlane ? fallbackCursorDepth : depth;
}

void recordCursor3D(const glm::vec3& location, const RenderView& lens, glm::vec2 viewSize, DebugDraw& output) {
    const glm::vec3 forward = glm::normalize(lens.forward);
    const float depth = glm::dot(location - lens.position, forward);
    if (depth <= lens.nearPlane || viewSize.y <= 0.0f) {
        return;
    }
    const float pixelsPerUnit = viewSize.y * 0.5f / std::tan(lens.verticalFovRadians * 0.5f) / depth;
    const float radius = cursorRingRadiusPixels / pixelsPerUnit;
    const glm::vec3 right = glm::normalize(glm::cross(forward, lens.up));
    const glm::vec3 up = glm::cross(right, forward);
    const auto onRing = [&](float angle) {
        return location + radius * (std::cos(angle) * right + std::sin(angle) * up);
    };
    const DebugStyle ring{.color = debugColor(palette::cursorRing), .depth = DebugDepth::AlwaysOnTop};
    const DebugStyle ringAlternate{.color = debugColor(palette::cursorRingAlternate), .depth = DebugDepth::AlwaysOnTop};
    for (uint32_t dash = 0; dash < ringDashes; ++dash) {
        const float step = glm::two_pi<float>() / static_cast<float>(ringDashes);
        output.line(onRing(step * static_cast<float>(dash)), onRing(step * static_cast<float>(dash + 1)),
                    dash % 2 == 0 ? ring : ringAlternate);
    }
    const DebugStyle crosshair{.color = debugColor(palette::cursorCrosshair), .depth = DebugDepth::AlwaysOnTop};
    for (const glm::vec3& direction : {right, up, -right, -up}) {
        output.line(location + direction * radius * tickStart, location + direction * radius * tickEnd, crosshair);
    }
}

std::vector<Operator> cursorOperators(ViewportRegistry& viewports) {
    std::vector<Operator> operators;
    operators.push_back(placeCursorOperator(viewports));
    operators.push_back({.id = "view3d.cursor_reset",
                         .label = "Cursor to World Origin",
                         .description = "Move the 3D cursor back to the world origin",
                         .execute = [](OperatorContext& context, OperatorProperties&) {
                             context.editing.editorData().cursorLocation = glm::vec3(0.0f);
                             return OperatorResult::finished();
                         }});
    operators.push_back({.id = "view3d.snap_cursor_to_selected",
                         .label = "Cursor to Selected",
                         .description = "Move the 3D cursor to the median of the selected objects",
                         .poll = [](const OperatorContext& context) { return !context.selection.empty(); },
                         .execute =
                             [](OperatorContext& context, OperatorProperties&) {
                                 Scene& scene = context.editing.scene();
                                 scene.updateWorldTransforms();
                                 const std::optional<glm::vec3> median =
                                     selectionMedian(scene, context.selection.ids());
                                 if (!median) {
                                     return OperatorResult::cancelled("The selected objects no longer exist");
                                 }
                                 context.editing.editorData().cursorLocation = *median;
                                 return OperatorResult::finished();
                             }});
    operators.push_back(snapSelectedToCursorOperator());
    return operators;
}

OperatorRegistrationResult registerCursorOperatorFamily(OperatorRegistry& registry,
                                                        const EditorOperatorServices& services) {
    return addOperators(registry, cursorOperators(services.viewports));
}
