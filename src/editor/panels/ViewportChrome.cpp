#include "ViewportChrome.hpp"

#include "editor/interaction/EditorCamera.hpp"
#include "editor/theme/EditorWidgets.hpp"

#include <imgui.h>

#include <glm/trigonometric.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <string>
#include <utility>

namespace {
struct ShadingProperties {
    ViewportShading type{ViewportShading::Rendered};
};

constexpr std::array<ViewportShading, 4> shadingModes{ViewportShading::Rendered, ViewportShading::MaterialPreview,
                                                      ViewportShading::Solid, ViewportShading::Wireframe};

struct ToolChoice {
    const char* label;
    const char* description;
    GizmoMode mode;
    EditorIcon icon;
};

constexpr std::array<ToolChoice, 4> toolChoices{{
    {"Tweak", "Select objects by clicking; no gizmo", GizmoMode::Select, EditorIcon::ToolTweak},
    {"Move", "Move the selection with a gizmo", GizmoMode::Translate, EditorIcon::ToolMove},
    {"Rotate", "Rotate the selection with a gizmo", GizmoMode::Rotate, EditorIcon::ToolRotate},
    {"Scale", "Scale the selection with a gizmo", GizmoMode::Scale, EditorIcon::ToolScale},
}};

constexpr const char* noViewportError = "No viewport is open";

std::function<bool(const OperatorContext&)> hasTarget(const ViewportChromeTarget& target) {
    return [target](const OperatorContext&) { return target() != nullptr; };
}

struct OverlayName {
    ViewportOverlay overlay;
    // The operator option and area-state key.
    const char* key;
    const char* label;
};

constexpr std::array<OverlayName, 14> overlayNames{{
    {ViewportOverlay::Grid, "grid", "Grid"},
    {ViewportOverlay::SelectionOutline, "selection_outline", "Selection Outline"},
    {ViewportOverlay::ActiveName, "active_name", "Active Object Name"},
    {ViewportOverlay::TransformGizmo, "transform_gizmo", "Transform Gizmo"},
    {ViewportOverlay::Lights, "lights", "Lights"},
    {ViewportOverlay::Cameras, "cameras", "Cameras"},
    {ViewportOverlay::Colliders, "colliders", "Colliders"},
    {ViewportOverlay::ReflectionProbes, "reflection_probes", "Reflection Probes"},
    {ViewportOverlay::Decals, "decals", "Decals"},
    {ViewportOverlay::IrradianceVolumes, "irradiance_volumes", "Irradiance Volumes"},
    {ViewportOverlay::Names, "names", "Names"},
    {ViewportOverlay::SelectionBounds, "selection_bounds", "Selection Bounds"},
    {ViewportOverlay::SceneInPlay, "scene_in_play", "Scene Overlays in Play"},
    {ViewportOverlay::Cursor3D, "cursor_3d", "3D Cursor"},
}};

struct PivotName {
    TransformPivot pivot;
    const char* key;
    const char* label;
};

constexpr std::array<PivotName, 4> pivotNames{{
    {TransformPivot::MedianPoint, "median_point", "Median Point"},
    {TransformPivot::ActiveElement, "active_element", "Active Element"},
    {TransformPivot::IndividualOrigins, "individual_origins", "Individual Origins"},
    {TransformPivot::Cursor, "cursor", "3D Cursor"},
}};

struct SnapTargetName {
    SnapTarget target;
    const char* key;
    const char* label;
};

constexpr std::array<SnapTargetName, 3> snapTargetNames{{
    {SnapTarget::Increment, "increment", "Increment"},
    {SnapTarget::Surface, "surface", "Surface"},
    {SnapTarget::Vertex, "vertex", "Vertex"},
}};

struct PivotProperties {
    TransformPivot pivot{TransformPivot::MedianPoint};
};

struct SnapTargetProperties {
    SnapTarget target{SnapTarget::Increment};
};

std::vector<EnumOption> pivotOptions() {
    std::vector<EnumOption> options;
    for (const PivotName& name : pivotNames) {
        options.push_back({name.key, name.label, static_cast<int64_t>(name.pivot)});
    }
    return options;
}

std::vector<EnumOption> snapTargetOptions() {
    std::vector<EnumOption> options;
    for (const SnapTargetName& name : snapTargetNames) {
        options.push_back({name.key, name.label, static_cast<int64_t>(name.target)});
    }
    return options;
}

// The tool settings saved in the area state, under "tools".
nlohmann::json toolStateJson(const TransformToolSettings& tools) {
    nlohmann::json saved{{"snapping", tools.snapping},
                         {"absolute_grid", tools.absoluteGridSnap},
                         {"translation_snap", tools.translationSnap},
                         {"rotation_snap_degrees", tools.rotationSnapDegrees},
                         {"scale_snap", tools.scaleSnap}};
    for (const PivotName& name : pivotNames) {
        if (name.pivot == tools.pivot) {
            saved["pivot"] = name.key;
        }
    }
    for (const SnapTargetName& name : snapTargetNames) {
        if (name.target == tools.snapTarget) {
            saved["snap_target"] = name.key;
        }
    }
    return saved;
}

void restoreToolState(const nlohmann::json& saved, TransformToolSettings& tools) {
    const auto readBool = [&saved](const char* key, bool& value) {
        if (const auto found = saved.find(key); found != saved.end() && found->is_boolean()) {
            value = found->get<bool>();
        }
    };
    // Increments must stay positive, as the widgets keep them.
    const auto readIncrement = [&saved](const char* key, float& value) {
        if (const auto found = saved.find(key);
            found != saved.end() && found->is_number() && found->get<float>() > 0.0f) {
            value = found->get<float>();
        }
    };
    readBool("snapping", tools.snapping);
    readBool("absolute_grid", tools.absoluteGridSnap);
    readIncrement("translation_snap", tools.translationSnap);
    readIncrement("rotation_snap_degrees", tools.rotationSnapDegrees);
    readIncrement("scale_snap", tools.scaleSnap);
    if (const auto pivot = saved.find("pivot"); pivot != saved.end() && pivot->is_string()) {
        for (const PivotName& name : pivotNames) {
            if (pivot->get<std::string>() == name.key) {
                tools.pivot = name.pivot;
            }
        }
    }
    if (const auto target = saved.find("snap_target"); target != saved.end() && target->is_string()) {
        for (const SnapTargetName& name : snapTargetNames) {
            if (target->get<std::string>() == name.key) {
                tools.snapTarget = name.target;
            }
        }
    }
}

struct ScopeName {
    OverlayScope scope;
    const char* key;
    const char* label;
};

constexpr std::array<ScopeName, 3> scopeNames{{
    {OverlayScope::Off, "off", "Off"},
    {OverlayScope::Selected, "selected", "Selected Only"},
    {OverlayScope::All, "all", "All"},
}};

struct OverlayToggleProperties {
    ViewportOverlay overlay{ViewportOverlay::Grid};
};

struct OverlayScopeProperties {
    ViewportOverlay overlay{ViewportOverlay::Lights};
    OverlayScope scope{OverlayScope::All};
};

std::vector<EnumOption> overlayOptions() {
    std::vector<EnumOption> options;
    for (const OverlayName& name : overlayNames) {
        options.push_back({name.key, name.label, static_cast<int64_t>(name.overlay)});
    }
    return options;
}

std::vector<EnumOption> scopeOptions() {
    std::vector<EnumOption> options;
    for (const ScopeName& name : scopeNames) {
        options.push_back({name.key, name.label, static_cast<int64_t>(name.scope)});
    }
    return options;
}

const char* overlayLabel(ViewportOverlay overlay) {
    for (const OverlayName& name : overlayNames) {
        if (name.overlay == overlay) {
            return name.label;
        }
    }
    return "Overlay";
}

// Flips an on/off overlay, or switches a scoped one between Off and the scope
// it has by default.
void toggleOverlay(ViewportOverlays& overlays, ViewportOverlay overlay) {
    if (bool* flag = viewportOverlayFlag(overlays, overlay)) {
        *flag = !*flag;
        return;
    }
    OverlayScope* scope = viewportOverlayScope(overlays, overlay);
    if (scope == nullptr) {
        return;
    }
    if (*scope != OverlayScope::Off) {
        *scope = OverlayScope::Off;
        return;
    }
    ViewportOverlays defaults;
    const OverlayScope* defaultScope = viewportOverlayScope(defaults, overlay);
    *scope = defaultScope != nullptr && *defaultScope != OverlayScope::Off ? *defaultScope : OverlayScope::All;
}

Operator overlayToggleOperator(ViewportChromeTarget target) {
    return {.id = "view3d.overlay_toggle",
            .label = "Toggle Overlay",
            .description = "Show or hide one of the viewport's overlays",
            .properties = OperatorProperties::make(OverlayToggleProperties{},
                                                   std::move(Fields<OverlayToggleProperties>()
                                                                 .add("overlay", &OverlayToggleProperties::overlay)
                                                                 .options(overlayOptions()))),
            .poll = hasTarget(target),
            .execute = [target](OperatorContext&, OperatorProperties& properties) {
                ViewportChromeState* state = target();
                if (state == nullptr) {
                    return OperatorResult::failed(noViewportError);
                }
                const OverlayToggleProperties* values = properties.get<OverlayToggleProperties>();
                if (values == nullptr) {
                    return OperatorResult::failed("view3d.overlay_toggle needs an overlay");
                }
                toggleOverlay(state->overlays, values->overlay);
                return OperatorResult::finished();
            }};
}

Operator overlayScopeOperator(ViewportChromeTarget target) {
    return {.id = "view3d.overlay_scope_set",
            .label = "Set Overlay Scope",
            .description = "Choose whether a scene overlay draws for no objects, the selection, or every object",
            .properties = OperatorProperties::make(OverlayScopeProperties{},
                                                   std::move(Fields<OverlayScopeProperties>()
                                                                 .add("overlay", &OverlayScopeProperties::overlay)
                                                                 .options(overlayOptions())
                                                                 .add("scope", &OverlayScopeProperties::scope)
                                                                 .options(scopeOptions()))),
            .poll = hasTarget(target),
            .execute = [target](OperatorContext&, OperatorProperties& properties) {
                ViewportChromeState* state = target();
                if (state == nullptr) {
                    return OperatorResult::failed(noViewportError);
                }
                const OverlayScopeProperties* values = properties.get<OverlayScopeProperties>();
                if (values == nullptr) {
                    return OperatorResult::failed("view3d.overlay_scope_set needs an overlay and a scope");
                }
                OverlayScope* scope = viewportOverlayScope(state->overlays, values->overlay);
                if (scope == nullptr) {
                    return OperatorResult::failed(std::string(overlayLabel(values->overlay)) +
                                                  " is shown or hidden, not scoped; use view3d.overlay_toggle");
                }
                *scope = values->scope;
                return OperatorResult::finished();
            }};
}

void drawScopeMenu(const char* label, OverlayScope& scope) {
    if (ImGui::BeginMenu(label)) {
        for (const ScopeName& name : scopeNames) {
            if (ImGui::MenuItem(name.label, nullptr, scope == name.scope)) {
                scope = name.scope;
            }
        }
        ImGui::EndMenu();
    }
}

Operator toggleOperator(std::string id, std::string label, std::string description, ViewportChromeTarget target,
                        bool ViewportChromeState::* flag) {
    return {.id = std::move(id),
            .label = std::move(label),
            .description = std::move(description),
            .poll = hasTarget(target),
            .execute = [target, flag](OperatorContext&, OperatorProperties&) {
                ViewportChromeState* state = target();
                if (state == nullptr) {
                    return OperatorResult::failed(noViewportError);
                }
                state->*flag = !(state->*flag);
                return OperatorResult::finished();
            }};
}

void drawSpaceChoice(TransformToolSettings& tools) {
    int space = static_cast<int>(tools.space);
    ImGui::TextUnformatted("Orientation");
    ImGui::RadioButton("Global", &space, static_cast<int>(TransformSpace::Global));
    ImGui::SameLine();
    ImGui::RadioButton("Local", &space, static_cast<int>(TransformSpace::Local));
    tools.space = static_cast<TransformSpace>(space);
}

void drawSnapTargetMenuItems(TransformToolSettings& tools) {
    for (const SnapTargetName& name : snapTargetNames) {
        if (ImGui::MenuItem(name.label, nullptr, tools.snapTarget == name.target)) {
            tools.snapTarget = name.target;
        }
    }
}

void drawPivotMenuItems(TransformToolSettings& tools) {
    for (const PivotName& name : pivotNames) {
        if (ImGui::MenuItem(name.label, nullptr, tools.pivot == name.pivot)) {
            tools.pivot = name.pivot;
        }
    }
}

void drawSnapIncrements(TransformToolSettings& tools) {
    // Scoped so the labels do not collide with the tool choices of the same name.
    ImGui::PushID("SnapIncrements");
    ImGui::DragFloat("Move", &tools.translationSnap, 0.05f, 0.001f, 1000.0f, "%.3f m");
    ImGui::DragFloat("Rotate", &tools.rotationSnapDegrees, 0.5f, 0.1f, 180.0f, "%.1f deg");
    ImGui::DragFloat("Scale", &tools.scaleSnap, 0.01f, 0.001f, 100.0f, "%.3f");
    ImGui::PopID();
}
} // namespace

