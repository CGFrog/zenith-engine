#include "EditorTheme.hpp"

#include <imgui.h>

#include <cstdint>
#include <string>
#include <system_error>

namespace {
constexpr ImVec4 rgb(uint32_t hex, float alpha = 1.0f) {
    return {static_cast<float>((hex >> 16u) & 0xffu) / 255.0f, static_cast<float>((hex >> 8u) & 0xffu) / 255.0f,
            static_cast<float>(hex & 0xffu) / 255.0f, alpha};
}

// Blender 4.x default theme values.
constexpr uint32_t gap = 0x161616;         // Space between editor areas.
constexpr uint32_t field = 0x1d1d1d;       // Text and number fields.
constexpr uint32_t header = 0x232323;      // Title and menu bars.
constexpr uint32_t tab = 0x2b2b2b;         // Inactive tabs and table headers.
constexpr uint32_t region = 0x303030;      // Panel body.
constexpr uint32_t subPanel = 0x3d3d3d;    // Collapsing headers and list rows.
constexpr uint32_t widgetHover = 0x474747; // Hovered list rows.
constexpr uint32_t button = 0x545454;      // Push buttons.
constexpr uint32_t buttonHover = 0x656565; // Hovered push buttons.
constexpr uint32_t accent = 0x4772b3;      // Selection and active state.
constexpr uint32_t accentHover = 0x5680c2; // Hovered active state.
constexpr uint32_t text = 0xe6e6e6;
constexpr uint32_t textDisabled = 0x8c8c8c;
} // namespace

