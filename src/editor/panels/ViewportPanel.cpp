#include "ViewportPanel.hpp"

#include "editor/interaction/ComponentEditors.hpp"
#include "editor/panels/AssetDragPayloads.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/EditorWidgets.hpp"
#include "editor/theme/ThemeChrome.hpp"
#include "engine/graphics/ViewShading.hpp"

#include <imgui.h>

#include <ImGuizmo.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
constexpr float chromeMargin = 4.0f;

// Smoked glass behind the current chrome child window, which has no
// background of its own.
void paintChildGlass() {
    const ImVec2 min = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    ThemeChrome::paintSmokedGlass(ImGui::GetWindowDrawList(), min, {min.x + size.x, min.y + size.y});
}

const char* entityName(const Scene& scene, Entity entity) {
    const auto* name = scene.registry().try_get<NameComponent>(entity);
    return name != nullptr ? name->name.c_str() : "Camera";
}
} // namespace

ViewShading toViewShading(ViewportShading shading) {
    switch (shading) {
    case ViewportShading::Rendered:
        return ViewShading::Rendered;
    case ViewportShading::MaterialPreview:
        return ViewShading::MaterialPreview;
    case ViewportShading::Solid:
        return ViewShading::Solid;
    case ViewportShading::Wireframe:
        return ViewShading::Wireframe;
    }
    return ViewShading::Rendered;
}

std::optional<RenderSettings> viewportViewSettings(const RenderSettings& projectSettings,
                                                   const RenderCapabilities& capabilities, ViewportShading shading,
                                                   EditorMode mode) {
    const ViewShading viewShading = toViewShading(shading);
    if (mode != EditorMode::Edit || viewShading == projectSettings.viewShading) {
        return std::nullopt;
    }
    RenderSettings settings = projectSettings;
    settings.viewShading = viewShading;
    return viewShadingFrameSettings(settings, capabilities);
}

ViewportPanel::ViewportPanel(ViewportRegistry& registry)
    : m_registry(registry)
    , m_id(registry.add(*this)) {}

ViewportPanel::~ViewportPanel() {
    releaseRenderView();
    m_registry.remove(m_id);
}

void ViewportPanel::releaseRenderView() {
    if (m_rendering != nullptr) {
        m_registry.releaseView(*m_rendering, m_view);
    }
    m_rendering = nullptr;
    m_view = {};
    m_inputActive = false;
}

const char* ViewportPanel::title() const {
    return "Viewport";
}

int ViewportPanel::windowFlags() const {
    return ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
}

ViewportId ViewportPanel::id() const {
    return m_id;
}

RenderViewId ViewportPanel::renderView() const {
    return m_view;
}

EditorCamera& ViewportPanel::camera() {
    return m_camera;
}