float& TransformToolSettings::activeSnap() {
    if (gizmo == GizmoMode::Translate) {
        return translationSnap;
    }
    return gizmo == GizmoMode::Rotate ? rotationSnapDegrees : scaleSnap;
}

bool* viewportOverlayFlag(ViewportOverlays& overlays, ViewportOverlay overlay) {
    switch (overlay) {
    case ViewportOverlay::Grid:
        return &overlays.grid;
    case ViewportOverlay::SelectionOutline:
        return &overlays.selectionOutline;
    case ViewportOverlay::ActiveName:
        return &overlays.activeName;
    case ViewportOverlay::TransformGizmo:
        return &overlays.gizmo;
    case ViewportOverlay::SelectionBounds:
        return &overlays.scene.selectionBounds;
    case ViewportOverlay::SceneInPlay:
        return &overlays.sceneInPlay;
    case ViewportOverlay::Cursor3D:
        return &overlays.cursor;
    default:
        return nullptr;
    }
}

OverlayScope* viewportOverlayScope(ViewportOverlays& overlays, ViewportOverlay overlay) {
    switch (overlay) {
    case ViewportOverlay::Lights:
        return &overlays.scene.lights;
    case ViewportOverlay::Cameras:
        return &overlays.scene.cameras;
    case ViewportOverlay::Colliders:
        return &overlays.scene.colliders;
    case ViewportOverlay::ReflectionProbes:
        return &overlays.scene.reflectionProbes;
    case ViewportOverlay::Decals:
        return &overlays.scene.decals;
    case ViewportOverlay::IrradianceVolumes:
        return &overlays.scene.irradianceVolumes;
    case ViewportOverlay::Names:
        return &overlays.scene.names;
    default:
        return nullptr;
    }
}

