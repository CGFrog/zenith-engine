// Blender object-mode operators that need the running editor: modal G/R/S
// transforms and the Delete/Rename popups, and drawing the menu opened with
// wm.call_menu (the Add menu is defined in editor/menus). Operators that only
// edit the document live in editor/operators/ObjectOperators, and the view3d
// ones in editor/panels/ViewportOperators.

#include "EditorSession.hpp"

#include "editor/application/EditorSessionOperators.hpp"
#include "editor/application/EntityCommands.hpp"
#include "editor/operators/ObjectOperators.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/ThemeChrome.hpp"
#include "editor/viewport/SnapQueries.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <array>
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
} // namespace

void EditorSession::handleObjectKeys(ApplicationContext& context, KeymapRegion region) {
    handleObjectKeysOn(context, nullptr, region);
}

void EditorSession::handleObjectKeysOn(ApplicationContext& context, PrefabWindowDocument* window, KeymapRegion region) {
    // A prefab window's document stays editable while the level plays.
    const bool documentPlaying = window == nullptr && m_playMode.mode() != EditorMode::Edit;
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || documentPlaying || m_activeTransform || m_fileDialog.pending() ||
        m_keymapEditor.capturing() || ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
        ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        return;
    }
    dispatchKeymap(context, window, region);
}

OperatorRegistrationResult registerObjectModeOperatorFamily(OperatorRegistry& registry,
                                                            const EditorOperatorServices& services) {
    EditorObjectModeCommands& objectMode = services.objectModeCommands;
    ViewportRegistry& viewports = services.viewports;
    const auto canTransform = [&objectMode, &viewports](const OperatorContext& context) {
        // A modal transform follows the mouse over the active viewport, which
        // must show the document the selection is in.
        return context.app != nullptr && context.editMode() && !context.selection.empty() &&
               !objectMode.modalTransformRunning() && viewports.active() != nullptr &&
               objectMode.viewportsShowDocument(context);
    };
    const auto modalTransform = [&objectMode, canTransform](std::string id, std::string label, std::string description,
                                                            TransformOperation operation, std::string commandName) {
        return Operator{
            .id = std::move(id),
            .label = std::move(label),
            .description = std::move(description),
            .poll = canTransform,
            .execute = [&objectMode, operation, commandName](OperatorContext& context, OperatorProperties&) {
                return objectMode.beginModalTransform(context, operation, commandName, {}, false)
                           ? OperatorResult::finished()
                           : OperatorResult::cancelled("Nothing to transform in an open viewport");
            }};
    };
    std::vector<Operator> operators;
    operators.push_back(modalTransform("transform.translate", "Move", "Move the selection with the mouse",
                                       TransformOperation::Translate, "Move"));
    operators.push_back(modalTransform("transform.rotate", "Rotate", "Rotate the selection with the mouse",
                                       TransformOperation::Rotate, "Rotate"));
    operators.push_back(modalTransform("transform.resize", "Scale", "Scale the selection with the mouse",
                                       TransformOperation::Scale, "Resize"));
    // The copy follows the mouse; the copy and its move form one undo step,
    // and cancelling the move keeps the copy.
    operators.push_back(
        {.id = "object.duplicate_move",
         .label = "Duplicate and Move",
         .description = "Duplicate the selection and move the copies with the mouse",
         .poll = canTransform,
         .execute = [&objectMode](OperatorContext& context, OperatorProperties&) {
             SceneEditing::EntitySnapshot before;
             if (!EntityCommands::duplicateSelectionUncommitted(context.editing, context.selection, before)) {
                 return OperatorResult::cancelled("Nothing was duplicated");
             }
             if (!objectMode.beginModalTransform(context, TransformOperation::Translate, "Duplicate Objects",
                                                 std::optional(before), true)) {
                 context.editing.commit("Duplicate Objects", {}, std::move(before));
             }
             return OperatorResult::finished();
         }});

    operators.push_back(applicationOperator(
        "object.delete_menu", "Delete...", "Ask before deleting the selected objects",
        [](const OperatorContext& context) { return context.editMode() && !context.selection.empty(); },
        [&objectMode](ApplicationContext&) { objectMode.openDeleteMenu(); }));
    operators.push_back({.id = "object.rename_popup",
                         .label = "Rename...",
                         .description = "Type a new name for the active object",
                         .poll =
                             [](const OperatorContext& context) {
                                 return context.app != nullptr && context.editMode() && !context.selection.empty() &&
                                        context.editing.scene().findEntity(*context.selection.active()) != entt::null;
                             },
                         .execute =
                             [&objectMode](OperatorContext& context, OperatorProperties&) {
                                 objectMode.openRenamePopup(context);
                                 return OperatorResult::finished();
                             }});
    return addOperators(registry, std::move(operators));
}

