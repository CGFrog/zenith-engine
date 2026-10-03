#include "editor/theme/EditorWidgets.hpp"

#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/PixelPainter.hpp"

#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>

namespace widgets {
namespace {
enum class ButtonState {
    Normal,
    Hovered,
    Pressed,
    Active,
};

ButtonState buttonState(bool active) {
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemActive() && hovered) {
        return ButtonState::Pressed;
    }
    if (active) {
        return ButtonState::Active;
    }
    return hovered ? ButtonState::Hovered : ButtonState::Normal;
}

void paintButton(ImDrawList* drawList, ImVec2 min, ImVec2 max, ButtonState state) {
    using namespace palette;
    const float artPixel = editorArtPixel();
    switch (state) {
    case ButtonState::Normal:
        pixel::splitGloss(drawList, min, max, color(buttonTop), color(buttonBottom), artPixel);
        break;
    case ButtonState::Hovered:
        pixel::splitGloss(drawList, min, max, color(buttonHoverTop), color(buttonHoverBottom), artPixel);
        break;
    case ButtonState::Pressed:
        pixel::notchedRect(drawList, min, max, color(buttonBottom), artPixel);
        break;
    case ButtonState::Active:
        pixel::splitGloss(drawList, min, max, color(buttonActiveTop), color(buttonActiveBottom), artPixel);
        break;
    }
    const ImVec2 innerMin{min.x + artPixel, min.y + artPixel};
    const ImVec2 innerMax{max.x - artPixel, max.y - artPixel};
    if (state == ButtonState::Pressed) {
        // Pressed in: the shadow falls along the top and left edges.
        pixel::bevel(drawList, innerMin, innerMax, color(glassShade), color(buttonBottom), artPixel);
    } else {
        pixel::bevel(drawList, innerMin, innerMax, color(buttonHighlight), color(glassShade), artPixel);
    }
    pixel::notchedOutline(drawList, min, max, color(glassBorder), artPixel);
    if (state == ButtonState::Hovered || state == ButtonState::Active) {
        pixel::glowRing(drawList, min, max, color(pixelGlow), artPixel);
    }
}

// Calls an ImGui widget with its frame and text drawn invisibly.
template <typename Widget> bool invisibly(Widget&& widget) {
    const ImVec4 transparent = toImVec4(palette::transparent);
    for (const ImGuiCol slot : {ImGuiCol_Button, ImGuiCol_ButtonHovered, ImGuiCol_ButtonActive, ImGuiCol_Header,
                                ImGuiCol_HeaderHovered, ImGuiCol_HeaderActive, ImGuiCol_Text}) {
        ImGui::PushStyleColor(slot, transparent);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    const bool result = widget();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(7);
    return result;
}

// The label as ImGui::Button places it, optionally nudged.
void drawButtonLabel(ImVec2 min, ImVec2 max, const char* label, ThemeColor ink, float shift) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const char* labelEnd = ImGui::FindRenderedTextEnd(label);
    const ImVec2 labelSize = ImGui::CalcTextSize(label, labelEnd, true);
    const ImRect bounds(min, max);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color(ink)));
    ImGui::RenderTextClipped({min.x + style.FramePadding.x + shift, min.y + style.FramePadding.y + shift},
                             {max.x - style.FramePadding.x + shift, max.y - style.FramePadding.y + shift}, label,
                             labelEnd, &labelSize, style.ButtonTextAlign, &bounds);
    ImGui::PopStyleColor();
}

struct TitleText {
    ImFont* font;
    float size;
};

// The title font, or the current font without the pixel fonts.
TitleText titleText() {
    ImFont* title = editorTitleFont();
    if (title != nullptr) {
        return {title, title->FontSize};
    }
    return {ImGui::GetFont(), ImGui::GetFontSize()};
}
} // namespace

ImU32 color(ThemeColor themeColor) {
    return ImGui::GetColorU32(toImVec4(themeColor));
}

bool Button(const char* label, ImVec2 size, bool active) {
    const bool pressed = invisibly([&] { return ImGui::Button(label, size); });
    if (!ImGui::IsItemVisible()) {
        return pressed;
    }
    const ButtonState state = buttonState(active);
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    paintButton(ImGui::GetWindowDrawList(), min, max, state);
    drawButtonLabel(min, max, label, state == ButtonState::Active ? palette::activeInk : palette::ink,
                    state == ButtonState::Pressed ? editorArtPixel() : 0.0f);
    return pressed;
}