std::string viewportAreaState(const ViewportChromeState& chrome) {
    // The lookups hand out mutable pointers; read them from a copy.
    ViewportOverlays overlays = chrome.overlays;
    nlohmann::json saved = nlohmann::json::object();
    for (const OverlayName& name : overlayNames) {
        if (const bool* flag = viewportOverlayFlag(overlays, name.overlay)) {
            saved[name.key] = *flag;
        } else if (const OverlayScope* scope = viewportOverlayScope(overlays, name.overlay)) {
            for (const ScopeName& scopeName : scopeNames) {
                if (scopeName.scope == *scope) {
                    saved[name.key] = scopeName.key;
                }
            }
        }
    }
    return nlohmann::json{{"overlays", std::move(saved)}}.dump();
}

void restoreViewportAreaState(std::string_view state, ViewportChromeState& chrome) {
    const nlohmann::json parsed = nlohmann::json::parse(state, nullptr, false);
    if (!parsed.is_object()) {
        return;
    }
    const auto saved = parsed.find("overlays");
    if (saved == parsed.end() || !saved->is_object()) {
        return;
    }
    for (const OverlayName& name : overlayNames) {
        const auto value = saved->find(name.key);
        if (value == saved->end()) {
            continue;
        }
        if (bool* flag = viewportOverlayFlag(chrome.overlays, name.overlay)) {
            if (value->is_boolean()) {
                *flag = value->get<bool>();
            }
        } else if (OverlayScope* scope = viewportOverlayScope(chrome.overlays, name.overlay)) {
            for (const ScopeName& scopeName : scopeNames) {
                if (value->is_string() && value->get<std::string>() == scopeName.key) {
                    *scope = scopeName.scope;
                }
            }
        }
    }
}

