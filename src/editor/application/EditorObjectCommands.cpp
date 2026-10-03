// Blender object-mode operators that need the running editor: modal G/R/S
// transforms, the Add/Delete/Rename popups, and the viewpoint keys. Operators
// that only edit the document live in editor/operators/ObjectOperators.

#include "EditorSession.hpp"

#include "editor/application/EntityCommands.hpp"
#include "editor/operators/ObjectOperators.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <utility>

namespace {
// Blender orbits 15 degrees per numpad press.
constexpr float orbitStepRadians = 0.261799388f;
constexpr float quarterTurn = glm::half_pi<float>();
constexpr float halfTurn = glm::pi<float>();
constexpr const char* addMenuPopup = "ObjectAddMenu";
constexpr const char* deleteMenuPopup = "ObjectDeleteMenu";
constexpr const char* renamePopup = "ObjectRename";

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

ImU32 axisColor(TransformAxis axis) {
    switch (axis) {
    case TransformAxis::X:
        return IM_COL32(230, 70, 70, 255);
    case TransformAxis::Y:
        return IM_COL32(120, 200, 60, 255);
    case TransformAxis::Z:
        return IM_COL32(70, 130, 240, 255);
    }
    return IM_COL32_WHITE;
}

// Typed characters for modal numeric input, from the main row and the numpad.
std::optional<char> pressedNumericCharacter() {
    for (int digit = 0; digit <= 9; ++digit) {
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_0 + digit), false) ||
            ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_Keypad0 + digit), false)) {
            return static_cast<char>('0' + digit);
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Period, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadDecimal, false)) {
        return '.';
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Minus, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, false)) {
        return '-';
    }
    return std::nullopt;
}

glm::mat3 normalizedAxes(const glm::mat4& world) {
    glm::mat3 axes(1.0f);
    for (int column = 0; column < 3; ++column) {
        const glm::vec3 basis(world[column]);
        if (glm::length(basis) > 1e-8f) {
            axes[column] = glm::normalize(basis);
        }
    }
    return axes;
}
} // namespace

void EditorSession::handleObjectKeys(ApplicationContext& context, KeymapRegion region) {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || m_playMode.mode() != EditorMode::Edit || m_activeTransform || m_fileDialog.pending() ||
        m_keymapEditor.capturing() || ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
        ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        return;
    }
    dispatchKeymap(context, region);
}

