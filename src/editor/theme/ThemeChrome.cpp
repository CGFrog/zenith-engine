#include "editor/theme/ThemeChrome.hpp"

#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/PixelPainter.hpp"

#include <imgui_internal.h>

#include <algorithm>

namespace {
ImU32 u32(ThemeColor color) {
    return toImU32(color);
}

void paintPopupBevels(ImGuiContext* context, ImGuiContextHook*) {
    const float artPixel = editorArtPixel();
    constexpr ImGuiWindowFlags popupFlags =
        ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_ChildMenu;
    for (ImGuiWindow* window : context->Windows) {
        if (!window->Active || window->Hidden || (window->Flags & popupFlags) == 0) {
            continue;
        }
        const ImVec2 min = window->Pos;
        const ImVec2 max{window->Pos.x + window->Size.x, window->Pos.y + window->Size.y};
        // Inside the window border, over the contents' edge.
        window->DrawList->PushClipRect(min, max, false);
        pixel::bevel(window->DrawList, {min.x + artPixel, min.y + artPixel}, {max.x - artPixel, max.y - artPixel},
                     u32(palette::bevelHighlight), u32(palette::bevelShade), artPixel);
        window->DrawList->PopClipRect();
    }
}
} // namespace

void ThemeChrome::setParameters(const ThemeParameters& parameters) {
    m_parameters = clampedThemeParameters(parameters);
}

void ThemeChrome::setModeTint(ThemeModeTint tint) {
    m_modeTint = tint;
}

void ThemeChrome::beginFrame() const {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 max{viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y};
    ImGui::GetBackgroundDrawList(viewport)->AddRectFilled(viewport->Pos, max, u32(tinted(palette::areaGap)));
    const std::optional<ThemeColor> tint = themeModeTintColor(m_modeTint);
    if (!tint) {
        return;
    }
    // A solid edge in the tint's hue around the whole window, over every
    // window, so the mode reads even where panes fill the screen.
    const float edge = editorArtPixel() * 2.0f;
    ImDrawList* foreground = ImGui::GetForegroundDrawList(viewport);
    const ImU32 edgeColor = u32(tint->withAlpha(1.0f));
    foreground->AddRectFilled(viewport->Pos, {max.x, viewport->Pos.y + edge}, edgeColor);
    foreground->AddRectFilled({viewport->Pos.x, max.y - edge}, max, edgeColor);
    foreground->AddRectFilled(viewport->Pos, {viewport->Pos.x + edge, max.y}, edgeColor);
    foreground->AddRectFilled({max.x - edge, viewport->Pos.y}, max, edgeColor);
}

void ThemeChrome::paintAreaWindow() const {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImVec2 windowMax{window->Pos.x + window->Size.x, window->Pos.y + window->Size.y};
    const float paneTop = window->InnerRect.Min.y;
    if (window->DockIsActive && window->DockNode != nullptr) {
        // Docked areas never overlap, so they can be painted behind every
        // window, under the dock node's tab bar.
        ImDrawList* background = ImGui::GetBackgroundDrawList(window->Viewport);
        const ImRect node = window->DockNode->Rect();
        paintHeader(background, node.Min, {node.Max.x, paneTop});
        paintPane(background, {node.Min.x, paneTop}, node.Max);
        pixel::outline(background, node.Min, node.Max, u32(palette::outline), editorArtPixel());
        return;
    }
    // A floating area keeps ImGui's title bar; its menu bar becomes a header
    // strip and its body the pane.
    ImDrawList* drawList = window->DrawList;
    const float bodyTop = window->Pos.y + window->TitleBarHeight;
    if (paneTop > bodyTop) {
        paintHeader(drawList, {window->Pos.x, bodyTop}, {windowMax.x, paneTop});
    }
    paintPane(drawList, {window->Pos.x, paneTop}, windowMax);
}

void ThemeChrome::paintBarWindow() const {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImVec2 max{window->Pos.x + window->Size.x, window->Pos.y + window->Size.y};
    ImDrawList* background = ImGui::GetBackgroundDrawList(window->Viewport);
    paintHeader(background, window->Pos, max);
    pixel::outline(background, window->Pos, max, u32(palette::outline), editorArtPixel());
}

void ThemeChrome::paintOverlayPanel(ImDrawList* drawList, ImVec2 min, ImVec2 max) {
    using namespace palette;
    const float artPixel = editorArtPixel();
    pixel::rect(drawList, min, max, u32(overlayPanel));
    pixel::bevel(drawList, {min.x + artPixel, min.y + artPixel}, {max.x - artPixel, max.y - artPixel},
                 u32(headerHighlight), u32(bevelShade), artPixel);
    pixel::outline(drawList, min, max, u32(outline), artPixel);
}

void ThemeChrome::installPopupBevels(ImGuiContext& context) {
    const ImGuiID owner = ImHashStr("zenith.theme.popup_bevels");
    const bool installed =
        std::ranges::any_of(context.Hooks, [owner](const ImGuiContextHook& hook) { return hook.Owner == owner; });
    if (installed) {
        return;
    }
    ImGuiContextHook hook;
    hook.Type = ImGuiContextHookType_EndFramePre;
    hook.Owner = owner;
    hook.Callback = paintPopupBevels;
    ImGui::AddContextHook(&context, &hook);
}

// A flat strip with a one-art-pixel highlight along its top edge, as
// Blender's region headers.
void ThemeChrome::paintHeader(ImDrawList* drawList, ImVec2 min, ImVec2 max) const {
    const float artPixel = editorArtPixel();
    pixel::rect(drawList, min, max, u32(tinted(palette::header)));
    pixel::rect(drawList, min, {max.x, min.y + artPixel}, u32(tinted(palette::headerHighlight)));
}

void ThemeChrome::paintPane(ImDrawList* drawList, ImVec2 min, ImVec2 max) const {
    pixel::rect(drawList, min, max, u32(tinted(palette::pane)));
}

ThemeColor ThemeChrome::tinted(ThemeColor color) const {
    if (const std::optional<ThemeColor> tint = themeModeTintColor(m_modeTint)) {
        return composite(*tint, color.withAlpha(1.0f)).withAlpha(color.a);
    }
    return color;
}