void applyEditorTheme(ImGuiStyle& style) {
    style = ImGuiStyle();

    style.WindowPadding = {8.0f, 6.0f};
    style.FramePadding = {6.0f, 3.0f};
    style.CellPadding = {4.0f, 2.0f};
    style.ItemSpacing = {6.0f, 4.0f};
    style.ItemInnerSpacing = {4.0f, 4.0f};
    style.IndentSpacing = 14.0f;
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 10.0f;

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.TabBorderSize = 0.0f;
    style.TabBarBorderSize = 1.0f;
    style.TabBarOverlineSize = 2.0f;
    style.SeparatorTextBorderSize = 1.0f;
    style.DockingSeparatorSize = 2.0f;

    style.WindowRounding = 4.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 3.0f;

    style.WindowTitleAlign = {0.5f, 0.5f};
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.ColorButtonPosition = ImGuiDir_Left;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = rgb(text);
    colors[ImGuiCol_TextDisabled] = rgb(textDisabled);
    colors[ImGuiCol_WindowBg] = rgb(region);
    colors[ImGuiCol_ChildBg] = rgb(region, 0.0f);
    colors[ImGuiCol_PopupBg] = rgb(0x181818, 0.98f);
    colors[ImGuiCol_Border] = rgb(gap);
    colors[ImGuiCol_BorderShadow] = rgb(0x000000, 0.0f);
    colors[ImGuiCol_FrameBg] = rgb(field);
    colors[ImGuiCol_FrameBgHovered] = rgb(0x262626);
    colors[ImGuiCol_FrameBgActive] = rgb(0x2c2c2c);
    colors[ImGuiCol_TitleBg] = rgb(header);
    colors[ImGuiCol_TitleBgActive] = rgb(header);
    colors[ImGuiCol_TitleBgCollapsed] = rgb(header);
    colors[ImGuiCol_MenuBarBg] = rgb(header);
    colors[ImGuiCol_ScrollbarBg] = rgb(region, 0.0f);
    colors[ImGuiCol_ScrollbarGrab] = rgb(0x4d4d4d);
    colors[ImGuiCol_ScrollbarGrabHovered] = rgb(0x5c5c5c);
    colors[ImGuiCol_ScrollbarGrabActive] = rgb(0x6b6b6b);
    colors[ImGuiCol_CheckMark] = rgb(0xffffff);
    colors[ImGuiCol_SliderGrab] = rgb(accent);
    colors[ImGuiCol_SliderGrabActive] = rgb(accentHover);
    colors[ImGuiCol_Button] = rgb(button);
    colors[ImGuiCol_ButtonHovered] = rgb(buttonHover);
    colors[ImGuiCol_ButtonActive] = rgb(accent);
    colors[ImGuiCol_Header] = rgb(subPanel);
    colors[ImGuiCol_HeaderHovered] = rgb(widgetHover);
    colors[ImGuiCol_HeaderActive] = rgb(accent);
    colors[ImGuiCol_Separator] = rgb(gap);
    colors[ImGuiCol_SeparatorHovered] = rgb(accent, 0.8f);
    colors[ImGuiCol_SeparatorActive] = rgb(accent);
    colors[ImGuiCol_ResizeGrip] = rgb(button, 0.25f);
    colors[ImGuiCol_ResizeGripHovered] = rgb(accent, 0.6f);
    colors[ImGuiCol_ResizeGripActive] = rgb(accent);
    colors[ImGuiCol_Tab] = rgb(tab);
    colors[ImGuiCol_TabHovered] = rgb(widgetHover);
    colors[ImGuiCol_TabSelected] = rgb(region);
    colors[ImGuiCol_TabSelectedOverline] = rgb(accent);
    colors[ImGuiCol_TabDimmed] = rgb(header);
    colors[ImGuiCol_TabDimmedSelected] = rgb(region);
    colors[ImGuiCol_TabDimmedSelectedOverline] = rgb(accent, 0.0f);
    colors[ImGuiCol_DockingPreview] = rgb(accent, 0.7f);
    colors[ImGuiCol_DockingEmptyBg] = rgb(gap);
    colors[ImGuiCol_PlotLines] = rgb(0xa6a6a6);
    colors[ImGuiCol_PlotLinesHovered] = rgb(accentHover);
    colors[ImGuiCol_PlotHistogram] = rgb(accent);
    colors[ImGuiCol_PlotHistogramHovered] = rgb(accentHover);
    colors[ImGuiCol_TableHeaderBg] = rgb(tab);
    colors[ImGuiCol_TableBorderStrong] = rgb(gap);
    colors[ImGuiCol_TableBorderLight] = rgb(0x262626);
    colors[ImGuiCol_TableRowBg] = rgb(0x000000, 0.0f);
    colors[ImGuiCol_TableRowBgAlt] = rgb(0xffffff, 0.03f);
    colors[ImGuiCol_TextLink] = rgb(accentHover);
    colors[ImGuiCol_TextSelectedBg] = rgb(accent, 0.5f);
    colors[ImGuiCol_DragDropTarget] = rgb(accentHover);
    colors[ImGuiCol_NavCursor] = rgb(accent);
    colors[ImGuiCol_NavWindowingHighlight] = rgb(0xffffff, 0.7f);
    colors[ImGuiCol_NavWindowingDimBg] = rgb(0x000000, 0.3f);
    colors[ImGuiCol_ModalWindowDimBg] = rgb(0x000000, 0.5f);
}

EditorFontResult loadEditorFont(ImGuiIO& io, const std::filesystem::path& fontPath, float pixelSize) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(fontPath, error)) {
        return {.error = "Editor font not found: " + fontPath.string()};
    }
    ImFontConfig config;
    config.OversampleH = 2;
    config.OversampleV = 2;
    // ImGui opens files from UTF-8 paths on every platform.
    const std::u8string utf8Path = fontPath.u8string();
    const std::string path(utf8Path.begin(), utf8Path.end());
    ImFont* font = io.Fonts->AddFontFromFileTTF(path.c_str(), pixelSize, &config);
    if (font == nullptr) {
        return {.error = "Could not load editor font: " + fontPath.string()};
    }
    io.FontDefault = font;
    return {.success = true};
}