const EditorCamera& ViewportPanel::camera() const {
    return m_camera;
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

bool ViewportPanel::lookThroughCamera(const Scene& scene, SceneEntityId entity) {
    if (!sceneCameraLens(scene, scene.findEntity(entity))) {
        return false;
    }
    m_cameraView = entity;
    return true;
}

void ViewportPanel::exitCameraView() {
    m_cameraView.reset();
}

std::optional<SceneEntityId> ViewportPanel::cameraView() const {
    return m_cameraView;
}

RenderView ViewportPanel::lens(const Scene& scene) const {
    if (m_cameraView) {
        if (const std::optional<RenderView> sceneLens = sceneCameraLens(scene, scene.findEntity(*m_cameraView))) {
            return *sceneLens;
        }
    }
    return m_camera.renderView();
}

const std::optional<RenderSettings>& ViewportPanel::appliedViewSettings() const {
    return m_appliedViewSettings;
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
    // Looking through a scene camera is read-only: the editor camera waits.
    const bool cameraInputActive =
        m_inputActive && !context.actions.modalTransformActive() && !m_suppressCameraUntilRelease && !m_cameraView;
    m_navigating =
        cameraInputActive && (input.mouseButtonDown(MouseButton::Middle) || input.mouseButtonDown(MouseButton::Right));
    m_camera.update(input, deltaSeconds, cameraInputActive);
}

void ViewportPanel::syncRenderView(EditorContext& context) {
    RenderRuntime& rendering = context.app.rendering;
    const Scene& scene = context.app.scene;
    // A deleted camera (or one that lost its CameraComponent) ends the
    // camera view.
    if (m_cameraView && !sceneCameraLens(scene, scene.findEntity(*m_cameraView))) {
        m_cameraView.reset();
    }
    const RenderView currentLens = lens(scene);
    rendering.setViewOverride(m_view, currentLens);
    m_appliedViewSettings =
        viewportViewSettings(rendering.settings(), rendering.capabilities(), m_chrome.shading, context.mode);
    rendering.setViewSettings(m_view, m_appliedViewSettings);
    if (m_chrome.overlays.grid) {
        const float distance = std::max(m_cameraView ? std::abs(currentLens.position.y) : m_camera.distance(), 0.1f);
        const float spacing = std::pow(10.0f, std::floor(std::log10(distance / 5.0f)));
        rendering.setViewGridOverlay(
            m_view, GridOverlay{.planeHeight = 0.0f, .spacing = spacing, .fadeDistance = spacing * 40.0f});
    } else {
        rendering.setViewGridOverlay(m_view, std::nullopt);
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
    context.app.rendering.setViewOutlinedObjects(m_view, m_outlinedObjects);
}

void ViewportPanel::hidden(EditorContext& context) {
    setInputActive(context, false);
}

void ViewportPanel::setInputActive(EditorContext& context, bool active) {
    m_inputActive = active;
    context.app.rendering.setViewInputActive(m_view, active);
}

void ViewportPanel::draw(EditorContext& context) {
    RenderRuntime& rendering = context.app.rendering;
    m_registry.markDrawn(m_id);
    // Trade a view of its own for the free default view, which renders every
    // frame anyway and keeps GPU-driven submission.
    if (m_view && m_view != rendering.defaultView() && m_registry.defaultViewFree()) {
        releaseRenderView();
    }
    if (!m_view) {
        m_view = m_registry.acquireView(rendering, m_id);
        if (m_view) {
            m_rendering = &rendering;
        }
    }
    // Blender's rule: view operators act on the viewport under the mouse. A
    // modal transform keeps the viewport it started in.
    const bool modal = context.actions.modalTransformActive();
    if (!modal && (m_navigating || ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))) {
        m_registry.activate(m_id);
    }

    const ImVec2 available = ImGui::GetContentRegionAvail();
    const uint32_t width = static_cast<uint32_t>(std::max(available.x, 16.0f));
    const uint32_t height = static_cast<uint32_t>(std::max(available.y, 16.0f));
    UiTextureHandle texture;
    if (m_view) {
        rendering.requestViewSize(m_view, width, height);
        texture = rendering.viewTexture(m_view);
    }

    // The image area is laid out without a texture too, so the chrome (and
    // headless tests of it) does not depend on a rendered frame.
    const ImVec2 imagePosition = ImGui::GetCursorScreenPos();
    m_position = {imagePosition.x, imagePosition.y};
    m_size = {static_cast<float>(width), static_cast<float>(height)};
    ImGui::Dummy({m_size.x, m_size.y});
    drawViewImage(texture, m_view ? "Viewport will be available on the next rendered frame."
                                  : "No render view is free; close another viewport.");
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ZENITH_MODEL_ASSET")) {
            context.placeModel(static_cast<const char*>(payload->Data), entt::null);
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(prefabAssetPayload)) {
            context.placePrefab(static_cast<const char*>(payload->Data), entt::null);
        }
        ImGui::EndDragDropTarget();
    }
    // The chrome is drawn in child windows, so the image reports no hover or
    // click while the mouse is over the header, tool shelf, or sidebar.
    const bool imageHovered = ImGui::IsItemHovered();
    const bool imageClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    m_inputActive = texture && (imageHovered || m_navigating);
    rendering.setViewInputActive(m_view, m_inputActive && !modal && !m_suppressCameraUntilRelease && !m_cameraView);

    const bool chromeHovered = drawChrome(context);
    if (m_view) {
        // After the chrome, whose menus may change the shading or overlays.
        syncRenderView(context);
    }
    if (modal) {
        // Only the viewport the transform started in advances it.
        context.actions.updateModalTransform(context.app, m_id);
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

void ViewportPanel::drawViewImage(UiTextureHandle texture, const char* placeholder) const {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 minimum{m_position.x, m_position.y};
    const ImVec2 maximum{m_position.x + m_size.x, m_position.y + m_size.y};
    if (texture) {
        drawList->AddImage(static_cast<ImTextureID>(texture.value), minimum, maximum);
        return;
    }
    const ImVec2 textSize = ImGui::CalcTextSize(placeholder);
    drawList->AddText({minimum.x + (m_size.x - textSize.x) * 0.5f, minimum.y + (m_size.y - textSize.y) * 0.5f},
                      ImGui::GetColorU32(ImGuiCol_TextDisabled), placeholder);
}

bool ViewportPanel::drawChrome(EditorContext& context) {
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const float headerHeight = ImGui::GetFrameHeight();
    // Each chrome child paints smoked glass in place of a background.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, toImVec4(palette::transparent));
    ImGui::PushStyleColor(ImGuiCol_MenuBarBg, toImVec4(palette::transparent));
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
        paintChildGlass();
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("View")) {
                drawViewMenu(context);
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
            if (m_cameraView) {
                const Entity camera = context.app.scene.findEntity(*m_cameraView);
                if (camera != entt::null) {
                    ImGui::TextDisabled("| Camera: %s", entityName(context.app.scene, camera));
                }
            }
            ImGui::EndMenuBar();
        }
        hovered = ImGui::IsWindowHovered();
    }
    ImGui::EndChild();
    return hovered;
}

