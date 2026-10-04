#include "ViewportPanel.hpp"

#include "editor/document/EditorDocument.hpp"
#include "editor/interaction/ComponentEditors.hpp"
#include "editor/interaction/PickQueries.hpp"
#include "editor/operators/KeymapDispatch.hpp"
#include "editor/panels/AssetDragPayloads.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/EditorWidgets.hpp"
#include "editor/theme/PixelPainter.hpp"
#include "editor/theme/ThemeChrome.hpp"
#include "editor/viewport/Cursor3D.hpp"
#include "editor/viewport/SceneGizmoDraw.hpp"
#include "editor/viewport/ViewFraming.hpp"
#include "engine/graphics/ViewShading.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <ImGuizmo.h>

#include <algorithm>
#include <array>
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

ViewportShadingSource viewportShadingSource(EditorMode mode, bool simulating) {
    if (mode == EditorMode::Edit || simulating) {
        return ViewportShadingSource::Viewport;
    }
    return ViewportShadingSource::Project;
}

std::optional<RenderSettings> viewportViewSettings(const RenderSettings& projectSettings,
                                                   const RenderCapabilities& capabilities, ViewportShading shading,
                                                   ViewportShadingSource source) {
    const ViewShading viewShading = toViewShading(shading);
    if (source == ViewportShadingSource::Project || viewShading == projectSettings.viewShading) {
        return std::nullopt;
    }
    RenderSettings settings = projectSettings;
    settings.viewShading = viewShading;
    return viewShadingFrameSettings(settings, capabilities);
}

bool viewportDrawsSceneGizmos(EditorMode mode, bool lookingThroughSceneCamera, const ViewportOverlays& overlays) {
    if (mode == EditorMode::Edit) {
        return true;
    }
    return overlays.sceneInPlay && !lookingThroughSceneCamera;
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
    // The registry resets a released default view's isolation, and a
    // created view is destroyed, so the next view needs it handed over again.
    m_isolationView = {};
    m_sentIsolation.reset();
}

std::string ViewportPanel::areaState() const {
    return viewportAreaState(m_chrome, m_tools);
}

