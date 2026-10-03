#include "ViewportPanel.hpp"

#include "editor/interaction/ComponentEditors.hpp"
#include "editor/interaction/EditorCamera.hpp"
#include "editor/panels/EditorContext.hpp"

#include <imgui.h>

#include <ImGuizmo.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
// Built-in sidebar tabs; added tabs follow them.
constexpr const char* itemTabTitle = "Item";
constexpr const char* toolTabTitle = "Tool";
constexpr const char* viewTabTitle = "View";
constexpr float chromeMargin = 4.0f;
constexpr ImU32 chromeBackground = IM_COL32(28, 28, 32, 225);

// Wave 2 (engine view shading): map the viewport's shading choice to the
// renderer here, as RenderSettings::viewShading, once the engine has shading
// modes. This is the only place the choice reaches the engine; until then
// every choice renders fully shaded.
void applyViewportShading(RenderRuntime&, ViewportShading) {}

bool isBuiltInSidebarTab(std::string_view title) {
    return title == itemTabTitle || title == toolTabTitle || title == viewTabTitle;
}
} // namespace

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

ViewportChromeState& ViewportPanel::chrome() {
    return m_chrome;
}

const ViewportChromeState& ViewportPanel::chrome() const {
    return m_chrome;
}

std::vector<Operator> ViewportPanel::chromeOperators() {
    return viewportChromeOperators(m_chrome);
}

ViewportSidebarTabResult ViewportPanel::addSidebarTab(ViewportSidebarTab tab) {
    if (tab.title.empty()) {
        return {.error = "A viewport sidebar tab needs a title"};
    }
    if (!tab.draw) {
        return {.error = "Viewport sidebar tab '" + tab.title + "' needs a draw function"};
    }
    const bool duplicate = std::ranges::any_of(
        m_sidebarTabs, [&](const ViewportSidebarTab& existing) { return existing.title == tab.title; });
    if (duplicate || isBuiltInSidebarTab(tab.title)) {
        return {.error = "The viewport sidebar already has a tab named '" + tab.title + "'"};
    }
    m_sidebarTabs.push_back(std::move(tab));
    return {.success = true};
}

bool ViewportPanel::removeSidebarTab(std::string_view title) {
    return std::erase_if(m_sidebarTabs, [&](const ViewportSidebarTab& tab) { return tab.title == title; }) != 0;
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
    applyViewportShading(rendering, m_chrome.shading);
    if (m_chrome.overlays.grid) {
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
    if (m_chrome.overlays.selectionOutline && context.selection.size() == 1) {
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

    // The image area is laid out without a texture too, so the chrome (and
    // headless tests of it) does not depend on a rendered frame.
    const ImVec2 imagePosition = ImGui::GetCursorScreenPos();
    m_position = {imagePosition.x, imagePosition.y};
    m_size = {static_cast<float>(width), static_cast<float>(height)};
    ImGui::Dummy({m_size.x, m_size.y});
    drawViewImage(texture);
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ZENITH_MODEL_ASSET")) {
            context.placeModel(static_cast<const char*>(payload->Data), entt::null);
        }
        ImGui::EndDragDropTarget();
    }
    // The chrome is drawn in child windows, so the image reports no hover or
    // click while the mouse is over the header, tool shelf, or sidebar.
    const bool imageHovered = ImGui::IsItemHovered();
    const bool imageClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    m_inputActive = texture && (imageHovered || m_navigating);
    rendering.setViewportInputActive(m_inputActive && !context.actions.modalTransformActive() &&
                                     !m_suppressCameraUntilRelease);

    const bool chromeHovered = drawChrome(context);
    if (context.actions.modalTransformActive()) {
        context.actions.updateModalTransform(context.app);
        return;
    }

    const bool gizmoBlocksPicking = m_chrome.overlays.gizmo && drawGizmo(context);
    if (imageHovered || chromeHovered) {
        context.actions.handleObjectKeys(context.app, KeymapRegion::Viewport);
    }
    if (imageClicked && !gizmoBlocksPicking && !context.actions.modalTransformActive()) {
        pickAtMouse(context);
    }
    if (m_chrome.overlays.activeName) {
        drawActiveName(context);
    }
}

void ViewportPanel::drawViewImage(UiTextureHandle texture) const {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 minimum{m_position.x, m_position.y};
    const ImVec2 maximum{m_position.x + m_size.x, m_position.y + m_size.y};
    if (texture) {
        drawList->AddImage(static_cast<ImTextureID>(texture.value), minimum, maximum);
        return;
    }
    constexpr const char* waiting = "Viewport will be available on the next rendered frame.";
    const ImVec2 textSize = ImGui::CalcTextSize(waiting);
    drawList->AddText({minimum.x + (m_size.x - textSize.x) * 0.5f, minimum.y + (m_size.y - textSize.y) * 0.5f},
                      ImGui::GetColorU32(ImGuiCol_TextDisabled), waiting);
}

bool ViewportPanel::drawChrome(EditorContext& context) {
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const float headerHeight = ImGui::GetFrameHeight();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, chromeBackground);
    ImGui::PushStyleColor(ImGuiCol_MenuBarBg, chromeBackground);
    bool hovered = drawHeader(context);
    m_labelOrigin = {m_position.x + 2.0f * chromeMargin, m_position.y + headerHeight + 2.0f * chromeMargin};
    if (m_chrome.toolbarVisible) {
        hovered = drawToolShelf(headerHeight) || hovered;
    }
    if (m_chrome.sidebarVisible) {
        hovered = drawSidebar(context, headerHeight) || hovered;
    }
    ImGui::PopStyleColor(2);
    ImGui::SetCursorScreenPos(cursor);
    return hovered;
}