std::string viewportAreaState(const ViewportChromeState& chrome, const TransformToolSettings& tools) {
    nlohmann::json state = nlohmann::json::parse(viewportAreaState(chrome));
    state["tools"] = toolStateJson(tools);
    return state.dump();
}

void restoreViewportAreaState(std::string_view state, ViewportChromeState& chrome, TransformToolSettings& tools) {
    restoreViewportAreaState(state, chrome);
    const nlohmann::json parsed = nlohmann::json::parse(state, nullptr, false);
    if (!parsed.is_object()) {
        return;
    }
    if (const auto saved = parsed.find("tools"); saved != parsed.end() && saved->is_object()) {
        restoreToolState(*saved, tools);
    }
}

TransformSnapping transformSnapping(const TransformToolSettings& tools, bool invert) {
    return {.enabled = tools.snapping != invert,
            .translation = tools.translationSnap,
            .rotationDegrees = tools.rotationSnapDegrees,
            .scale = tools.scaleSnap,
            .target = tools.snapTarget,
            .absoluteGrid = tools.absoluteGridSnap};
}

const char* transformPivotLabel(TransformPivot pivot) {
    for (const PivotName& name : pivotNames) {
        if (name.pivot == pivot) {
            return name.label;
        }
    }
    return "Median Point";
}

