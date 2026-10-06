#include "editor/theme/EditorWidgets.hpp"

#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/PixelPainter.hpp"

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace widgets {
namespace {
ButtonFrame buttonFrame(bool active) {
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemActive() && hovered) {
        return ButtonFrame::Pressed;
    }
    if (active) {
        return ButtonFrame::Active;
    }
    return hovered ? ButtonFrame::Hovered : ButtonFrame::Normal;
}

// A square raised block: flat fill, a hard bevel, and a black outline.
void paintRaised(ImDrawList* drawList, ImVec2 min, ImVec2 max, ThemeColor fill, ThemeColor highlight,
                 ThemeColor shade) {
    const float artPixel = editorArtPixel();
    pixel::rect(drawList, min, max, color(fill));
    pixel::bevel(drawList, {min.x + artPixel, min.y + artPixel}, {max.x - artPixel, max.y - artPixel}, color(highlight),
                 color(shade), artPixel);
    pixel::outline(drawList, min, max, color(palette::outline), artPixel);
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
} // namespace

ImU32 color(ThemeColor themeColor) {
    return ImGui::GetColorU32(toImVec4(themeColor));
}

void PaintButtonFrame(ImDrawList* drawList, ImVec2 min, ImVec2 max, ButtonFrame frame) {
    using namespace palette;
    switch (frame) {
    case ButtonFrame::Normal:
        paintRaised(drawList, min, max, button, bevelHighlight, bevelShade);
        break;
    case ButtonFrame::Hovered:
        paintRaised(drawList, min, max, buttonHovered, bevelHighlight, bevelShade);
        break;
    case ButtonFrame::Pressed:
        // Pressed in: the shadow falls along the top and left edges.
        paintRaised(drawList, min, max, buttonPressed, bevelShade, buttonPressed);
        break;
    case ButtonFrame::Active:
        paintRaised(drawList, min, max, buttonActive, buttonActiveHighlight, buttonActiveShade);
        break;
    case ButtonFrame::Disabled:
        // Flat and sunk into the pane: nothing to press.
        pixel::rect(drawList, min, max, color(pane));
        pixel::outline(drawList, min, max, color(outline), editorArtPixel());
        break;
    }
}

bool Button(const char* label, ImVec2 size, bool active) {
    const bool pressed = invisibly([&] { return ImGui::Button(label, size); });
    if (!ImGui::IsItemVisible()) {
        return pressed;
    }
    const ButtonFrame frame = buttonFrame(active);
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    PaintButtonFrame(ImGui::GetWindowDrawList(), min, max, frame);
    drawButtonLabel(min, max, label, frame == ButtonFrame::Active ? palette::activeInk : palette::ink,
                    frame == ButtonFrame::Pressed ? editorArtPixel() : 0.0f);
    return pressed;
}

bool CopyableText(const std::string& text, std::optional<ThemeColor> ink) {
    if (ink) {
        ImGui::PushStyleColor(ImGuiCol_Text, toImVec4(*ink));
    }
    ImGui::TextWrapped("%s", text.c_str());
    if (ink) {
        ImGui::PopStyleColor();
    }
    if (text.empty()) {
        return false;
    }
    // Plain text has no id, so the popup is keyed by the text itself.
    ImGui::PushID(text.c_str(), text.c_str() + text.size());
    bool copied = ImGui::IsItemHovered() && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C);
    if (ImGui::BeginPopupContextItem("##copy_text")) {
        if (ImGui::MenuItem("Copy")) {
            copied = true;
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    if (copied) {
        ImGui::SetClipboardText(text.c_str());
    }
    return copied;
}

bool ToolButton(const char* label, EditorIcon icon, bool active, ImVec2 size) {
    const bool pressed = ImGui::InvisibleButton(label, size);
    if (!ImGui::IsItemVisible()) {
        return pressed;
    }
    const ButtonFrame frame = buttonFrame(active);
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    PaintButtonFrame(drawList, min, max, frame);
    const float artPixel = editorArtPixel();
    // The largest whole pixel size that leaves a two-pixel margin.
    const float side = std::min(max.x - min.x, max.y - min.y);
    const float pixelSize = std::max(1.0f, std::floor((side - 4.0f * artPixel) / static_cast<float>(iconBitmapSize)));
    const float extent = pixelSize * static_cast<float>(iconBitmapSize);
    const float shift = frame == ButtonFrame::Pressed ? artPixel : 0.0f;
    drawIcon(drawList, icon,
             {std::floor((min.x + max.x - extent) / 2.0f) + shift, std::floor((min.y + max.y - extent) / 2.0f) + shift},
             pixelSize, {.disabled = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0});
    return pressed;
}

bool SectionHeader(const char* label, ImGuiTreeNodeFlags flags) {
    const bool open = invisibly([&] { return ImGui::CollapsingHeader(label, flags); });
    if (ImGui::IsItemVisible()) {
        using namespace palette;
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        paintRaised(drawList, min, max, ImGui::IsItemHovered() ? sectionHovered : section, bevelHighlight, bevelShade);
        const ImGuiStyle& style = ImGui::GetStyle();
        const float fontSize = ImGui::GetFontSize();
        const float textY = std::floor((min.y + max.y - fontSize) / 2.0f);
        // The chevron is two thirds of a line, like the font's capitals.
        const float chevron = std::floor(fontSize * 2.0f / 3.0f);
        pixel::triangle(drawList, {min.x + style.FramePadding.x, std::floor((min.y + max.y - chevron) / 2.0f)}, chevron,
                        open ? ImGuiDir_Down : ImGuiDir_Right, color(sectionInk));
        drawList->AddText({min.x + chevron + style.FramePadding.x * 3.0f, textY}, color(sectionInk), label,
                          ImGui::FindRenderedTextEnd(label));
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

void StatusPill(EditorIcon icon, const char* text, StatusPillStyle style) {
    using namespace palette;
    const float artPixel = editorArtPixel();
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    const float extent = iconSize(artPixel);
    const ImVec2 size{artPixel * 11.0f + extent + textSize.x, ImGui::GetFrameHeight()};
    const ImVec2 min = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    const ImVec2 max{min.x + size.x, min.y + size.y};
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    if (style == StatusPillStyle::Lit) {
        paintRaised(drawList, min, max, buttonActive, buttonActiveHighlight, buttonActiveShade);
    } else {
        paintRaised(drawList, min, max, button, bevelHighlight, bevelShade);
    }
    drawIcon(drawList, icon, {min.x + artPixel * 4.0f, std::floor((min.y + max.y - extent) / 2.0f)}, artPixel);
    const ThemeColor textInk = style == StatusPillStyle::Lit       ? activeInk
                               : style == StatusPillStyle::Warning ? warning
                                                                   : ink;
    drawList->AddText({min.x + artPixel * 7.0f + extent, std::floor((min.y + max.y - textSize.y) / 2.0f)},
                      color(textInk), text);
}

void CaptionText(const char* text, ThemeColor ink) {
    const float artPixel = editorArtPixel();
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    const float height = ImGui::GetFrameHeight();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    ImGui::Dummy({textSize.x + artPixel * 2.0f, height});
    ImGui::GetWindowDrawList()->AddText(pixel::snap({min.x + artPixel, min.y + (height - textSize.y) / 2.0f}),
                                        color(ink), text);
}

} // namespace widgets