void ViewportPanel::drawViewMenu(EditorContext& context) {
    // The menu's operators act on the active viewport, which is this one
    // while its menu is in use.
    m_registry.activate(m_id);
    const std::string sidebarShortcut = context.shortcut("view3d.toggle_sidebar");
    const std::string toolbarShortcut = context.shortcut("view3d.toggle_toolbar");
    const std::string cameraShortcut = context.shortcut("view3d.view_camera");
    if (ImGui::MenuItem("Sidebar", sidebarShortcut.c_str(), m_chrome.sidebarVisible)) {
        context.invoke("view3d.toggle_sidebar");
    }
    if (ImGui::MenuItem("Toolbar", toolbarShortcut.c_str(), m_chrome.toolbarVisible)) {
        context.invoke("view3d.toggle_toolbar");
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Camera View", cameraShortcut.c_str(), m_cameraView.has_value())) {
        context.invoke("view3d.view_camera");
    }
    const Scene& scene = context.app.scene;
    auto cameras = scene.registry().view<const CameraComponent>();
    if (ImGui::BeginMenu("Look Through", !cameras.empty())) {
        for (const Entity entity : cameras) {
            const SceneEntityId id = scene.id(entity);
            ImGui::PushID(static_cast<int>(entt::to_integral(entity)));
            if (ImGui::MenuItem(entityName(scene, entity), nullptr, m_cameraView == id)) {
                lookThroughCamera(scene, id);
            }
            ImGui::PopID();
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Exit Camera View", nullptr, false, m_cameraView.has_value())) {
        exitCameraView();
    }
}

bool ViewportPanel::drawToolShelf(float headerHeight) {
    const ImGuiStyle& style = ImGui::GetStyle();
    // Square icon buttons a little taller than a text row.
    const float buttonWidth = ImGui::GetFrameHeight() + editorArtPixel() * 4.0f;
    const ImVec2 shelfSize{buttonWidth + style.WindowPadding.x * 2.0f,
                           buttonWidth * 4.0f + style.ItemSpacing.y * 3.0f + style.WindowPadding.y * 2.0f};
    ImGui::SetCursorScreenPos({m_position.x + chromeMargin, m_position.y + headerHeight + chromeMargin});
    bool hovered = false;
    if (ImGui::BeginChild("##ViewportToolShelf", shelfSize, ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        paintChildGlass();
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
        paintChildGlass();
        if (ImGui::BeginTabBar("##ViewportSidebarTabs")) {
            if (ImGui::BeginTabItem(BuiltInViewportSidebarTab::item)) {
                drawItemTab(context);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(BuiltInViewportSidebarTab::tool)) {
                ViewportChromeWidgets::drawToolTab(m_tools);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(BuiltInViewportSidebarTab::view)) {
                if (m_cameraView) {
                    ImGui::TextDisabled("Looking through a scene camera.");
                    if (widgets::Button("Exit Camera View")) {
                        exitCameraView();
                    }
                    ImGui::Separator();
                }
                ViewportChromeWidgets::drawViewTab(m_camera);
                ImGui::EndTabItem();
            }
            // By index, drawing a copy: a tab's draw may add or remove tabs.
            for (size_t index = 0; index < m_registry.sidebarTabs().size(); ++index) {
                const ViewportSidebarTab tab = m_registry.sidebarTabs()[index];
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
    const Entity picked = pickThroughLens(scene, context.app.assets, lens(scene), normalized, m_size.x / m_size.y);
    if (context.eyedropperArmed()) {
        // An armed eyedropper takes the click and leaves the selection alone;
        // a click on empty space keeps it waiting.
        if (picked != entt::null) {
            context.pickEyedropperEntity(scene.id(picked));
        }
        return;
    }
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
    const float artPixel = editorArtPixel();
    ThemeChrome::paintSmokedGlass(
        drawList, {labelPosition.x - 4.0f * artPixel, labelPosition.y - 2.0f * artPixel},
        {labelPosition.x + labelSize.x + 4.0f * artPixel, labelPosition.y + labelSize.y + 2.0f * artPixel});
    drawList->AddText(labelPosition, toImU32(palette::activeObjectLabel), name.c_str());
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
    const RenderView currentLens = lens(scene);
    const glm::mat4 view = lensViewMatrix(currentLens);
    const glm::mat4 projection = lensGizmoProjectionMatrix(currentLens, m_size.x / m_size.y);
    // ImGuizmo's drag state is global; the id keeps each viewport's gizmo
    // from answering another's drag.
    ImGuizmo::PushID(static_cast<int>(m_id.value));
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
    const bool usingGizmo = ImGuizmo::IsUsing();
    const bool overGizmo = ImGuizmo::IsOver();
    ImGuizmo::PopID();
    if (changed) {
        const Entity parent = registry.get<RelationshipComponent>(entity).parent;
        registry.get<TransformComponent>(entity).local =
            parent == entt::null ? world : glm::inverse(registry.get<TransformComponent>(parent).world) * world;
        scene.markTransformDirty(entity);
        scene.updateWorldTransforms();
    }
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
    return usingGizmo || overGizmo;
}
