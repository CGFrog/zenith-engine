// Blender object-mode operators that need the running editor: modal G/R/S
// transforms and the Delete/Rename popups, and drawing the menu opened with
// wm.call_menu (the Add menu is defined in editor/menus). Operators that only
// edit the document live in editor/operators/ObjectOperators, and the view3d
// ones in editor/panels/ViewportOperators.

#include "EditorSession.hpp"

#include "editor/application/EntityCommands.hpp"
#include "editor/operators/ObjectOperators.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/ThemeChrome.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <optional>
#include <utility>

namespace {
constexpr const char* deleteMenuPopup = "ObjectDeleteMenu";
constexpr const char* renamePopup = "ObjectRename";

ImU32 axisColor(TransformAxis axis) {
    switch (axis) {
    case TransformAxis::X:
        return toImU32(palette::axisX);
    case TransformAxis::Y:
        return toImU32(palette::axisY);
    case TransformAxis::Z:
        return toImU32(palette::axisZ);
    }
    return toImU32(palette::guideLine);
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
        // A modal transform follows the mouse over the active viewport.
        return context.app != nullptr && context.editMode() && !context.selection.empty() && !m_activeTransform &&
               m_viewports.active() != nullptr;
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
                                       : OperatorResult::cancelled("Nothing to transform in an open viewport");
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

    registerOperator(applicationOperator(
        "object.delete_menu", "Delete...", "Ask before deleting the selected objects",
        [](const OperatorContext& context) { return context.editMode() && !context.selection.empty(); },
        [this](ApplicationContext&) { m_openDeleteMenu = true; }));
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
}

void EditorSession::drawObjectPopups(ApplicationContext& context) {
    // The Add menu and any other menu opened with wm.call_menu.
    EditorContext editor = editorContext(context);
    m_menus.draw(editor);

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

bool EditorSession::beginModalTransform(ApplicationContext& context, TransformOperation operation,
                                        std::string commandName, std::string before, bool commitOnCancel) {
    const ViewportPanel* viewport = m_viewports.active();
    if (m_playMode.mode() != EditorMode::Edit || m_selection.empty() || viewport == nullptr) {
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
    // Projected through whatever the viewport shows, its editor camera or a
    // scene camera it looks through.
    const glm::vec2 viewportSize = viewport->size();
    const float aspectRatio = viewportSize.x / std::max(viewportSize.y, 1.0f);
    const RenderView lens = viewport->lens(context.scene);
    const ModalTransformView view{.view = lensViewMatrix(lens),
                                  .projection = lensProjectionMatrix(lens, aspectRatio),
                                  .viewportPosition = viewport->position(),
                                  .viewportSize = viewportSize};
    const ImVec2 mouse = ImGui::GetMousePos();
    m_activeTransform.emplace(ActiveTransform{
        .transform = ModalTransform(operation, std::move(startWorld), localAxes, {mouse.x, mouse.y}, view),
        .viewport = viewport->id(),
        .entities = std::move(roots),
        .startLocal = std::move(startLocal),
        .before = std::move(before),
        .commandName = std::move(commandName),
        .commitOnCancel = commitOnCancel});
    return true;
}

bool EditorSession::modalTransformActive() const {
    // Through the end of the frame that finished it, so the confirming click
    // does not also pick or grab a gizmo in a viewport drawn later.
    return m_activeTransform.has_value() || m_modalTransformFinishedThisFrame;
}

void EditorSession::updateModalTransform(ApplicationContext& context, ViewportId viewportId) {
    if (!m_activeTransform || m_activeTransform->viewport != viewportId) {
        return;
    }
    ViewportPanel* viewport = m_viewports.find(viewportId);
    if (viewport == nullptr) {
        finishModalTransform(context, false);
        return;
    }
    ModalTransform& transform = m_activeTransform->transform;
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            // The cancelling click may be over any viewport.
            for (ViewportPanel* open : m_viewports.openViewports()) {
                open->suppressCameraUntilRightRelease();
            }
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
    const TransformToolSettings& tools = viewport->tools();
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
    drawModalTransformOverlay(*viewport, result);
}

void EditorSession::finishModalTransform(ApplicationContext& context, bool confirm) {
    ActiveTransform active = std::move(*m_activeTransform);
    m_activeTransform.reset();
    m_modalTransformFinishedThisFrame = true;
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

void EditorSession::drawModalTransformOverlay(const ViewportPanel& viewport, const ModalTransformResult& result) const {
    const ModalTransform& transform = m_activeTransform->transform;
    const glm::vec2 viewportPosition = viewport.position();
    const glm::vec2 viewportSize = viewport.size();
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
            drawList->AddLine({from.x, from.y}, {to.x, to.y}, toImU32(palette::guideLine), 1.5f);
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
    ThemeChrome::paintSmokedGlass(
        drawList, {position.x - 6.0f, position.y - 4.0f},
        {position.x + std::max(descriptionSize.x, hintSize.x) + 6.0f, position.y + lineHeight + hintSize.y + 4.0f});
    drawList->AddText(position, ImGui::GetColorU32(ImGuiCol_Text), result.description.c_str());
    drawList->AddText({position.x, position.y + lineHeight}, ImGui::GetColorU32(ImGuiCol_TextDisabled), hint);
    drawList->PopClipRect();
}

bool EditorSession::updateSmokeMenus(ApplicationContext& context) {
    // Frames 30-32: open the Add menu with wm.call_menu, choose Empty from its
    // search as typing "empty" would, check the entity was added and
    // selected, then undo it.
    constexpr uint32_t firstFrame = 30;
    const auto fail = [&](const std::string& message) {
        const std::string error = "Menu smoke failed: " + message;
        std::cerr << error << '\n';
        m_diagnostics.reportError(error);
        context.control.requestSceneChange({std::numeric_limits<uint32_t>::max()});
    };
    const auto entityCount = [&context] {
        return context.scene.registry().view<const SceneEntityIdComponent>().size();
    };
    if (m_smokeFrame == firstFrame) {
        m_smokeEntityCount = entityCount();
        invokeOperator(context, "wm.call_menu", nlohmann::json{{"menu", addMenuId}}.dump(), KeymapRegion::Viewport);
    } else if (m_smokeFrame == firstFrame + 1) {
        const std::optional<ResolvedMenu> menu =
            m_menus.resolve(m_menus.request(), operatorContext(context, KeymapRegion::Global));
        const std::vector<MenuSearchResult> results =
            menu ? searchMenu(*menu, "empty") : std::vector<MenuSearchResult>{};
        if (!m_menus.isOpen() || results.empty()) {
            fail("wm.call_menu did not open an Add menu offering Empty");
            return true;
        }
        invokeOperator(context, results.front().item->operatorId, results.front().item->properties,
                       KeymapRegion::Global);
        m_menus.close();
        if (entityCount() != m_smokeEntityCount + 1 || m_selection.size() != 1) {
            fail("choosing Empty did not add and select one entity");
        }
    } else if (m_smokeFrame == firstFrame + 2) {
        if (m_menus.isOpen()) {
            fail("the Add menu did not close");
        }
        undo(context);
        if (entityCount() != m_smokeEntityCount) {
            fail("undo did not remove the added entity");
        }
    }
    return m_smokeFrame > firstFrame + 2;
}