void EditorSession::registerObjectModeOperators() {
    const auto canTransform = [this](const OperatorContext& context) {
        return context.app != nullptr && context.editMode() && !context.selection.empty() && !m_activeTransform;
    };
    const auto modalTransform = [this, canTransform](std::string id, std::string label, std::string description,
                                                     TransformOperation operation, std::string commandName) {
        return Operator{.id = std::move(id),
                        .label = std::move(label),
                        .description = std::move(description),
                        .poll = canTransform,
                        .execute = [this, operation, commandName](OperatorContext& context, OperatorProperties&) {
                            return beginModalTransform(*context.app, operation, commandName, {}, false)
                                       ? OperatorResult::finished()
                                       : OperatorResult::cancelled();
                        }};
    };
    registerOperator(modalTransform("transform.translate", "Move", "Move the selection with the mouse",
                                    TransformOperation::Translate, "Move"));
    registerOperator(modalTransform("transform.rotate", "Rotate", "Rotate the selection with the mouse",
                                    TransformOperation::Rotate, "Rotate"));
    registerOperator(modalTransform("transform.resize", "Scale", "Scale the selection with the mouse",
                                    TransformOperation::Scale, "Resize"));
    // The copy follows the mouse; the copy and its move form one undo step,
    // and cancelling the move keeps the copy.
    registerOperator(
        {.id = "object.duplicate_move",
         .label = "Duplicate and Move",
         .description = "Duplicate the selection and move the copies with the mouse",
         .poll = canTransform,
         .execute = [this](OperatorContext& context, OperatorProperties&) {
             std::string before;
             if (!EntityCommands::duplicateSelectionUncommitted(context.editing, m_selection, before)) {
                 return OperatorResult::cancelled("Nothing was duplicated");
             }
             if (!beginModalTransform(*context.app, TransformOperation::Translate, "Duplicate Objects", before, true)) {
                 context.editing.commit("Duplicate Objects", {}, std::move(before));
             }
             return OperatorResult::finished();
         }});

    const auto inEditMode = [](const OperatorContext& context) { return context.app != nullptr && context.editMode(); };
    registerOperator(applicationOperator(
        "object.delete_menu", "Delete...", "Ask before deleting the selected objects",
        [](const OperatorContext& context) { return context.editMode() && !context.selection.empty(); },
        [this](ApplicationContext&) { m_openDeleteMenu = true; }));
    registerOperator(applicationOperator("object.add_menu", "Add...", "Open the Add menu at the mouse", inEditMode,
                                         [this](ApplicationContext&) { m_openAddMenu = true; }));
    registerOperator(applicationOperator(
        "object.rename_popup", "Rename...", "Type a new name for the active object",
        [](const OperatorContext& context) {
            return context.editMode() && !context.selection.empty() &&
                   context.editing.scene().findEntity(*context.selection.active()) != entt::null;
        },
        [this](ApplicationContext& app) {
            const Entity active = app.scene.findEntity(*m_selection.active());
            m_renameBuffer = app.scene.registry().get<NameComponent>(active).name;
            m_openRenamePopup = true;
            m_focusRenameField = true;
        }));

    registerOperator(applicationOperator(
        "view3d.toggle_snapping", "Toggle Snapping", "Turn transform snapping on or off", {},
        [this](ApplicationContext&) { m_viewport.tools().snapping = !m_viewport.tools().snapping; }));
    for (Operator& op : m_viewport.chromeOperators()) {
        registerOperator(std::move(op));
    }
    registerOperator(applicationOperator(
        "view3d.frame_selected", "Frame Selected", "Move the view to show the selected objects",
        [](const OperatorContext& context) { return !context.selection.empty(); },
        [this](ApplicationContext& app) { frameEntities(app, m_selection.ids()); }));
    registerOperator(applicationOperator("view3d.frame_all", "Frame All", "Move the view to show every object", {},
                                         [this](ApplicationContext& app) { frameAll(app); }));
    registerOperator({.id = "view3d.view_axis",
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
                                                  {"bottom", "Bottom", static_cast<int64_t>(ViewAxis::Bottom)}}))),
                      .poll = [](const OperatorContext& context) { return context.app != nullptr; },
                      .execute =
                          [this](OperatorContext&, OperatorProperties& properties) {
                              const ViewAxisProperties* values = properties.get<ViewAxisProperties>();
                              const auto [yaw, pitch] =
                                  viewAxisAngles(values != nullptr ? values->type : ViewAxis::Front);
                              m_camera.setViewAngles(yaw, pitch);
                              return OperatorResult::finished();
                          }});
    registerOperator(applicationOperator(
        "view3d.view_opposite", "View Opposite", "Look from the opposite side", {},
        [this](ApplicationContext&) { m_camera.setViewAngles(m_camera.yaw() + halfTurn, -m_camera.pitch()); }));
    registerOperator({.id = "view3d.view_orbit",
                      .label = "Orbit View",
                      .description = "Orbit the view 15 degrees around its pivot",
                      .properties = OperatorProperties::make(
                          OrbitProperties{},
                          std::move(Fields<OrbitProperties>()
                                        .add("direction", &OrbitProperties::direction)
                                        .options({{"left", "Left", static_cast<int64_t>(OrbitDirection::Left)},
                                                  {"right", "Right", static_cast<int64_t>(OrbitDirection::Right)},
                                                  {"up", "Up", static_cast<int64_t>(OrbitDirection::Up)},
                                                  {"down", "Down", static_cast<int64_t>(OrbitDirection::Down)}}))),
                      .poll = [](const OperatorContext& context) { return context.app != nullptr; },
                      .execute =
                          [this](OperatorContext&, OperatorProperties& properties) {
                              const OrbitProperties* values = properties.get<OrbitProperties>();
                              const glm::vec2 step =
                                  orbitStep(values != nullptr ? values->direction : OrbitDirection::Left);
                              m_camera.orbitAroundPivot(step.x, step.y);
                              return OperatorResult::finished();
                          }});
}