bool EditorSession::modalTransformRunning() const {
    return m_activeTransform.has_value();
}

void EditorSession::openDeleteMenu() {
    m_openDeleteMenu = true;
}

bool EditorSession::viewportsShowDocument(const OperatorContext& context) const {
    const ViewportPanel* viewport = m_viewports.active();
    return viewport != nullptr && viewport->showsDocument(context.editing.document());
}

void EditorSession::openRenamePopup(OperatorContext& context) {
    const Scene& scene = context.editing.scene();
    const Entity active = scene.findEntity(*context.selection.active());
    m_renameBuffer = scene.registry().get<NameComponent>(active).name;
    m_openRenamePopup = true;
    m_focusRenameField = true;
}

void EditorSession::drawObjectPopups(ApplicationContext& context) {
    // The Add menu and any other menu opened with wm.call_menu, on the
    // document whose window opened it (the one last focused).
    withFocusedEditorContext(context, [this](EditorContext& editor) { m_menus.draw(editor); });

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
                OperatorContext operatorContext = this->operatorContext(context, KeymapRegion::Global);
                m_operatorRunner.invoke("object.rename", operatorContext, &properties);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

bool EditorSession::beginModalTransform(OperatorContext& context, TransformOperation operation, std::string commandName,
                                        std::optional<SceneEditing::EntitySnapshot> before, bool commitOnCancel) {
    const ViewportPanel* viewport = m_viewports.active();
    const PrefabWindowDocument* window = m_prefabWindows.owner(context.editing.document());
    // The level transforms only in Edit mode; a prefab window's document
    // always is in Edit mode.
    const bool levelPlaying = window == nullptr && m_playMode.mode() != EditorMode::Edit;
    if (levelPlaying || context.selection.empty() || viewport == nullptr ||
        !viewport->showsDocument(context.editing.document())) {
        return false;
    }
    Scene& scene = context.editing.scene();
    const Selection& selection = context.selection;
    scene.updateWorldTransforms();
    std::vector<SceneEntityId> roots = selection.transformRoots(scene);
    if (roots.empty()) {
        return false;
    }
    // Only the transform roots' local transforms change.
    if (!before) {
        before.emplace();
        if (!context.editing.captureEntities(roots, SceneEditing::CaptureReach::Entities,
                                             SceneEditing::CaptureCollections::Exclude, *before)) {
            return false;
        }
    }
    std::vector<glm::mat4> startWorld;
    std::vector<glm::mat4> startLocal;
    for (SceneEntityId id : roots) {
        const auto& transform = scene.registry().get<TransformComponent>(scene.findEntity(id));
        startWorld.push_back(transform.world);
        startLocal.push_back(transform.local);
    }
    glm::mat3 localAxes(1.0f);
    ModalTransformPivot pivot{.mode = viewport->tools().pivot,
                              .cursor = context.editing.document().editorData().cursorLocation};
    if (const Entity active = scene.findEntity(*selection.active()); active != entt::null) {
        const glm::mat4& activeWorld = scene.registry().get<TransformComponent>(active).world;
        localAxes = normalizedAxes(activeWorld);
        pivot.activeOrigin = glm::vec3(activeWorld[3]);
    }
    // Projected through whatever the viewport shows, its editor camera or a
    // scene camera it looks through.
    const glm::vec2 viewportSize = viewport->size();
    const float aspectRatio = viewportSize.x / std::max(viewportSize.y, 1.0f);
    const RenderView lens = viewport->lens(scene);
    const ModalTransformView view{.view = lensViewMatrix(lens),
                                  .projection = lensProjectionMatrix(lens, aspectRatio),
                                  .viewportPosition = viewport->position(),
                                  .viewportSize = viewportSize};
    const ImVec2 mouse = ImGui::GetMousePos();
    m_activeTransform.emplace(ActiveTransform{
        .transform = ModalTransform(operation, std::move(startWorld), localAxes, {mouse.x, mouse.y}, view, pivot),
        .viewport = viewport->id(),
        .window = window != nullptr ? window->id : 0,
        .entities = std::move(roots),
        .startLocal = std::move(startLocal),
        .before = std::move(*before),
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
    Scene* target = modalTransformScene(context);
    if (viewport == nullptr || target == nullptr) {
        finishModalTransform(context, false);
        return;
    }
    Scene& scene = *target;
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
    TransformSnapping snapping = transformSnapping(viewport->tools(), io.KeyCtrl);
    if (snapping.enabled && snapping.target != SnapTarget::Increment &&
        transform.operation() == TransformOperation::Translate) {
        // The moved objects are where the mouse is, so they never snap to themselves.
        const std::vector<Entity> excluded = snapExclusions(scene, m_activeTransform->entities);
        snapping.targetPoint =
            snapTargetPoint(snapping.target, scene, context.assets, viewport->snapView(scene),
                            glm::vec2(io.MousePos.x, io.MousePos.y) - viewport->position(), excluded);
    }
    const ModalTransformResult result = transform.evaluate(snapping);
    const std::vector<SceneEntityId>& entities = m_activeTransform->entities;
    for (size_t index = 0; index < entities.size() && index < result.world.size(); ++index) {
        const Entity entity = scene.findEntity(entities[index]);
        if (entity == entt::null) {
            continue;
        }
        const Entity parent = scene.registry().get<RelationshipComponent>(entity).parent;
        scene.registry().get<TransformComponent>(entity).local =
            parent == entt::null
                ? result.world[index]
                : glm::inverse(scene.registry().get<TransformComponent>(parent).world) * result.world[index];
        scene.markTransformDirty(entity);
    }
    if (m_activeTransform->window == 0) {
        // A prefab window's scene updates once per frame before it renders.
        scene.updateWorldTransforms();
    }
    drawModalTransformOverlay(*viewport, result);
}

Scene* EditorSession::modalTransformScene(ApplicationContext& context) {
    if (!m_activeTransform || m_activeTransform->window == 0) {
        return &context.scene;
    }
    PrefabWindowDocument* window = m_prefabWindows.find(m_activeTransform->window);
    return window != nullptr ? &window->scene : nullptr;
}

std::optional<SceneEditing> EditorSession::modalTransformEditing(ApplicationContext& context) {
    if (!m_activeTransform || m_activeTransform->window == 0) {
        return editing(context);
    }
    PrefabWindowDocument* window = m_prefabWindows.find(m_activeTransform->window);
    if (window == nullptr) {
        return std::nullopt;
    }
    return SceneEditing(window->scene, context.assets, window->document, window->history, m_diagnostics);
}

void EditorSession::finishModalTransform(ApplicationContext& context, bool confirm) {
    Scene* scene = modalTransformScene(context);
    std::optional<SceneEditing> editing = modalTransformEditing(context);
    ActiveTransform active = std::move(*m_activeTransform);
    m_activeTransform.reset();
    m_modalTransformFinishedThisFrame = true;
    if (scene == nullptr || !editing) {
        // Its prefab window closed: nothing left to restore or record.
        return;
    }
    if (!confirm) {
        for (size_t index = 0; index < active.entities.size(); ++index) {
            if (const Entity entity = scene->findEntity(active.entities[index]); entity != entt::null) {
                scene->registry().get<TransformComponent>(entity).local = active.startLocal[index];
                scene->markTransformDirty(entity);
            }
        }
        if (active.window == 0) {
            scene->updateWorldTransforms();
        }
    }
    if (confirm || active.commitOnCancel) {
        editing->commit(std::move(active.commandName), {}, std::move(active.before));
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

    // A ring marks the surface or vertex the move snapped to.
    if (result.snappedTo) {
        const glm::vec2 snapped = transform.toScreen(*result.snappedTo);
        drawList->AddCircle({snapped.x, snapped.y}, 6.0f, toImU32(palette::snapIndicator), 12, 2.0f);
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
    ThemeChrome::paintOverlayPanel(
        drawList, {position.x - 6.0f, position.y - 4.0f},
        {position.x + std::max(descriptionSize.x, hintSize.x) + 6.0f, position.y + lineHeight + hintSize.y + 4.0f});
    drawList->AddText(position, ImGui::GetColorU32(ImGuiCol_Text), result.description.c_str());
    drawList->AddText({position.x, position.y + lineHeight}, ImGui::GetColorU32(ImGuiCol_TextDisabled), hint);
    drawList->PopClipRect();
}