bool ToolButton(const char* label, EditorIcon icon, bool active, ImVec2 size) {
    const bool pressed = ImGui::InvisibleButton(label, size);
    if (!ImGui::IsItemVisible()) {
        return pressed;
    }
    const ButtonState state = buttonState(active);
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    paintButton(drawList, min, max, state);
    const float artPixel = editorArtPixel();
    // The largest whole pixel size that leaves a two-pixel margin.
    const float side = std::min(max.x - min.x, max.y - min.y);
    const float pixelSize = std::max(1.0f, std::floor((side - 4.0f * artPixel) / static_cast<float>(iconBitmapSize)));
    const float extent = pixelSize * static_cast<float>(iconBitmapSize);
    const float shift = state == ButtonState::Pressed ? artPixel : 0.0f;
    drawIcon(drawList, icon,
             {std::floor((min.x + max.x - extent) / 2.0f) + shift, std::floor((min.y + max.y - extent) / 2.0f) + shift},
             pixelSize, {.disabled = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0});
    return pressed;
}

bool SectionHeader(const char* label, ImGuiTreeNodeFlags flags) {
    ImFont* title = editorTitleFont();
    if (title != nullptr) {
        ImGui::PushFont(title);
    }
    const bool open = invisibly([&] { return ImGui::CollapsingHeader(label, flags); });
    if (ImGui::IsItemVisible()) {
        using namespace palette;
        const float artPixel = editorArtPixel();
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        pixel::splitGloss(drawList, min, max, color(hovered ? buttonTop : sectionTop),
                          color(hovered ? buttonBottom : sectionBottom), 0.0f);
        drawList->AddRectFilled(min, {max.x, min.y + artPixel}, color(buttonHighlight));
        drawList->AddRectFilled({min.x, max.y - artPixel}, max, color(glassBorder));
        const ImGuiStyle& style = ImGui::GetStyle();
        const float fontSize = ImGui::GetFontSize();
        const float textY = std::floor((min.y + max.y - fontSize) / 2.0f);
        pixel::triangle(drawList, {min.x + style.FramePadding.x, textY}, fontSize,
                        open ? ImGuiDir_Down : ImGuiDir_Right, color(sectionInk));
        drawList->AddText({min.x + fontSize + style.FramePadding.x * 3.0f, textY}, color(sectionInk), label,
                          ImGui::FindRenderedTextEnd(label));
    }
    if (title != nullptr) {
        ImGui::PopFont();
    }
    return open;
}

void ItemIcon(EditorIcon icon, float labelOffset, IconState state) {
    const float artPixel = editorArtPixel();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const float extent = iconSize(artPixel);
    drawIcon(ImGui::GetWindowDrawList(), icon, {min.x + labelOffset, std::floor((min.y + max.y - extent) / 2.0f)},
             artPixel, state);
}

const char* IconLabelPadding() {
    // Rebuilt when the font or art pixel changes the space needed.
    static std::string padding;
    const float spaceWidth = std::max(1.0f, ImGui::CalcTextSize(" ").x);
    const float wanted = iconSize(editorArtPixel()) + editorArtPixel() * 3.0f;
    const auto spaces = static_cast<size_t>(std::ceil(wanted / spaceWidth));
    if (padding.size() != spaces) {
        padding.assign(spaces, ' ');
    }
    return padding.c_str();
}

void StatusPill(EditorIcon icon, const char* text, ThemeColor top, ThemeColor bottom, ThemeColor ink) {
    const float artPixel = editorArtPixel();
    const TitleText title = titleText();
    const ImVec2 textSize = title.font->CalcTextSizeA(title.size, FLT_MAX, 0.0f, text);
    const float extent = iconSize(artPixel);
    const ImVec2 size{artPixel * 11.0f + extent + textSize.x, ImGui::GetFrameHeight()};
    const ImVec2 min = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    const ImVec2 max{min.x + size.x, min.y + size.y};
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    pixel::splitGloss(drawList, min, max, color(top), color(bottom), artPixel);
    pixel::notchedOutline(drawList, min, max, color(palette::glassBorder), artPixel);
    drawIcon(drawList, icon, {min.x + artPixel * 4.0f, std::floor((min.y + max.y - extent) / 2.0f)}, artPixel);
    drawList->AddText(title.font, title.size,
                      {min.x + artPixel * 7.0f + extent, std::floor((min.y + max.y - textSize.y) / 2.0f)}, color(ink),
                      text);
}

void CaptionText(const char* text, ThemeColor ink) {
    const float artPixel = editorArtPixel();
    const TitleText title = titleText();
    const ImVec2 textSize = title.font->CalcTextSizeA(title.size, FLT_MAX, 0.0f, text);
    const float height = ImGui::GetFrameHeight();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    ImGui::Dummy({textSize.x + artPixel * 2.0f, height});
    pixel::outlinedText(ImGui::GetWindowDrawList(), title.font, title.size,
                        {min.x + artPixel, std::floor(min.y + (height - textSize.y) / 2.0f)}, color(ink),
                        color(palette::captionOutline), text, artPixel);
}

} // namespace widgets