void EditorSession::drawObjectPopups(ApplicationContext& context) {
    if (m_openAddMenu) {
        ImGui::OpenPopup(addMenuPopup);
        m_openAddMenu = false;
    }
    if (ImGui::BeginPopup(addMenuPopup)) {
        ImGui::TextDisabled("Add");
        ImGui::Separator();
        if (ImGui::BeginMenu("Mesh")) {
            operatorMenuItem(context, "object.add_cube", "Cube");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Light")) {
            operatorMenuItem(context, "object.add_sun", "Sun");
            ImGui::EndMenu();
        }
        operatorMenuItem(context, "object.add_environment", "Environment");
        operatorMenuItem(context, "object.add_fog", "Fog");
        operatorMenuItem(context, "object.add_empty", "Empty");
        ImGui::EndPopup();
    }

    if (m_openDeleteMenu) {
        ImGui::OpenPopup(deleteMenuPopup);
        m_openDeleteMenu = false;
    }
    if (ImGui::BeginPopup(deleteMenuPopup)) {
        operatorMenuItem(context, "object.delete", "Delete");
        ImGui::EndPopup();
    }

    if (m_openRenamePopup) {
        ImGui::OpenPopup(renamePopup);
        m_openRenamePopup = false;
    }
    if (ImGui::BeginPopup(renamePopup)) {
        ImGui::TextDisabled("Name");
        if (m_focusRenameField) {
            ImGui::SetKeyboardFocusHere();
            m_focusRenameField = false;
        }
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::InputText("##RenameObject", &m_renameBuffer,
                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
            if (const Operator* rename = m_operators.find("object.rename")) {
                OperatorProperties properties = rename->properties;
                if (RenameProperties* values = properties.get<RenameProperties>()) {
                    values->name = m_renameBuffer;
                }
                invokeOperator(context, "object.rename", properties);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void EditorSession::frameEntities(ApplicationContext& context, const std::vector<SceneEntityId>& entities) {
    context.scene.updateWorldTransforms();
    constexpr float objectRadius = 0.5f;
    glm::vec3 lower(std::numeric_limits<float>::max());
    glm::vec3 upper(std::numeric_limits<float>::lowest());
    bool any = false;
    for (SceneEntityId id : entities) {
        const Entity entity = context.scene.findEntity(id);
        if (entity == entt::null) {
            continue;
        }
        const glm::mat4& world = context.scene.registry().get<TransformComponent>(entity).world;
        glm::vec3 center(world[3]);
        float radius = objectRadius;
        if (const auto* renderer = context.scene.registry().try_get<MeshRendererComponent>(entity)) {
            if (const MeshAsset* mesh = context.assets.mesh(renderer->mesh)) {
                const float scale = std::max({glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])),
                                              glm::length(glm::vec3(world[2]))});
                center = glm::vec3(world * glm::vec4(mesh->bounds.center, 1.0f));
                radius = mesh->bounds.radius * scale;
            }
        }
        lower = glm::min(lower, center - radius);
        upper = glm::max(upper, center + radius);
        any = true;
    }
    if (any) {
        m_camera.focusOnBounds((lower + upper) * 0.5f, std::max(glm::length(upper - lower) * 0.5f, objectRadius));
    }
}

void EditorSession::frameAll(ApplicationContext& context) {
    std::vector<SceneEntityId> all;
    for (Entity entity : context.scene.registry().view<const TransformComponent>()) {
        all.push_back(context.scene.id(entity));
    }
    frameEntities(context, all);
}

bool EditorSession::beginModalTransform(ApplicationContext& context, TransformOperation operation,
                                        std::string commandName, std::string before, bool commitOnCancel) {
    if (m_playMode.mode() != EditorMode::Edit || m_selection.empty()) {
        return false;
    }
    context.scene.updateWorldTransforms();
    std::vector<SceneEntityId> roots = m_selection.transformRoots(context.scene);
    if (roots.empty()) {
        return false;
    }
    if (before.empty() && !editing(context).capture(before)) {
        return false;
    }
    std::vector<glm::mat4> startWorld;
    std::vector<glm::mat4> startLocal;
    for (SceneEntityId id : roots) {
        const auto& transform = context.scene.registry().get<TransformComponent>(context.scene.findEntity(id));
        startWorld.push_back(transform.world);
        startLocal.push_back(transform.local);
    }
    glm::mat3 localAxes(1.0f);
    if (const Entity active = context.scene.findEntity(*m_selection.active()); active != entt::null) {
        localAxes = normalizedAxes(context.scene.registry().get<TransformComponent>(active).world);
    }
    const glm::vec2 viewportSize = m_viewport.size();
    const float aspectRatio = viewportSize.x / std::max(viewportSize.y, 1.0f);
    const ModalTransformView view{.view = m_camera.viewMatrix(),
                                  .projection = m_camera.projectionMatrix(aspectRatio),
                                  .viewportPosition = m_viewport.position(),
                                  .viewportSize = viewportSize};
    const ImVec2 mouse = ImGui::GetMousePos();
    m_activeTransform.emplace(ActiveTransform{
        .transform = ModalTransform(operation, std::move(startWorld), localAxes, {mouse.x, mouse.y}, view),
        .entities = std::move(roots),
        .startLocal = std::move(startLocal),
        .before = std::move(before),
        .commandName = std::move(commandName),
        .commitOnCancel = commitOnCancel});
    return true;
}

bool EditorSession::modalTransformActive() const {
    return m_activeTransform.has_value();
}

void EditorSession::updateModalTransform(ApplicationContext& context) {
    ModalTransform& transform = m_activeTransform->transform;
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            m_viewport.suppressCameraUntilRightRelease();
        }
        finishModalTransform(context, false);
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        finishModalTransform(context, true);
        return;
    }

    if (!io.KeyCtrl && !io.KeyAlt) {
        if (!io.KeyShift) {
            if (ImGui::IsKeyPressed(ImGuiKey_G, false)) {
                transform.setOperation(TransformOperation::Translate);
            } else if (ImGui::IsKeyPressed(ImGuiKey_R, false)) {
                transform.setOperation(TransformOperation::Rotate);
            } else if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
                transform.setOperation(TransformOperation::Scale);
            }
        }
        constexpr std::array<std::pair<ImGuiKey, TransformAxis>, 3> axisKeys{
            {{ImGuiKey_X, TransformAxis::X}, {ImGuiKey_Y, TransformAxis::Y}, {ImGuiKey_Z, TransformAxis::Z}}};
        for (const auto& [key, axis] : axisKeys) {
            if (ImGui::IsKeyPressed(key, false)) {
                transform.pressAxis(axis, io.KeyShift);
            }
        }
        if (const std::optional<char> character = pressedNumericCharacter()) {
            transform.typeCharacter(*character);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
            transform.eraseCharacter();
        }
    }

    transform.moveMouse({io.MousePos.x, io.MousePos.y}, io.KeyShift);
    // Ctrl inverts the snapping toggle while it is held, as in Blender.
    const TransformToolSettings& tools = m_viewport.tools();
    const TransformSnapping snapping{.enabled = tools.snapping != io.KeyCtrl,
                                     .translation = tools.translationSnap,
                                     .rotationDegrees = tools.rotationSnapDegrees,
                                     .scale = tools.scaleSnap};
    const ModalTransformResult result = transform.evaluate(snapping);
    const std::vector<SceneEntityId>& entities = m_activeTransform->entities;
    for (size_t index = 0; index < entities.size() && index < result.world.size(); ++index) {
        const Entity entity = context.scene.findEntity(entities[index]);
        if (entity == entt::null) {
            continue;
        }
        const Entity parent = context.scene.registry().get<RelationshipComponent>(entity).parent;
        context.scene.registry().get<TransformComponent>(entity).local =
            parent == entt::null
                ? result.world[index]
                : glm::inverse(context.scene.registry().get<TransformComponent>(parent).world) * result.world[index];
        context.scene.markTransformDirty(entity);
    }
    context.scene.updateWorldTransforms();
    drawModalTransformOverlay(result);
}