const char* snapTargetLabel(SnapTarget target) {
    for (const SnapTargetName& name : snapTargetNames) {
        if (name.target == target) {
            return name.label;
        }
    }
    return "Increment";
}

std::vector<Operator> transformToolOperators(TransformToolTarget target) {
    const auto hasTools = [target](const OperatorContext&) { return target() != nullptr; };
    std::vector<Operator> operators;
    operators.push_back(
        {.id = "transform.pivot_set",
         .label = "Set Pivot Point",
         .description = "Choose what rotating and scaling turn about",
         .properties = OperatorProperties::make(
             PivotProperties{},
             std::move(Fields<PivotProperties>().add("pivot", &PivotProperties::pivot).options(pivotOptions()))),
         .poll = hasTools,
         .execute = [target](OperatorContext&, OperatorProperties& properties) {
             TransformToolSettings* tools = target();
             const PivotProperties* values = properties.get<PivotProperties>();
             if (tools == nullptr || values == nullptr) {
                 return OperatorResult::failed(noViewportError);
             }
             tools->pivot = values->pivot;
             return OperatorResult::finished();
         }});
    operators.push_back({.id = "transform.snap_target_set",
                         .label = "Set Snap Target",
                         .description = "Choose whether moving snaps to increments, surfaces, or vertices",
                         .properties = OperatorProperties::make(
                             SnapTargetProperties{}, std::move(Fields<SnapTargetProperties>()
                                                                   .add("target", &SnapTargetProperties::target)
                                                                   .options(snapTargetOptions()))),
                         .poll = hasTools,
                         .execute = [target](OperatorContext&, OperatorProperties& properties) {
                             TransformToolSettings* tools = target();
                             const SnapTargetProperties* values = properties.get<SnapTargetProperties>();
                             if (tools == nullptr || values == nullptr) {
                                 return OperatorResult::failed(noViewportError);
                             }
                             tools->snapTarget = values->target;
                             return OperatorResult::finished();
                         }});
    return operators;
}