void ViewportPanel::restoreAreaState(std::string_view state) {
    restoreViewportAreaState(state, m_chrome, m_tools);
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

SnapView ViewportPanel::snapView(const Scene& scene) const {
    return {.lens = lens(scene),
            .viewSize = m_size,
            .isolatedObjects = m_localViewObjects ? &*m_localViewObjects : nullptr};
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

bool ViewportPanel::cameraWaitsForRightRelease() const {
    return m_suppressCameraUntilRelease;
}

void ViewportPanel::suppressCameraUntilRightRelease() {
    m_suppressCameraUntilRelease = true;
}

void ViewportPanel::requestClickSelect(glm::vec2 viewPoint, bool extend, bool cycle) {
    m_queuedSelect = QueuedSelect{
        .kind = cycle ? ViewportPickKind::Cycle : ViewportPickKind::Click, .point = viewPoint, .extend = extend};
}

glm::vec2 ViewportPanel::mouseInView() const {
    return m_mouseInView;
}

void ViewportPanel::beginBoxSelect(SelectionSetMode mode) {
    m_lassoSelect.cancel();
    m_boxSelect.begin(mode);
}

void ViewportPanel::beginLassoSelect(SelectionSetMode mode, bool rightDrag) {
    m_boxSelect.cancel();
    // The gesture's press is the outline's first point.
    m_lassoSelect.begin(mode, rightDrag, rightDrag ? m_mouseGestureStart.value_or(m_mouseInView) : m_mouseInView);
    if (rightDrag) {
        suppressCameraUntilRightRelease();
    }
}

void ViewportPanel::cancelSelectTool() {
    m_boxSelect.cancel();
    m_lassoSelect.cancel();
}

bool ViewportPanel::selectToolActive() const {
    return m_boxSelect.active() || m_lassoSelect.active();
}

const BoxSelect& ViewportPanel::boxSelect() const {
    return m_boxSelect;
}

const LassoSelect& ViewportPanel::lassoSelect() const {
    return m_lassoSelect;
}

bool ViewportPanel::pickPending() const {
    return m_queuedSelect.has_value() || m_picker.pending();
}

const std::optional<ViewportPickOutcome>& ViewportPanel::lastPickOutcome() const {
    return m_lastPickOutcome;
}

bool ViewportPanel::enterLocalView(Scene& scene, const AssetManager& assets, std::span<const SceneEntityId> entities) {
    if (m_localView) {
        return false;
    }
    std::vector<SceneEntityId> existing;
    for (SceneEntityId id : entities) {
        if (scene.findEntity(id) != entt::null && std::ranges::find(existing, id) == existing.end()) {
            existing.push_back(id);
        }
    }
    if (existing.empty()) {
        return false;
    }
    m_localView = LocalView{.entities = existing, .previousCamera = m_camera, .previousCameraView = m_cameraView};
    exitCameraView();
    // Frame the whole isolated hierarchy, as the view will show it.
    std::vector<SceneEntityId> framed;
    std::vector<Entity> traversal;
    for (SceneEntityId id : existing) {
        traversal.push_back(scene.findEntity(id));
    }
    while (!traversal.empty()) {
        const Entity entity = traversal.back();
        traversal.pop_back();
        framed.push_back(scene.id(entity));
        if (const auto* relationship = scene.registry().try_get<RelationshipComponent>(entity)) {
            traversal.insert(traversal.end(), relationship->children.begin(), relationship->children.end());
        }
    }
    frameEntities(m_camera, scene, assets, framed);
    return true;
}

void ViewportPanel::exitLocalView() {
    if (!m_localView) {
        return;
    }
    m_camera = m_localView->previousCamera;
    m_cameraView = m_localView->previousCameraView;
    m_localView.reset();
    m_localViewObjects.reset();
}

bool ViewportPanel::localViewActive() const {
    return m_localView.has_value();
}

std::span<const SceneEntityId> ViewportPanel::localViewEntities() const {
    return m_localView ? std::span<const SceneEntityId>(m_localView->entities) : std::span<const SceneEntityId>{};
}

const std::optional<std::vector<RenderObjectId>>& ViewportPanel::localViewObjects() const {
    return m_localViewObjects;
}

void ViewportPanel::update(EditorContext& context, const InputState& input, float deltaSeconds) {
    if (m_suppressCameraUntilRelease && !input.mouseButtonDown(MouseButton::Right)) {
        m_suppressCameraUntilRelease = false;
    }
    // Looking through a scene camera is read-only: the editor camera waits.
    // So does it during a box or lasso, and from a right or middle press the
    // keymap binds with the modifiers held (Ctrl+RMB lassos and Shift+RMB
    // places the 3D cursor rather than flying); unbound presses navigate.
    const bool boundPress =
        !m_navigating && ((input.mouseButtonDown(MouseButton::Right) &&
                           context.actions.mouseButtonBound(KeymapRegion::Viewport, ImGuiMouseButton_Right)) ||
                          (input.mouseButtonDown(MouseButton::Middle) &&
                           context.actions.mouseButtonBound(KeymapRegion::Viewport, ImGuiMouseButton_Middle)));
    const bool cameraInputActive = m_inputActive && !context.actions.modalTransformActive() &&
                                   !m_suppressCameraUntilRelease && !m_cameraView && !selectToolActive() && !boundPress;
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
    m_appliedViewSettings = viewportViewSettings(rendering.settings(), rendering.capabilities(), m_chrome.shading,
                                                 viewportShadingSource(context.mode, context.simulating));
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
    recordSceneGizmos(context);
}

void ViewportPanel::recordSceneGizmos(EditorContext& context) {
    if (!viewportDrawsSceneGizmos(context.mode, m_cameraView.has_value(), m_chrome.overlays)) {
        return;
    }
    // Only this view's list: the all-views list would reach every view.
    DebugDraw* output = context.app.rendering.viewDebugDraw(m_view);
    if (output == nullptr) {
        return;
    }
    Scene& scene = context.app.scene;
    scene.updateWorldTransforms();
    // A camera view skips the frustum of the camera it looks through.
    const std::optional<SceneEntityId> viewCamera =
        m_cameraView && sceneCameraLens(scene, scene.findEntity(*m_cameraView)) ? m_cameraView : std::nullopt;
    drawSceneGizmos({.scene = scene,
                     .assets = context.app.assets,
                     .selection = context.selection.ids(),
                     .aspectRatio = m_size.x / std::max(m_size.y, 1.0f),
                     .viewCamera = viewCamera,
                     .isolatedObjects = m_localViewObjects ? &*m_localViewObjects : nullptr},
                    m_chrome.overlays.scene, *output);
    if (m_chrome.overlays.cursor) {
        recordCursor3D(context.editing.document().editorData().cursorLocation, lens(scene), m_size, *output);
    }
}

void ViewportPanel::drawSceneLabels(EditorContext& context) const {
    if (!m_view || !viewportDrawsSceneGizmos(context.mode, m_cameraView.has_value(), m_chrome.overlays)) {
        return;
    }
    const RenderRuntime& rendering = context.app.rendering;
    const std::span<const ProjectedDebugLabel> labels = rendering.viewDebugLabels(m_view);
    const float viewWidth = static_cast<float>(rendering.viewWidth(m_view));
    const float viewHeight = static_cast<float>(rendering.viewHeight(m_view));
    if (labels.empty() || viewWidth <= 0.0f || viewHeight <= 0.0f) {
        return;
    }
    // Labels are in the view's output pixels from its latest frame, which
    // can lag a resize by a frame; scale them onto the image as drawn.
    const glm::vec2 scale = m_size / glm::vec2(viewWidth, viewHeight);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float artPixel = editorArtPixel();
    const ImU32 outline = toImU32(palette::captionOutline);
    drawList->PushClipRect({m_position.x, m_position.y}, {m_position.x + m_size.x, m_position.y + m_size.y}, true);
    for (const ProjectedDebugLabel& label : labels) {
        const glm::vec2 anchor = m_position + label.position * scale;
        const ImU32 ink = toImU32({label.color.r, label.color.g, label.color.b, label.color.a});
        pixel::outlinedText(
            drawList, ImGui::GetFont(), ImGui::GetFontSize(),
            {std::floor(anchor.x + 3.0f * artPixel), std::floor(anchor.y - ImGui::GetFontSize() * 0.5f)}, ink, outline,
            label.text.c_str(), artPixel);
    }
    drawList->PopClipRect();
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
    cancelSelectTool();
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
    if (texture) {
        drawSceneLabels(context);
    }
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
    m_inputActive = texture && (imageHovered || m_navigating);
    rendering.setViewInputActive(m_view, m_inputActive && !modal && !m_suppressCameraUntilRelease && !m_cameraView);

    const bool chromeHovered = drawChrome(context);
    // Before the render view, whose scene gizmos follow this frame's isolation.
    syncLocalView(context);
    if (m_view) {
        // After the chrome, whose menus may change the shading or overlays.
        syncRenderView(context);
    }
    const ImVec2 mouse = ImGui::GetMousePos();
    m_mouseInView = glm::vec2(mouse.x, mouse.y) - m_position;
    if (modal) {
        // Only the viewport the transform started in advances it.
        context.actions.updateModalTransform(context.app, m_id);
        processPicks(context);
        return;
    }

    bool mousePressesAllowed = false;
    if (selectToolActive()) {
        // A box or lasso owns the mouse and keys until it ends.
        updateSelectTools(imageHovered);
    } else {
        const bool gizmoBlocksPicking = m_chrome.overlays.gizmo && drawGizmo(context);
        if (imageHovered || chromeHovered) {
            context.actions.handleObjectKeys(context.app, KeymapRegion::Viewport);
        }
        mousePressesAllowed = imageHovered && !gizmoBlocksPicking && !context.actions.modalTransformActive();
    }
    dispatchMouseChords(context, mousePressesAllowed);
    processPicks(context);
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
    ImGui::Separator();
    if (ImGui::BeginMenu("Snap")) {
        constexpr std::array<std::pair<const char*, const char*>, 3> cursorItems{{
            {"Cursor to Selected", "view3d.snap_cursor_to_selected"},
            {"Cursor to World Origin", "view3d.cursor_reset"},
            {"Selection to Cursor", "view3d.snap_selected_to_cursor"},
        }};
        for (const auto& [label, operatorId] : cursorItems) {
            const std::string shortcut = context.shortcut(operatorId);
            if (ImGui::MenuItem(label, shortcut.c_str(), false, context.canInvoke(operatorId))) {
                context.invoke(operatorId);
            }
        }
        constexpr const char* keepOffset = R"({"offset":true})";
        if (ImGui::MenuItem("Selection to Cursor (Keep Offset)", nullptr, false,
                            context.canInvoke("view3d.snap_selected_to_cursor"))) {
            context.invoke("view3d.snap_selected_to_cursor", keepOffset);
        }
        ImGui::EndMenu();
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
    // In Play and Simulate this edits the running scene live (SceneEditing
    // records nothing there).
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
}

void ViewportPanel::dispatchMouseChords(EditorContext& context, bool pressesAllowed) {
    // Presses count over the image when no gizmo is under the mouse; while
    // the eyedropper is armed, a right press is left to cancel it.
    uint32_t allowedButtons = 0;
    if (pressesAllowed) {
        allowedButtons = allMouseButtons;
        if (context.eyedropperArmed()) {
            allowedButtons &= ~(1u << ImGuiMouseButton_Right);
        }
    }
    if (!ImGui::IsAnyMouseDown()) {
        m_mouseGestureStart.reset();
    }
    const uint32_t claimed = context.actions.dispatchMouseChords(context.app, KeymapRegion::Viewport, allowedButtons);
    if (claimed != 0) {
        m_mouseGestureStart = m_mouseInView;
    }
    if ((claimed & (1u << ImGuiMouseButton_Right)) != 0) {
        // A bound right press is not fly navigation, even if its modifiers
        // are let go before the button.
        suppressCameraUntilRightRelease();
    }
}

void ViewportPanel::updateSelectTools(bool imageHovered) {
    const ImGuiIO& io = ImGui::GetIO();
    const SelectionGestureInput input{.mouse = m_mouseInView,
                                      .hovered = imageHovered,
                                      .leftPressed = ImGui::IsMouseClicked(ImGuiMouseButton_Left),
                                      .leftDown = ImGui::IsMouseDown(ImGuiMouseButton_Left),
                                      .rightPressed = ImGui::IsMouseClicked(ImGuiMouseButton_Right),
                                      .rightDown = ImGui::IsMouseDown(ImGuiMouseButton_Right),
                                      .escapePressed = ImGui::IsKeyPressed(ImGuiKey_Escape, false),
                                      .shift = io.KeyShift,
                                      .ctrl = io.KeyCtrl};
    if (input.rightPressed && !m_lassoSelect.drawsWithRightButton()) {
        // The right press cancels the tool; it must not start fly navigation.
        suppressCameraUntilRightRelease();
    }
    std::optional<RegionSelection> region =
        m_boxSelect.active() ? m_boxSelect.update(input) : m_lassoSelect.update(input);
    if (region) {
        m_queuedSelect = QueuedSelect{.kind = ViewportPickKind::Region, .region = std::move(*region)};
    }
    ImDrawList& drawList = *ImGui::GetWindowDrawList();
    m_boxSelect.draw(drawList, m_position);
    m_lassoSelect.draw(drawList, m_position);
}

void ViewportPanel::processPicks(EditorContext& context) {
    RenderPickProvider rendererPicks(context.app.rendering);
    ViewportPickProvider* provider = m_registry.pickProvider();
    ViewportPickProvider& picks = provider != nullptr ? *provider : rendererPicks;
    Scene& scene = context.app.scene;
    if (m_queuedSelect) {
        QueuedSelect queued = std::move(*m_queuedSelect);
        m_queuedSelect.reset();
        scene.updateWorldTransforms();
        m_pendingIntent =
            SelectIntent{.kind = queued.kind,
                         .extend = queued.extend,
                         .mode = queued.region.mode,
                         .eyedropper = queued.kind != ViewportPickKind::Region && context.eyedropperArmed()};
        m_picker.start(picks, m_view,
                       ViewportPickQuery{.kind = queued.kind,
                                         .point = queued.point,
                                         .region = std::move(queued.region),
                                         .lens = lens(scene),
                                         .viewSize = m_size,
                                         .isolatedObjects = m_localViewObjects});
    }
    if (!m_picker.pending()) {
        return;
    }
    scene.updateWorldTransforms();
    if (const std::optional<ViewportPickOutcome> outcome = m_picker.poll(picks, scene, context.app.assets)) {
        applyPick(context, m_pendingIntent, *outcome);
    }
}

void ViewportPanel::applyPick(EditorContext& context, const SelectIntent& intent, const ViewportPickOutcome& outcome) {
    m_lastPickOutcome = outcome;
    if (intent.kind == ViewportPickKind::Region) {
        applySelectionSet(context.selection, outcome.entities, intent.mode);
        return;
    }
    std::optional<SceneEntityId> picked;
    if (intent.kind == ViewportPickKind::Cycle && !intent.eyedropper) {
        picked = cycleCandidate(outcome.entities, context.selection.active());
    } else if (!outcome.entities.empty()) {
        picked = outcome.entities.front();
    }
    if (intent.eyedropper) {
        // An armed eyedropper takes the click and leaves the selection alone;
        // a click on empty space keeps it waiting.
        if (picked && context.eyedropperArmed()) {
            context.pickEyedropperEntity(*picked);
        }
        return;
    }
    if (!picked) {
        // Blender keeps the selection when a Shift click misses.
        if (!intent.extend) {
            context.selection.clear();
        }
        return;
    }
    context.selection.click(*picked, intent.extend ? SelectionClickMode::Extend : SelectionClickMode::Replace);
}

void ViewportPanel::syncLocalView(EditorContext& context) {
    if (!m_localView) {
        m_localViewObjects.reset();
    } else {
        const Scene& scene = context.app.scene;
        std::vector<RenderObjectId> objects;
        m_outlineTraversal.clear();
        for (SceneEntityId id : m_localView->entities) {
            if (const Entity entity = scene.findEntity(id); entity != entt::null) {
                m_outlineTraversal.push_back(entity);
            }
        }
        while (!m_outlineTraversal.empty()) {
            const Entity entity = m_outlineTraversal.back();
            m_outlineTraversal.pop_back();
            objects.push_back(static_cast<RenderObjectId>(entt::to_integral(entity)));
            if (const auto* relationship = scene.registry().try_get<RelationshipComponent>(entity)) {
                m_outlineTraversal.insert(m_outlineTraversal.end(), relationship->children.begin(),
                                          relationship->children.end());
            }
        }
        std::ranges::sort(objects);
        const auto duplicates = std::ranges::unique(objects);
        objects.erase(duplicates.begin(), duplicates.end());
        m_localViewObjects = std::move(objects);
    }
    if (m_view && (m_isolationView != m_view || m_sentIsolation != m_localViewObjects)) {
        context.app.rendering.setViewVisibleEntities(m_view, m_localViewObjects);
        m_isolationView = m_view;
        m_sentIsolation = m_localViewObjects;
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
    const glm::mat4 startWorld = registry.get<TransformComponent>(entity).world;
    // The gizmo sits on the pivot: the 3D cursor while rotating or scaling
    // about it, otherwise the object's origin (with one object selected, the
    // median, active, and individual origins are all that origin).
    const bool aboutCursor = m_tools.pivot == TransformPivot::Cursor && m_tools.gizmo != GizmoMode::Translate;
    glm::mat4 gizmo = startWorld;
    if (aboutCursor) {
        gizmo[3] = glm::vec4(context.editing.document().editorData().cursorLocation, 1.0f);
    }
    const glm::mat4 gizmoBefore = gizmo;
    if (!m_gizmoWasUsing) {
        m_gizmoStartPosition = glm::vec3(gizmo[3]);
    }
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
    // Ctrl inverts snapping, as during modal transforms. ImGuizmo snaps in
    // increments from the drag start; the absolute grid and surface or vertex
    // targets snap its translation here instead.
    const TransformSnapping snapping = transformSnapping(m_tools, ImGui::GetIO().KeyCtrl);
    const bool snapsTranslationHere = snapping.enabled && m_tools.gizmo == GizmoMode::Translate &&
                                      (snapping.absoluteGrid || snapping.target != SnapTarget::Increment);
    const float snapValue = m_tools.activeSnap();
    const float snap[3]{snapValue, snapValue, snapValue};
    const glm::mat4 localBeforeManipulation = registry.get<TransformComponent>(entity).local;
    const bool changed = ImGuizmo::Manipulate(&view[0][0], &projection[0][0], operation, mode, &gizmo[0][0], nullptr,
                                              snapping.enabled && !snapsTranslationHere ? snap : nullptr);
    const bool usingGizmo = ImGuizmo::IsUsing();
    const bool overGizmo = ImGuizmo::IsOver();
    ImGuizmo::PopID();
    if (changed && snapsTranslationHere) {
        gizmo[3] =
            glm::vec4(snapGizmoTranslation(context, snapping, glm::vec3(gizmo[3]), normalizedAxes(gizmoBefore)), 1.0f);
    }
    // About the cursor the object follows the gizmo's change; otherwise the
    // gizmo is the object.
    glm::mat4 world = gizmo;
    if (aboutCursor) {
        world = std::abs(glm::determinant(glm::mat3(gizmoBefore))) > 1e-12f
                    ? gizmo * glm::inverse(gizmoBefore) * startWorld
                    : startWorld;
    }
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
        // In Play and Simulate the editing is the runtime scene's, which
        // records nothing: a drag there never reaches the edit history.
        context.editing.commit("Transform Gizmo", {}, std::move(m_gizmoBefore));
        m_gizmoBefore.clear();
    }
    m_gizmoWasUsing = usingGizmo;
    return usingGizmo || overGizmo;
}

glm::vec3 ViewportPanel::snapGizmoTranslation(EditorContext& context, const TransformSnapping& snapping,
                                              const glm::vec3& position, const glm::mat3& localAxes) const {
    // The gizmo's axis or plane is whichever of its frame's components moved
    // since the drag started; only those snap.
    const glm::mat3 frame = m_tools.space == TransformSpace::Local ? localAxes : glm::mat3(1.0f);
    const glm::mat3 toFrame = glm::transpose(frame);
    const glm::vec3 start = toFrame * m_gizmoStartPosition;
    const glm::vec3 moved = toFrame * (position - m_gizmoStartPosition);
    std::optional<glm::vec3> target;
    if (snapping.target != SnapTarget::Increment) {
        Scene& scene = context.app.scene;
        const std::vector<Entity> excluded = snapExclusions(scene, context.selection.ids());
        target = snapTargetPoint(snapping.target, scene, context.app.assets, snapView(scene), m_mouseInView, excluded);
        if (!target) {
            return position;
        }
    }
    glm::vec3 snapped = moved;
    for (int index = 0; index < 3; ++index) {
        if (std::abs(moved[index]) <= 1e-6f) {
            continue;
        }
        if (target) {
            snapped[index] = (toFrame * (*target - m_gizmoStartPosition))[index];
        } else {
            const float increment = snapping.translation;
            if (increment <= 0.0f) {
                continue;
            }
            snapped[index] = std::round((start[index] + moved[index]) / increment) * increment - start[index];
        }
    }
    return m_gizmoStartPosition + frame * snapped;
}