bool ViewportPanel::drawHeader(EditorContext& context) {
    ImGui::SetCursorScreenPos({m_position.x, m_position.y});
    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    bool hovered = false;
    if (ImGui::BeginChild("##ViewportHeader", {m_size.x, ImGui::GetFrameHeight()}, ImGuiChildFlags_None, flags)) {
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("View")) {
                const std::string sidebarShortcut = context.shortcut("view3d.toggle_sidebar");
                const std::string toolbarShortcut = context.shortcut("view3d.toggle_toolbar");
                if (ImGui::MenuItem("Sidebar", sidebarShortcut.c_str(), m_chrome.sidebarVisible)) {
                    context.invoke("view3d.toggle_sidebar");
                }
                if (ImGui::MenuItem("Toolbar", toolbarShortcut.c_str(), m_chrome.toolbarVisible)) {
                    context.invoke("view3d.toggle_toolbar");
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Shading")) {
                ViewportChromeWidgets::drawShadingMenu(m_chrome);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Overlays")) {
                ViewportChromeWidgets::drawOverlaysMenu(m_chrome.overlays);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Gizmos")) {
                ViewportChromeWidgets::drawGizmosMenu(m_tools);
                ImGui::EndMenu();
            }
            ImGui::TextDisabled("%s", viewportShadingLabel(m_chrome.shading));
            ImGui::EndMenuBar();
        }
        hovered = ImGui::IsWindowHovered();
    }
    ImGui::EndChild();
    return hovered;
}

bool ViewportPanel::drawToolShelf(float headerHeight) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float buttonWidth = ImGui::CalcTextSize("Rotate").x + style.FramePadding.x * 2.0f;
    const ImVec2 shelfSize{buttonWidth + style.WindowPadding.x * 2.0f,
                           ImGui::GetFrameHeight() * 4.0f + style.ItemSpacing.y * 3.0f + style.WindowPadding.y * 2.0f};
    ImGui::SetCursorScreenPos({m_position.x + chromeMargin, m_position.y + headerHeight + chromeMargin});
    bool hovered = false;
    if (ImGui::BeginChild("##ViewportToolShelf", shelfSize, ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ViewportChromeWidgets::drawToolShelf(m_tools, buttonWidth);
        hovered = ImGui::IsWindowHovered();
    }
    ImGui::EndChild();
    m_labelOrigin.x = m_position.x + shelfSize.x + 3.0f * chromeMargin;
    return hovered;
}

bool ViewportPanel::drawSidebar(EditorContext& context, float headerHeight) {
    const float width = std::min(ImGui::GetFontSize() * 18.0f, m_size.x * 0.5f);
    const ImVec2 sidebarSize{width, std::max(m_size.y - headerHeight, 1.0f)};
    ImGui::SetCursorScreenPos({m_position.x + m_size.x - width, m_position.y + headerHeight});
    bool hovered = false;
    if (ImGui::BeginChild("##ViewportSidebar", sidebarSize, ImGuiChildFlags_None)) {
        if (ImGui::BeginTabBar("##ViewportSidebarTabs")) {
            if (ImGui::BeginTabItem(itemTabTitle)) {
                drawItemTab(context);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(toolTabTitle)) {
                ViewportChromeWidgets::drawToolTab(m_tools);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(viewTabTitle)) {
                ViewportChromeWidgets::drawViewTab(context.camera);
                ImGui::EndTabItem();
            }
            for (const ViewportSidebarTab& tab : m_sidebarTabs) {
                if (ImGui::BeginTabItem(tab.title.c_str())) {
                    tab.draw(context);
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }
        hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    }
    ImGui::EndChild();
    return hovered;
}

void ViewportPanel::drawItemTab(EditorContext& context) {
    const std::optional<SceneEntityId> active = context.selection.active();
    Scene& scene = context.app.scene;
    const Entity entity = active ? scene.findEntity(*active) : entt::null;
    if (entity == entt::null) {
        ImGui::TextDisabled("No active object.");
        return;
    }
    ImGui::TextUnformatted(scene.registry().get<NameComponent>(entity).name.c_str());
    ImGui::Separator();
    // Play mode edits a throwaway copy of the scene, so only Edit mode edits.
    ImGui::BeginDisabled(!context.editMode());
    TransformComponent& transform = scene.registry().get<TransformComponent>(entity);
    const TransformComponent original = transform;
    if (ComponentEditors::drawTransform(transform)) {
        // The widget edits in place; the undo snapshot needs the unedited value.
        const TransformComponent edited = transform;
        transform = original;
        std::string before;
        if (context.editing.capture(before)) {
            transform = edited;
            scene.markTransformDirty(entity);
            // Shares the inspector's key so a drag in either merges into one step.
            context.editing.commit("Edit Transform", "transform:" + std::to_string(active->value), std::move(before));
        }
    }
    ImGui::EndDisabled();
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
    const ImVec2 labelPosition{m_labelOrigin.x, m_labelOrigin.y};
    const ImVec2 labelSize = ImGui::CalcTextSize(name.c_str());
    drawList->AddRectFilled({labelPosition.x - 4.0f, labelPosition.y - 2.0f},
                            {labelPosition.x + labelSize.x + 4.0f, labelPosition.y + labelSize.y + 2.0f},
                            IM_COL32(20, 20, 24, 210), 3.0f);
    drawList->AddText(labelPosition, IM_COL32(255, 190, 60, 255), name.c_str());
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
