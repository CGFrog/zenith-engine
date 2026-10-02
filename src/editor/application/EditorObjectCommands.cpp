// Blender object-mode commands: modal G/R/S transforms, selection, add,
// delete, rename, clear transforms, and viewpoint keys.

#include "EditorSession.hpp"

#include "editor/application/EntityCommands.hpp"
#include "editor/document/DefaultScene.hpp"

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
        ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        return;
    }
    for (const ObjectKeyBinding& binding : objectKeymap()) {
        if ((binding.regions & static_cast<uint8_t>(region)) != 0 && ImGui::IsKeyChordPressed(binding.chord)) {
            runObjectCommand(context, binding.command, region);
            return;
        }
    }
}

void EditorSession::runObjectCommand(ApplicationContext& context, ObjectCommand command, KeymapRegion region) {
    switch (command) {
    case ObjectCommand::Grab:
        beginModalTransform(context, TransformOperation::Translate, "Move", {}, false);
        break;
    case ObjectCommand::Rotate:
        beginModalTransform(context, TransformOperation::Rotate, "Rotate", {}, false);
        break;
    case ObjectCommand::Scale:
        beginModalTransform(context, TransformOperation::Scale, "Resize", {}, false);
        break;
    case ObjectCommand::ClearLocation:
        clearSelectedTransforms(context, TransformPart::Location);
        break;
    case ObjectCommand::ClearRotation:
        clearSelectedTransforms(context, TransformPart::Rotation);
        break;
    case ObjectCommand::ClearScale:
        clearSelectedTransforms(context, TransformPart::Scale);
        break;
    case ObjectCommand::Duplicate: {
        // In the viewport the duplicate follows the mouse; the copy and its
        // move form one undo step, and cancelling the move keeps the copy.
        const SceneEditing sceneEditing = editing(context);
        if (region != KeymapRegion::Viewport) {
            EntityCommands::duplicateSelection(sceneEditing, m_selection);
            break;
        }
        std::string before;
        if (EntityCommands::duplicateSelectionUncommitted(sceneEditing, m_selection, before) &&
            !beginModalTransform(context, TransformOperation::Translate, "Duplicate Objects", before, true)) {
            sceneEditing.commit("Duplicate Objects", {}, std::move(before));
        }
        break;
    }
    case ObjectCommand::DeleteWithConfirmation:
        m_openDeleteMenu = !m_selection.empty();
        break;
    case ObjectCommand::Delete:
        EntityCommands::deleteSelection(editing(context), m_selection);
        break;
    case ObjectCommand::SelectAll: {
        // Tapping A twice quickly deselects everything.
        const double now = ImGui::GetTime();
        if (m_lastSelectAllTime >= 0.0 && now - m_lastSelectAllTime <= ImGui::GetIO().MouseDoubleClickTime) {
            m_selection.clear();
            m_lastSelectAllTime = -1.0;
        } else {
            m_selection.selectAll(context.scene);
            m_lastSelectAllTime = now;
        }
        break;
    }
    case ObjectCommand::DeselectAll:
        m_selection.clear();
        break;
    case ObjectCommand::InvertSelection:
        m_selection.invert(context.scene);
        break;
    case ObjectCommand::AddMenu:
        m_openAddMenu = true;
        break;
    case ObjectCommand::Rename:
        if (const Entity active = m_selection.empty() ? entt::null : context.scene.findEntity(*m_selection.active());
            active != entt::null) {
            m_renameBuffer = context.scene.registry().get<NameComponent>(active).name;
            m_openRenamePopup = true;
            m_focusRenameField = true;
        }
        break;
    case ObjectCommand::Copy:
        copySelection(context);
        break;
    case ObjectCommand::Paste:
        pasteClipboard(context);
        break;
    case ObjectCommand::ToggleSnapping:
        m_viewport.tools().snapping = !m_viewport.tools().snapping;
        break;
    case ObjectCommand::FrameSelected:
        frameEntities(context, m_selection.ids());
        break;
    case ObjectCommand::FrameAll:
        frameAll(context);
        break;
    case ObjectCommand::ViewFront:
        m_camera.setViewAngles(0.0f, 0.0f);
        break;
    case ObjectCommand::ViewBack:
        m_camera.setViewAngles(halfTurn, 0.0f);
        break;
    case ObjectCommand::ViewRight:
        m_camera.setViewAngles(-quarterTurn, 0.0f);
        break;
    case ObjectCommand::ViewLeft:
        m_camera.setViewAngles(quarterTurn, 0.0f);
        break;
    case ObjectCommand::ViewTop:
        m_camera.setViewAngles(0.0f, -quarterTurn);
        break;
    case ObjectCommand::ViewBottom:
        m_camera.setViewAngles(0.0f, quarterTurn);
        break;
    case ObjectCommand::ViewOpposite:
        m_camera.setViewAngles(m_camera.yaw() + halfTurn, -m_camera.pitch());
        break;
    case ObjectCommand::OrbitLeft:
        m_camera.orbitAroundPivot(orbitStepRadians, 0.0f);
        break;
    case ObjectCommand::OrbitRight:
        m_camera.orbitAroundPivot(-orbitStepRadians, 0.0f);
        break;
    case ObjectCommand::OrbitUp:
        m_camera.orbitAroundPivot(0.0f, -orbitStepRadians);
        break;
    case ObjectCommand::OrbitDown:
        m_camera.orbitAroundPivot(0.0f, orbitStepRadians);
        break;
    }
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
            if (ImGui::MenuItem("Cube")) {
                addObject(context, AddableObject::Cube);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Light")) {
            if (ImGui::MenuItem("Sun")) {
                addObject(context, AddableObject::Sun);
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Environment")) {
            addObject(context, AddableObject::Environment);
        }
        if (ImGui::MenuItem("Empty")) {
            addObject(context, AddableObject::Empty);
        }
        ImGui::EndPopup();
    }

    if (m_openDeleteMenu) {
        ImGui::OpenPopup(deleteMenuPopup);
        m_openDeleteMenu = false;
    }
    if (ImGui::BeginPopup(deleteMenuPopup)) {
        if (ImGui::MenuItem("Delete")) {
            EntityCommands::deleteSelection(editing(context), m_selection);
        }
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
            const Entity active = m_selection.empty() ? entt::null : context.scene.findEntity(*m_selection.active());
            if (active != entt::null) {
                const std::string name = m_renameBuffer;
                editing(context).perform("Rename", {}, [&] {
                    context.scene.registry().get<NameComponent>(active).name = name;
                    return SceneResult{.success = true};
                });
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void EditorSession::clearSelectedTransforms(ApplicationContext& context, TransformPart part) {
    if (m_selection.empty()) {
        return;
    }
    const char* name = part == TransformPart::Location
                           ? "Clear Location"
                           : (part == TransformPart::Rotation ? "Clear Rotation" : "Clear Scale");
    const std::vector<SceneEntityId> selection = m_selection.ids();
    editing(context).perform(name, {}, [&] {
        for (SceneEntityId id : selection) {
            if (const Entity entity = context.scene.findEntity(id); entity != entt::null) {
                glm::mat4& local = context.scene.registry().get<TransformComponent>(entity).local;
                local = clearTransformPart(local, part);
                context.scene.markTransformDirty(entity);
            }
        }
        context.scene.updateWorldTransforms();
        return SceneResult{.success = true};
    });
}

void EditorSession::addObject(ApplicationContext& context, AddableObject object) {
    static constexpr std::array names{"Add Cube", "Add Sun", "Add Environment", "Add Empty"};
    SceneEntityId created;
    editing(context).perform(names[static_cast<size_t>(object)], {}, [&]() -> SceneResult {
        Entity entity = entt::null;
        switch (object) {
        case AddableObject::Cube:
            if (SceneResult result = addCube(context.scene, context.assets, m_document.options().assetRoot, entity);
                !result) {
                return result;
            }
            break;
        case AddableObject::Sun:
            entity = addSun(context.scene);
            break;
        case AddableObject::Environment:
            entity = addEnvironment(context.scene);
            break;
        case AddableObject::Empty:
            entity = addEmpty(context.scene);
            break;
        }
        created = context.scene.id(entity);
        return {.success = true};
    });
    if (created) {
        m_selection.selectOnly(created);
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
