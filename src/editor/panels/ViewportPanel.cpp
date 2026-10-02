#include "ViewportPanel.hpp"

#include "editor/interaction/EditorCamera.hpp"
#include "editor/panels/EditorContext.hpp"

#include <imgui.h>

#include <ImGuizmo.h>

#include <algorithm>
#include <cmath>

float& TransformToolSettings::activeSnap() {
    if (gizmo == GizmoMode::Translate) {
        return translationSnap;
    }
    return gizmo == GizmoMode::Rotate ? rotationSnapDegrees : scaleSnap;
}

const char* ViewportPanel::title() const {
    return "Viewport";
}

int ViewportPanel::windowFlags() const {
    return ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
}

TransformToolSettings& ViewportPanel::tools() {
    return m_tools;
}

const TransformToolSettings& ViewportPanel::tools() const {
    return m_tools;
}

glm::vec2 ViewportPanel::position() const {
    return m_position;
}

glm::vec2 ViewportPanel::size() const {
    return m_size;
}

void ViewportPanel::suppressCameraUntilRightRelease() {
    m_suppressCameraUntilRelease = true;
}

void ViewportPanel::update(EditorContext& context, const InputState& input, float deltaSeconds) {
    if (m_suppressCameraUntilRelease && !input.mouseButtonDown(MouseButton::Right)) {
        m_suppressCameraUntilRelease = false;
    }
    const bool cameraInputActive =
        m_inputActive && !context.actions.modalTransformActive() && !m_suppressCameraUntilRelease;
    m_navigating =
        cameraInputActive && (input.mouseButtonDown(MouseButton::Middle) || input.mouseButtonDown(MouseButton::Right));
    context.camera.update(input, deltaSeconds, cameraInputActive);

    RenderRuntime& rendering = context.app.rendering;
    rendering.setViewOverride(context.camera.renderView());
    if (m_showGrid) {
        const float distance = std::max(context.camera.distance(), 0.1f);
        const float spacing = std::pow(10.0f, std::floor(std::log10(distance / 5.0f)));
        rendering.setGridOverlay(GridOverlay{.planeHeight = 0.0f, .spacing = spacing, .fadeDistance = spacing * 40.0f});
    } else {
        rendering.setGridOverlay(std::nullopt);
    }
    updateSelectionOutline(context);
}

void ViewportPanel::updateSelectionOutline(EditorContext& context) {
    // Only a single selection is outlined, together with its descendants.
    m_outlinedObjects.clear();
    if (context.selection.size() == 1) {
        const Scene& scene = context.app.scene;
        const Entity selectedEntity = scene.findEntity(*context.selection.active());
        if (selectedEntity != entt::null) {
            m_outlineTraversal.clear();
            m_outlineTraversal.push_back(selectedEntity);
            while (!m_outlineTraversal.empty()) {
                const Entity entity = m_outlineTraversal.back();
                m_outlineTraversal.pop_back();
                m_outlinedObjects.push_back(static_cast<RenderObjectId>(entt::to_integral(entity)));
                const auto* relationship = scene.registry().try_get<RelationshipComponent>(entity);
                if (relationship != nullptr) {
                    m_outlineTraversal.insert(m_outlineTraversal.end(), relationship->children.begin(),
                                              relationship->children.end());
                }
            }
        }
    }
    context.app.rendering.setOutlinedObjects(m_outlinedObjects);
}

void ViewportPanel::hidden(EditorContext& context) {
    setInputActive(context, false);
}

void ViewportPanel::setInputActive(EditorContext& context, bool active) {
    m_inputActive = active;
    context.app.rendering.setViewportInputActive(active);
}