const char* viewportShadingLabel(ViewportShading shading) {
    switch (shading) {
    case ViewportShading::Rendered:
        return "Rendered";
    case ViewportShading::MaterialPreview:
        return "Material Preview";
    case ViewportShading::Solid:
        return "Solid";
    case ViewportShading::Wireframe:
        return "Wireframe";
    }
    return "Rendered";
}

std::vector<Operator> viewportChromeOperators(ViewportChromeTarget target) {
    std::vector<Operator> operators;
    operators.push_back(toggleOperator("view3d.toggle_sidebar", "Toggle Sidebar",
                                       "Show or hide the viewport's Item, Tool, and View sidebar", target,
                                       &ViewportChromeState::sidebarVisible));
    operators.push_back(toggleOperator("view3d.toggle_toolbar", "Toggle Toolbar",
                                       "Show or hide the viewport's tool shelf", target,
                                       &ViewportChromeState::toolbarVisible));
    operators.push_back(
        {.id = "view3d.shading_set",
         .label = "Set Shading",
         .description = "Choose how the viewport shades the scene",
         .properties = OperatorProperties::make(
             ShadingProperties{},
             std::move(Fields<ShadingProperties>()
                           .add("type", &ShadingProperties::type)
                           .options({{"rendered", "Rendered", static_cast<int64_t>(ViewportShading::Rendered)},
                                     {"material_preview", "Material Preview",
                                      static_cast<int64_t>(ViewportShading::MaterialPreview)},
                                     {"solid", "Solid", static_cast<int64_t>(ViewportShading::Solid)},
                                     {"wireframe", "Wireframe", static_cast<int64_t>(ViewportShading::Wireframe)}}))),
         .poll = hasTarget(target),
         .execute = [target](OperatorContext&, OperatorProperties& properties) {
             ViewportChromeState* state = target();
             if (state == nullptr) {
                 return OperatorResult::failed(noViewportError);
             }
             const ShadingProperties* values = properties.get<ShadingProperties>();
             state->shading = values != nullptr ? values->type : ViewportShading::Rendered;
             return OperatorResult::finished();
         }});
    operators.push_back(overlayToggleOperator(target));
    operators.push_back(overlayScopeOperator(target));
    return operators;
}

