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
void paintPane(ImDrawList* drawList, ImVec2 min, ImVec2 max, float artPixel, const ThemeParameters& parameters) {
    using namespace palette;
    const ThemeColor fill = paneColor(parameters);
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

void ThemeChrome::beginFrame(RenderRuntime& rendering) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    m_backdrop.update(rendering, viewport->Pos, viewport->Size, editorArtPixel(), m_parameters);
    m_backdrop.drawWallpaper(ImGui::GetBackgroundDrawList(ImGui::GetMainViewport()));
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
        paintPane(background, paneMin, paneMax, artPixel, m_parameters);
        return;
    }
    // A floating area keeps ImGui's title bar; its body becomes the pane and
    // its menu bar a glass strip.
    ImDrawList* drawList = window->DrawList;
    const float bodyTop = window->Pos.y + window->TitleBarHeight;
    if (window->InnerRect.Min.y > bodyTop) {
        paintGlassPanel(drawList, {windowMin.x, bodyTop}, {windowMax.x, window->InnerRect.Min.y});
    }
    paintPane(drawList, {windowMin.x, window->InnerRect.Min.y}, windowMax, artPixel, m_parameters);
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
    pixel::glintStripes(drawList, {min.x + artPixel, min.y + artPixel}, {max.x - artPixel, max.y - artPixel},
                        u32(glassGlint), u32(glassGlintFaint), artPixel);
    pixel::bevel(drawList, {min.x + artPixel, min.y + artPixel}, {max.x - artPixel, max.y - artPixel},
                 u32(glassHighlight), u32(glassShade), artPixel);
    pixel::notchedOutline(drawList, min, max, u32(glassBorder), artPixel);
}
