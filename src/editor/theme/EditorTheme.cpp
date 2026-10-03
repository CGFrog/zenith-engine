#include "editor/theme/EditorTheme.hpp"

#include <ImGuizmo.h>

#include <algorithm>
#include <cmath>
#include <system_error>

namespace {
// Pixel font sizes in art pixels. Press Start 2P is drawn on an 8-pixel grid;
// VT323 reads best at 18.
constexpr float bodyFontSize = 18.0f;
constexpr float titleFontSize = 8.0f;
// The fallback (Inter) when the pixel fonts are missing.
constexpr float fallbackFontSize = 15.0f;
constexpr const char* bodyFontFile = "VT323-Regular.ttf";
constexpr const char* titleFontFile = "PressStart2P-Regular.ttf";

float g_artPixel = 1.0f;
ImFont* g_titleFont = nullptr;

std::string utf8Path(const std::filesystem::path& path) {
    // ImGui opens files from UTF-8 paths on every platform.
    const std::u8string utf8 = path.u8string();
    return {utf8.begin(), utf8.end()};
}

ImFont* addPixelFont(ImGuiIO& io, const std::filesystem::path& font, float pixelSize,
                     const std::filesystem::path& fallbackFont) {
    ImFontConfig config;
    // Pixel fonts are drawn on whole pixels; oversampling would blur them.
    config.OversampleH = 1;
    config.OversampleV = 1;
    config.PixelSnapH = true;
    ImFont* added = io.Fonts->AddFontFromFileTTF(utf8Path(font).c_str(), pixelSize, &config);
    if (added == nullptr) {
        return nullptr;
    }
    std::error_code error;
    if (std::filesystem::is_regular_file(fallbackFont, error)) {
        // Merged glyphs only fill code points the pixel font lacks.
        ImFontConfig merge;
        merge.MergeMode = true;
        merge.OversampleH = 2;
        merge.OversampleV = 2;
        io.Fonts->AddFontFromFileTTF(utf8Path(fallbackFont).c_str(), pixelSize * 0.85f, &merge);
    }
    return added;
}
} // namespace

float artPixelForScale(float contentScale) {
    return std::max(1.0f, std::round(contentScale));
}

float editorArtPixel() {
    return g_artPixel;
}

void applyEditorMetrics(ImGuiStyle& style, float artPixel) {
    style = ImGuiStyle();

    // Sizes in art pixels; ScaleAllSizes below puts them on the screen grid.
    style.WindowPadding = {4.0f, 4.0f};
    style.FramePadding = {4.0f, 2.0f};
    style.CellPadding = {3.0f, 1.0f};
    style.ItemSpacing = {4.0f, 3.0f};
    style.ItemInnerSpacing = {3.0f, 3.0f};
    style.IndentSpacing = 12.0f;
    style.ScrollbarSize = 8.0f;
    style.GrabMinSize = 8.0f;
    style.TabBarOverlineSize = 2.0f;
    // Wide gaps between docked areas let the wallpaper show through.
    style.DockingSeparatorSize = 9.0f;

    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = 0.0f;
    style.PopupRounding = 0.0f;
    style.ScrollbarRounding = 0.0f;
    style.GrabRounding = 0.0f;
    style.TabRounding = 0.0f;

    style.WindowTitleAlign = {0.0f, 0.5f};
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.ColorButtonPosition = ImGuiDir_Left;

    // Hard edges: pixel art has no anti-aliased slopes.
    style.AntiAliasedLines = false;
    style.AntiAliasedLinesUseTex = false;
    style.AntiAliasedFill = false;

    style.ScaleAllSizes(artPixel);
    // Border sizes are line thicknesses, which ScaleAllSizes leaves alone.
    style.WindowBorderSize = artPixel;
    style.ChildBorderSize = artPixel;
    style.PopupBorderSize = artPixel;
    style.FrameBorderSize = artPixel;
    style.TabBorderSize = 0.0f;
    style.TabBarBorderSize = artPixel;
    style.SeparatorTextBorderSize = artPixel;
    g_artPixel = artPixel;
}