namespace ViewportChromeWidgets {
void drawShadingMenu(ViewportChromeState& state) {
    for (const ViewportShading mode : shadingModes) {
        if (ImGui::MenuItem(viewportShadingLabel(mode), nullptr, state.shading == mode)) {
            state.shading = mode;
        }
    }
}

void drawOverlaysMenu(ViewportOverlays& overlays) {
    ImGui::MenuItem("Grid", nullptr, &overlays.grid);
    ImGui::MenuItem("Selection Outline", nullptr, &overlays.selectionOutline);
    ImGui::MenuItem("Active Object Name", nullptr, &overlays.activeName);
    ImGui::MenuItem("Transform Gizmo", nullptr, &overlays.gizmo);
    ImGui::MenuItem("3D Cursor", nullptr, &overlays.cursor);
    ImGui::SeparatorText("Scene");
    for (const OverlayName& name : overlayNames) {
        if (OverlayScope* scope = viewportOverlayScope(overlays, name.overlay)) {
            drawScopeMenu(name.label, *scope);
        }
    }
    ImGui::MenuItem("Selection Bounds", nullptr, &overlays.scene.selectionBounds);
    ImGui::MenuItem("Show in Play Mode", nullptr, &overlays.sceneInPlay);
    ImGui::SetItemTooltip("Draw the scene overlays while playing, except through a scene camera");
}

void drawGizmosMenu(TransformToolSettings& tools) {
    if (ImGui::MenuItem("Global", nullptr, tools.space == TransformSpace::Global)) {
        tools.space = TransformSpace::Global;
    }
    if (ImGui::MenuItem("Local", nullptr, tools.space == TransformSpace::Local)) {
        tools.space = TransformSpace::Local;
    }
    ImGui::SeparatorText("Pivot Point");
    drawPivotMenuItems(tools);
    ImGui::SeparatorText("Snapping");
    ImGui::MenuItem("Snapping", nullptr, &tools.snapping);
    if (ImGui::BeginMenu("Snap To")) {
        drawSnapTargetMenuItems(tools);
        ImGui::EndMenu();
    }
    ImGui::MenuItem("Absolute Grid Snap", nullptr, &tools.absoluteGridSnap);
    ImGui::SetItemTooltip("Increment snapping moves onto the world grid instead of in steps");
    ImGui::PushItemWidth(ImGui::GetFontSize() * 8.0f);
    drawSnapIncrements(tools);
    ImGui::PopItemWidth();
}

void drawToolShelf(TransformToolSettings& tools, float buttonWidth) {
    for (const ToolChoice& choice : toolChoices) {
        if (widgets::ToolButton(choice.label, choice.icon, tools.gizmo == choice.mode, {buttonWidth, buttonWidth})) {
            tools.gizmo = choice.mode;
        }
        ImGui::SetItemTooltip("%s: %s", choice.label, choice.description);
    }
}

void drawToolTab(TransformToolSettings& tools) {
    int tool = static_cast<int>(tools.gizmo);
    ImGui::TextUnformatted("Active Tool");
    for (const ToolChoice& choice : toolChoices) {
        ImGui::RadioButton(choice.label, &tool, static_cast<int>(choice.mode));
    }
    tools.gizmo = static_cast<GizmoMode>(tool);
    ImGui::Separator();
    drawSpaceChoice(tools);
    ImGui::Separator();
    int pivot = static_cast<int>(tools.pivot);
    ImGui::TextUnformatted("Pivot Point");
    for (const PivotName& name : pivotNames) {
        ImGui::RadioButton(name.label, &pivot, static_cast<int>(name.pivot));
    }
    tools.pivot = static_cast<TransformPivot>(pivot);
    ImGui::Separator();
    ImGui::Checkbox("Snapping", &tools.snapping);
    int snapTarget = static_cast<int>(tools.snapTarget);
    ImGui::PushID("SnapTo");
    for (const SnapTargetName& name : snapTargetNames) {
        ImGui::RadioButton(name.label, &snapTarget, static_cast<int>(name.target));
        ImGui::SameLine();
    }
    ImGui::NewLine();
    ImGui::PopID();
    tools.snapTarget = static_cast<SnapTarget>(snapTarget);
    ImGui::Checkbox("Absolute Grid Snap", &tools.absoluteGridSnap);
    drawSnapIncrements(tools);
}

void drawViewTab(EditorCamera& camera) {
    float fovDegrees = glm::degrees(camera.verticalFovRadians());
    if (ImGui::SliderFloat("Field of View", &fovDegrees, 5.0f, 150.0f, "%.1f deg")) {
        camera.setVerticalFovRadians(glm::radians(fovDegrees));
    }
    float clipStart = camera.nearPlane();
    float clipEnd = camera.farPlane();
    const bool clipStartChanged = ImGui::DragFloat("Clip Start", &clipStart, 0.005f, 0.0001f, 100.0f, "%.4f m");
    const bool clipEndChanged = ImGui::DragFloat("Clip End", &clipEnd, 1.0f, 0.01f, 1000000.0f, "%.1f m");
    if (clipStartChanged || clipEndChanged) {
        camera.setClipPlanes(clipStart, clipEnd);
    }
    float speed = camera.speed();
    if (ImGui::DragFloat("Fly Speed", &speed, 0.05f, 0.05f, 1000.0f, "%.2f m/s", ImGuiSliderFlags_Logarithmic)) {
        camera.setSpeed(speed);
    }
    ImGui::TextDisabled("Distance to pivot: %.2f m", camera.distance());
}
} // namespace ViewportChromeWidgets