void ViewportPanel::draw(EditorContext& context) {
    RenderRuntime& rendering = context.app.rendering;
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const uint32_t width = static_cast<uint32_t>(std::max(available.x, 16.0f));
    const uint32_t height = static_cast<uint32_t>(std::max(available.y, 16.0f));
    rendering.requestViewportSize(width, height);
    const UiTextureHandle texture = rendering.viewportTexture();
    if (!texture) {
        ImGui::TextDisabled("Viewport will be available on the next rendered frame.");
        setInputActive(context, false);
        return;
    }

    const ImVec2 imagePosition = ImGui::GetCursorScreenPos();
    ImGui::Image(static_cast<ImTextureID>(texture.value), {static_cast<float>(width), static_cast<float>(height)});
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ZENITH_MODEL_ASSET")) {
            context.placeModel(static_cast<const char*>(payload->Data), entt::null);
        }
        ImGui::EndDragDropTarget();
    }
    const bool imageHovered = ImGui::IsItemHovered();
    const bool imageClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    m_inputActive = imageHovered || m_navigating;
    rendering.setViewportInputActive(m_inputActive && !context.actions.modalTransformActive() &&
                                     !m_suppressCameraUntilRelease);

    m_position = {imagePosition.x, imagePosition.y};
    m_size = {static_cast<float>(width), static_cast<float>(height)};
    const bool toolbarBlocksPicking = drawToolbar();
    if (context.actions.modalTransformActive()) {
        context.actions.updateModalTransform(context.app);
        return;
    }

    const bool gizmoBlocksPicking = drawGizmo(context);
    if (imageHovered) {
        context.actions.handleObjectKeys(context.app, KeymapRegion::Viewport);
    }
    if (imageClicked && !toolbarBlocksPicking && !gizmoBlocksPicking && !context.actions.modalTransformActive()) {
        pickAtMouse(context);
    }
    drawActiveName(context);
}

void ViewportPanel::pickAtMouse(EditorContext& context) {
    const ImVec2 mouse = ImGui::GetMousePos();
    const glm::vec2 normalized = (glm::vec2(mouse.x, mouse.y) - m_position) / m_size;
    Scene& scene = context.app.scene;
    scene.updateWorldTransforms();
    const Entity picked = context.camera.pick(scene, context.app.assets, normalized, m_size.x / m_size.y);
    if (picked == entt::null) {
        context.selection.clear();
    } else {
        context.selection.click(scene.id(picked),
                                ImGui::GetIO().KeyShift ? SelectionClickMode::Extend : SelectionClickMode::Replace);
    }
}

void ViewportPanel::drawActiveName(EditorContext& context) const {
    const std::optional<SceneEntityId> active = context.selection.active();
    if (!active) {
        return;
    }
    const Entity selectedEntity = context.app.scene.findEntity(*active);
    if (selectedEntity == entt::null) {
        return;
    }
    const std::string& name = context.app.scene.registry().get<NameComponent>(selectedEntity).name;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 labelPosition{m_position.x + 8.0f, m_position.y + 8.0f};
    const ImVec2 labelSize = ImGui::CalcTextSize(name.c_str());
    drawList->AddRectFilled({labelPosition.x - 4.0f, labelPosition.y - 2.0f},
                            {labelPosition.x + labelSize.x + 4.0f, labelPosition.y + labelSize.y + 2.0f},
                            IM_COL32(20, 20, 24, 210), 3.0f);
    drawList->AddText(labelPosition, IM_COL32(255, 190, 60, 255), name.c_str());
}

