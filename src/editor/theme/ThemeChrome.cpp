#include "editor/theme/ThemeChrome.hpp"

#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/PixelPainter.hpp"

#include <imgui_internal.h>

#include <algorithm>

namespace {
ImU32 u32(ThemeColor color) {
    return toImU32(color);
}

// A solid content pane: notched, outlined, with a shadow along its top and
// left edges so it reads as set into the glass.
// Below full opacity the frosted glass behind shows through the pane.
// A mode tint is composited into the pane colour, keeping its opacity.
void paintPane(ImDrawList* drawList, ImVec2 min, ImVec2 max, float artPixel, const ThemeParameters& parameters,
               ThemeModeTint tint) {
    using namespace palette;
    ThemeColor fill = paneColor(parameters);
    if (const std::optional<ThemeColor> tintColor = themeModeTintColor(tint)) {
        fill = composite(*tintColor, fill.withAlpha(1.0f)).withAlpha(fill.a);
    }
    pixel::notchedRect(drawList, min, max, u32(fill), artPixel);
    pixel::bevel(drawList, {min.x + artPixel, min.y + artPixel}, {max.x - artPixel, max.y - artPixel},
                 u32(paneShadow.withAlpha(fill.a)), u32(fill), artPixel);
    pixel::notchedOutline(drawList, min, max, u32(glassBorder), artPixel);
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
                     u32(palette::glassHighlight), u32(palette::glassShade), artPixel);
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

void ThemeChrome::beginFrame(RenderRuntime& rendering) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    m_backdrop.update(rendering, viewport->Pos, viewport->Size, editorArtPixel(), m_parameters);
    ImDrawList* background = ImGui::GetBackgroundDrawList(viewport);
    m_backdrop.drawWallpaper(background);
    const std::optional<ThemeColor> tint = themeModeTintColor(m_modeTint);
    if (!tint) {
        return;
    }
    const ImVec2 max{viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y};
    background->AddRectFilled(viewport->Pos, max, u32(*tint));
    // A solid edge in the tint's hue around the whole window, over every
    // window, so the mode reads even where panes cover the wallpaper.
    const float edge = editorArtPixel() * 2.0f;
    ImDrawList* foreground = ImGui::GetForegroundDrawList(viewport);
    const ImU32 edgeColor = u32(tint->withAlpha(1.0f));
    foreground->AddRectFilled(viewport->Pos, {max.x, viewport->Pos.y + edge}, edgeColor);
    foreground->AddRectFilled({viewport->Pos.x, max.y - edge}, max, edgeColor);
    foreground->AddRectFilled(viewport->Pos, {viewport->Pos.x + edge, max.y}, edgeColor);
    foreground->AddRectFilled({max.x - edge, viewport->Pos.y}, max, edgeColor);
}

void ThemeChrome::release(RenderRuntime& rendering) {
    m_backdrop.release(rendering);
}

void ThemeChrome::paintAreaWindow() const {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const float artPixel = editorArtPixel();
    const float rim = artPixel * 2.0f;
    const ImVec2 windowMin = window->Pos;
    const ImVec2 windowMax{window->Pos.x + window->Size.x, window->Pos.y + window->Size.y};
    const ImVec2 paneMin{windowMin.x + rim, window->InnerRect.Min.y};
    const ImVec2 paneMax{windowMax.x - rim, windowMax.y - rim};
    if (window->DockIsActive && window->DockNode != nullptr) {
        // Docked areas never overlap, so their frames can sit behind every
        // window, under the dock node's tab bar.
        ImDrawList* background = ImGui::GetBackgroundDrawList(window->Viewport);
        const ImRect node = window->DockNode->Rect();
        paintGlassPanel(background, node.Min, node.Max);
        paintPane(background, paneMin, paneMax, artPixel, m_parameters, m_modeTint);
        return;
    }
    // A floating area keeps ImGui's title bar; its body becomes the pane and
    // its menu bar a glass strip.
    ImDrawList* drawList = window->DrawList;
    const float bodyTop = window->Pos.y + window->TitleBarHeight;
    if (window->InnerRect.Min.y > bodyTop) {
        paintGlassPanel(drawList, {windowMin.x, bodyTop}, {windowMax.x, window->InnerRect.Min.y});
    }
    paintPane(drawList, {windowMin.x, window->InnerRect.Min.y}, windowMax, artPixel, m_parameters, m_modeTint);
}

void ThemeChrome::paintBarWindow() const {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    paintGlassPanel(ImGui::GetBackgroundDrawList(window->Viewport), window->Pos,
                    {window->Pos.x + window->Size.x, window->Pos.y + window->Size.y});
}

void ThemeChrome::paintSmokedGlass(ImDrawList* drawList, ImVec2 min, ImVec2 max) {
    using namespace palette;
    const float artPixel = editorArtPixel();
    pixel::notchedRect(drawList, min, max, u32(smokedGlass), artPixel);
    pixel::bevel(drawList, {min.x + artPixel, min.y + artPixel}, {max.x - artPixel, max.y - artPixel},
                 u32(smokedHighlight), u32(glassShade), artPixel);
    pixel::notchedOutline(drawList, min, max, u32(glassBorder), artPixel);
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

void ThemeChrome::paintGlassPanel(ImDrawList* drawList, ImVec2 min, ImVec2 max) const {
    using namespace palette;
    const float artPixel = editorArtPixel();
    m_backdrop.drawFrosted(drawList, min, max, artPixel);
    if (const std::optional<ThemeColor> tint = themeModeTintColor(m_modeTint)) {
        pixel::notchedRect(drawList, min, max, u32(*tint), artPixel);
    }
    pixel::glintStripes(drawList, {min.x + artPixel, min.y + artPixel}, {max.x - artPixel, max.y - artPixel},
                        u32(glassGlint), u32(glassGlintFaint), artPixel);
    pixel::bevel(drawList, {min.x + artPixel, min.y + artPixel}, {max.x - artPixel, max.y - artPixel},
                 u32(glassHighlight), u32(glassShade), artPixel);
    pixel::notchedOutline(drawList, min, max, u32(glassBorder), artPixel);
}