void applyEditorColors(ImGuiStyle& style) {
    using namespace palette;
    ImVec4* colors = style.Colors;
    const ImVec4 clear = toImVec4(transparent);
    colors[ImGuiCol_Text] = toImVec4(ink);
    colors[ImGuiCol_TextDisabled] = toImVec4(inkDisabled);
    colors[ImGuiCol_WindowBg] = toImVec4(pane);
    colors[ImGuiCol_ChildBg] = clear;
    colors[ImGuiCol_PopupBg] = toImVec4(popup);
    colors[ImGuiCol_Border] = toImVec4(glassBorder);
    colors[ImGuiCol_BorderShadow] = clear;
    colors[ImGuiCol_FrameBg] = toImVec4(field);
    colors[ImGuiCol_FrameBgHovered] = toImVec4(fieldHovered);
    colors[ImGuiCol_FrameBgActive] = toImVec4(fieldActive);
    colors[ImGuiCol_TitleBg] = toImVec4(glassFallback);
    colors[ImGuiCol_TitleBgActive] = toImVec4(buttonBottom);
    colors[ImGuiCol_TitleBgCollapsed] = toImVec4(glassFallback);
    // Menu bars sit on painted glass.
    colors[ImGuiCol_MenuBarBg] = clear;
    colors[ImGuiCol_ScrollbarBg] = toImVec4(field);
    colors[ImGuiCol_ScrollbarGrab] = toImVec4(scrollbarThumb);
    colors[ImGuiCol_ScrollbarGrabHovered] = toImVec4(scrollbarThumbHovered);
    colors[ImGuiCol_ScrollbarGrabActive] = toImVec4(scrollbarThumbActive);
    colors[ImGuiCol_CheckMark] = toImVec4(sliderHandle);
    colors[ImGuiCol_SliderGrab] = toImVec4(sliderHandle);
    colors[ImGuiCol_SliderGrabActive] = toImVec4(pixelGlow);
    colors[ImGuiCol_Button] = toImVec4(buttonTop);
    colors[ImGuiCol_ButtonHovered] = toImVec4(buttonHoverTop);
    colors[ImGuiCol_ButtonActive] = toImVec4(buttonActiveBottom);
    colors[ImGuiCol_Header] = toImVec4(rowSelected);
    colors[ImGuiCol_HeaderHovered] = toImVec4(rowHovered);
    colors[ImGuiCol_HeaderActive] = toImVec4(rowSelectedHighlight);
    colors[ImGuiCol_Separator] = toImVec4(separator);
    colors[ImGuiCol_SeparatorHovered] = toImVec4(pixelGlow.withAlpha(0.6f));
    colors[ImGuiCol_SeparatorActive] = toImVec4(pixelGlow);
    colors[ImGuiCol_ResizeGrip] = toImVec4(buttonTop.withAlpha(0.3f));
    colors[ImGuiCol_ResizeGripHovered] = toImVec4(pixelGlow.withAlpha(0.6f));
    colors[ImGuiCol_ResizeGripActive] = toImVec4(pixelGlow);
    colors[ImGuiCol_Tab] = toImVec4(buttonBottom.withAlpha(0.85f));
    colors[ImGuiCol_TabHovered] = toImVec4(buttonHoverTop);
    colors[ImGuiCol_TabSelected] = toImVec4(rowSelected);
    colors[ImGuiCol_TabSelectedOverline] = toImVec4(pixelGlow);
    colors[ImGuiCol_TabDimmed] = toImVec4(glassFallback.withAlpha(0.85f));
    colors[ImGuiCol_TabDimmedSelected] = toImVec4(sectionTop);
    colors[ImGuiCol_TabDimmedSelectedOverline] = clear;
    colors[ImGuiCol_DockingPreview] = toImVec4(pixelGlow.withAlpha(0.5f));
    // Empty dock space shows the wallpaper.
    colors[ImGuiCol_DockingEmptyBg] = clear;
    colors[ImGuiCol_PlotLines] = toImVec4(inkMuted);
    colors[ImGuiCol_PlotLinesHovered] = toImVec4(pixelGlow);
    colors[ImGuiCol_PlotHistogram] = toImVec4(sliderFill);
    colors[ImGuiCol_PlotHistogramHovered] = toImVec4(pixelGlow);
    colors[ImGuiCol_TableHeaderBg] = toImVec4(sectionTop);
    colors[ImGuiCol_TableBorderStrong] = toImVec4(glassBorder);
    colors[ImGuiCol_TableBorderLight] = toImVec4(separator);
    colors[ImGuiCol_TableRowBg] = clear;
    colors[ImGuiCol_TableRowBgAlt] = toImVec4(paneAlternate);
    colors[ImGuiCol_TextLink] = toImVec4(info);
    colors[ImGuiCol_TextSelectedBg] = toImVec4(textSelection);
    colors[ImGuiCol_DragDropTarget] = toImVec4(pixelGlow);
    colors[ImGuiCol_NavCursor] = toImVec4(pixelGlow);
    colors[ImGuiCol_NavWindowingHighlight] = toImVec4(captionInk.withAlpha(0.7f));
    colors[ImGuiCol_NavWindowingDimBg] = toImVec4(modalDim.withAlpha(0.3f));
    colors[ImGuiCol_ModalWindowDimBg] = toImVec4(modalDim);

    // The gizmo matches the modal-transform guides and vector-field stripes.
    ImGuizmo::Style& gizmo = ImGuizmo::GetStyle();
    gizmo.Colors[ImGuizmo::DIRECTION_X] = toImVec4(axisX);
    gizmo.Colors[ImGuizmo::DIRECTION_Y] = toImVec4(axisY);
    gizmo.Colors[ImGuizmo::DIRECTION_Z] = toImVec4(axisZ);
    gizmo.Colors[ImGuizmo::PLANE_X] = toImVec4(axisX.withAlpha(0.38f));
    gizmo.Colors[ImGuizmo::PLANE_Y] = toImVec4(axisY.withAlpha(0.38f));
    gizmo.Colors[ImGuizmo::PLANE_Z] = toImVec4(axisZ.withAlpha(0.38f));
    gizmo.Colors[ImGuizmo::SELECTION] = toImVec4(pixelGlow);
}