bool ViewportPanel::drawToolbar() {
    ImGui::SetCursorScreenPos({m_position.x + m_size.x * 0.5f - 200.0f, m_position.y + 8.0f});
    ImGui::BeginGroup();

    constexpr const char* modeNames[] = {"Tweak", "Move", "Rotate", "Scale"};
    constexpr GizmoMode modes[] = {GizmoMode::Select, GizmoMode::Translate, GizmoMode::Rotate, GizmoMode::Scale};
    for (int index = 0; index < 4; ++index) {
        const bool isActive = m_tools.gizmo == modes[index];
        if (isActive) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
        }
        if (ImGui::Button(modeNames[index], ImVec2(90, 0))) {
            m_tools.gizmo = modes[index];
        }
        if (isActive) {
            ImGui::PopStyleColor(3);
        }
        ImGui::SameLine(0, 4.0f);
    }

    ImGui::Dummy(ImVec2(10, 0));
    ImGui::SameLine();

    constexpr const char* spaceNames[] = {"Global", "Local"};
    if (ImGui::Button(spaceNames[static_cast<int>(m_tools.space)], ImVec2(70, 0))) {
        m_tools.space = m_tools.space == TransformSpace::Global ? TransformSpace::Local : TransformSpace::Global;
    }
    ImGui::SameLine(0, 4.0f);
    ImGui::Checkbox("Snap", &m_tools.snapping);
    ImGui::SameLine(0, 4.0f);
    ImGui::SetNextItemWidth(70.0f);
    ImGui::DragFloat("##SnapValue", &m_tools.activeSnap(), 0.1f, 0.001f, 1000.0f, "%.3f");
    ImGui::SameLine(0, 8.0f);
    ImGui::Checkbox("Grid", &m_showGrid);

    ImGui::EndGroup();
    return ImGui::IsItemHovered();
}

bool ViewportPanel::drawGizmo(EditorContext& context) {
    if (m_tools.gizmo == GizmoMode::Select || context.selection.size() != 1) {
        m_gizmoWasUsing = false;
        m_gizmoBefore.clear();
        return false;
    }
    Scene& scene = context.app.scene;
    const Entity entity = scene.findEntity(*context.selection.active());
    if (entity == entt::null) {
        return false;
    }
    entt::registry& registry = scene.registry();
    scene.updateWorldTransforms();
    glm::mat4 world = registry.get<TransformComponent>(entity).world;
    const glm::mat4 view = context.camera.viewMatrix();
    const glm::mat4 projection = context.camera.gizmoProjectionMatrix(m_size.x / m_size.y);
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(m_position.x, m_position.y, m_size.x, m_size.y);
    ImGuizmo::SetOrthographic(false);
    const ImGuizmo::OPERATION operation =
        m_tools.gizmo == GizmoMode::Translate
            ? ImGuizmo::TRANSLATE
            : (m_tools.gizmo == GizmoMode::Rotate ? ImGuizmo::ROTATE : ImGuizmo::SCALE);
    const ImGuizmo::MODE mode = m_tools.space == TransformSpace::Local ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    const float snapValue = m_tools.activeSnap();
    const float snap[3]{snapValue, snapValue, snapValue};
    const glm::mat4 localBeforeManipulation = registry.get<TransformComponent>(entity).local;
    const bool changed = ImGuizmo::Manipulate(&view[0][0], &projection[0][0], operation, mode, &world[0][0], nullptr,
                                              m_tools.snapping ? snap : nullptr);
    if (changed) {
        const Entity parent = registry.get<RelationshipComponent>(entity).parent;
        registry.get<TransformComponent>(entity).local =
            parent == entt::null ? world : glm::inverse(registry.get<TransformComponent>(parent).world) * world;
        scene.markTransformDirty(entity);
        scene.updateWorldTransforms();
    }
    const bool usingGizmo = ImGuizmo::IsUsing();
    if (usingGizmo && !m_gizmoWasUsing) {
        // The first manipulated frame has already moved the entity, so the
        // undo snapshot is taken with the pre-drag transform restored.
        const glm::mat4 editedLocal = registry.get<TransformComponent>(entity).local;
        registry.get<TransformComponent>(entity).local = localBeforeManipulation;
        scene.markTransformDirty(entity);
        scene.updateWorldTransforms();
        if (!context.editing.capture(m_gizmoBefore)) {
            m_gizmoBefore.clear();
        }
        registry.get<TransformComponent>(entity).local = editedLocal;
        scene.markTransformDirty(entity);
        scene.updateWorldTransforms();
    } else if (!usingGizmo && m_gizmoWasUsing && !m_gizmoBefore.empty()) {
        context.editing.commit("Transform Gizmo", {}, std::move(m_gizmoBefore));
        m_gizmoBefore.clear();
    }
    m_gizmoWasUsing = usingGizmo;
    return usingGizmo || ImGuizmo::IsOver();
}