void EditorSession::finishModalTransform(ApplicationContext& context, bool confirm) {
    ActiveTransform active = std::move(*m_activeTransform);
    m_activeTransform.reset();
    if (!confirm) {
        for (size_t index = 0; index < active.entities.size(); ++index) {
            if (const Entity entity = context.scene.findEntity(active.entities[index]); entity != entt::null) {
                context.scene.registry().get<TransformComponent>(entity).local = active.startLocal[index];
                context.scene.markTransformDirty(entity);
            }
        }
        context.scene.updateWorldTransforms();
    }
    if (confirm || active.commitOnCancel) {
        editing(context).commit(std::move(active.commandName), {}, std::move(active.before));
    }
}

void EditorSession::drawModalTransformOverlay(const ModalTransformResult& result) const {
    const ModalTransform& transform = m_activeTransform->transform;
    const glm::vec2 viewportPosition = m_viewport.position();
    const glm::vec2 viewportSize = m_viewport.size();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 clipMin{viewportPosition.x, viewportPosition.y};
    const ImVec2 clipMax{viewportPosition.x + viewportSize.x, viewportPosition.y + viewportSize.y};
    drawList->PushClipRect(clipMin, clipMax, true);

    // Constraint axes are drawn as lines through the pivot across the view.
    const glm::vec2 pivot = transform.pivotOnScreen();
    const float extent = glm::length(viewportSize) * 2.0f;
    for (const ConstraintGuide& guide : transform.constraintGuides()) {
        const glm::vec2 along = transform.toScreen(transform.pivot() + guide.direction) - pivot;
        if (glm::length(along) < 1e-3f) {
            continue;
        }
        const glm::vec2 direction = glm::normalize(along) * extent;
        drawList->AddLine({pivot.x - direction.x, pivot.y - direction.y},
                          {pivot.x + direction.x, pivot.y + direction.y}, axisColor(guide.axis), 1.5f);
    }

    // Rotate and scale show a dashed line from the pivot to the cursor.
    if (transform.operation() != TransformOperation::Translate) {
        const glm::vec2 mouse = transform.mouse();
        const glm::vec2 span = mouse - pivot;
        const float length = glm::length(span);
        constexpr float dash = 6.0f;
        for (float start = 0.0f; start < length; start += dash * 2.0f) {
            const glm::vec2 from = pivot + span * (start / length);
            const glm::vec2 to = pivot + span * (std::min(start + dash, length) / length);
            drawList->AddLine({from.x, from.y}, {to.x, to.y}, IM_COL32(20, 20, 20, 230), 1.5f);
        }
    }

    // Header text in Blender's place for operator feedback.
    const char* hint = transform.operation() == TransformOperation::Rotate
                           ? "LMB/Enter confirm   RMB/Esc cancel   X Y Z axis   R trackball   Shift precision   "
                             "Ctrl snap"
                           : "LMB/Enter confirm   RMB/Esc cancel   X Y Z axis   Shift precision   Ctrl snap";
    const ImVec2 position{viewportPosition.x + 8.0f, viewportPosition.y + 8.0f};
    const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
    const ImVec2 descriptionSize = ImGui::CalcTextSize(result.description.c_str());
    const ImVec2 hintSize = ImGui::CalcTextSize(hint);
    drawList->AddRectFilled(
        {position.x - 6.0f, position.y - 4.0f},
        {position.x + std::max(descriptionSize.x, hintSize.x) + 6.0f, position.y + lineHeight + hintSize.y + 4.0f},
        IM_COL32(24, 24, 24, 220), 4.0f);
    drawList->AddText(position, ImGui::GetColorU32(ImGuiCol_Text), result.description.c_str());
    drawList->AddText({position.x, position.y + lineHeight}, ImGui::GetColorU32(ImGuiCol_TextDisabled), hint);
    drawList->PopClipRect();
}