ImVec4 toImVec4(ThemeColor color) {
    return {color.r, color.g, color.b, color.a};
}

ImU32 toImU32(ThemeColor color) {
    return ImGui::ColorConvertFloat4ToU32(toImVec4(color));
}

EditorFontResult loadEditorFonts(ImGuiIO& io, const std::filesystem::path& fontDirectory,
                                 const std::filesystem::path& fallbackFont, float artPixel) {
    g_titleFont = nullptr;
    EditorFontResult result;
    std::error_code error;
    const std::filesystem::path bodyPath = fontDirectory / bodyFontFile;
    const std::filesystem::path titlePath = fontDirectory / titleFontFile;
    if (std::filesystem::is_regular_file(bodyPath, error) && std::filesystem::is_regular_file(titlePath, error)) {
        ImFont* body = addPixelFont(io, bodyPath, bodyFontSize * artPixel, fallbackFont);
        ImFont* title = addPixelFont(io, titlePath, titleFontSize * artPixel, fallbackFont);
        if (body != nullptr && title != nullptr) {
            io.FontDefault = body;
            g_titleFont = title;
            result.success = true;
            return result;
        }
        result.warnings.push_back("Could not load the editor pixel fonts from " + fontDirectory.string());
    } else {
        result.warnings.push_back("Editor pixel fonts not found in " + fontDirectory.string() +
                                  "; using the fallback font");
    }
    if (!std::filesystem::is_regular_file(fallbackFont, error)) {
        result.error = "Editor font not found: " + fallbackFont.string();
        return result;
    }
    ImFontConfig config;
    config.OversampleH = 2;
    config.OversampleV = 2;
    ImFont* font = io.Fonts->AddFontFromFileTTF(utf8Path(fallbackFont).c_str(), fallbackFontSize * artPixel, &config);
    if (font == nullptr) {
        result.error = "Could not load editor font: " + fallbackFont.string();
        return result;
    }
    io.FontDefault = font;
    result.success = true;
    return result;
}

ImFont* editorTitleFont() {
    return g_titleFont;
}
