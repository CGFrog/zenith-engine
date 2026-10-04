#include "editor/theme/EditorTheme.hpp"

#include <ImGuizmo.h>

#include <algorithm>
#include <cmath>
#include <system_error>

namespace {
// The pixel font's size in art pixels; VT323 reads best at 18.
constexpr float pixelFontSize = 18.0f;
// The fallback (Inter) when the pixel font is missing.
constexpr float fallbackFontSize = 15.0f;
constexpr const char* pixelFontFile = "VT323-Regular.ttf";

float g_artPixel = 1.0f;

// Latin-1 plus the em dash the inspector shows for values that differ
// between selected entities (mixedValueText).
constexpr ImWchar editorGlyphRanges[]{0x0020, 0x00FF, 0x2014, 0x2014, 0};

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
    config.GlyphRanges = editorGlyphRanges;
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
        merge.GlyphRanges = editorGlyphRanges;
        io.Fonts->AddFontFromFileTTF(utf8Path(fallbackFont).c_str(), pixelSize * 0.85f, &merge);
    }
    return added;
}
} // namespace

float artPixelForScale(float contentScale) {
    return std::max(1.0f, std::round(contentScale));
}

float artPixelForUiScale(uint32_t uiScaleOverride, float contentScale) {
    if (uiScaleOverride == 0) {
        return artPixelForScale(contentScale);
    }
    return static_cast<float>(std::min(uiScaleOverride, maximumUiScale));
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
    // A thin dark gap between docked areas, as in Blender.
    style.DockingSeparatorSize = 2.0f;

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

void applyEditorColors(ImGuiStyle& style, const ThemeParameters& requested) {
    using namespace palette;
    const ThemeParameters parameters = clampedThemeParameters(requested);
    const ThemeColor accent = themeAccentInfo(parameters.accent).color;
    ImVec4* colors = style.Colors;
    const ImVec4 clear = toImVec4(transparent);
    colors[ImGuiCol_Text] = toImVec4(ink);
    colors[ImGuiCol_TextDisabled] = toImVec4(inkDisabled);
    colors[ImGuiCol_WindowBg] = toImVec4(pane);
    colors[ImGuiCol_ChildBg] = clear;
    colors[ImGuiCol_PopupBg] = toImVec4(popup);
    colors[ImGuiCol_Border] = toImVec4(outline);
    colors[ImGuiCol_BorderShadow] = clear;
    colors[ImGuiCol_FrameBg] = toImVec4(field);
    colors[ImGuiCol_FrameBgHovered] = toImVec4(fieldHovered);
    colors[ImGuiCol_FrameBgActive] = toImVec4(fieldActive);
    colors[ImGuiCol_TitleBg] = toImVec4(header);
    colors[ImGuiCol_TitleBgActive] = toImVec4(section);
    colors[ImGuiCol_TitleBgCollapsed] = toImVec4(header);
    // Menu bars sit on painted header strips.
    colors[ImGuiCol_MenuBarBg] = clear;
    colors[ImGuiCol_ScrollbarBg] = toImVec4(field);
    colors[ImGuiCol_ScrollbarGrab] = toImVec4(scrollbarThumb);
    colors[ImGuiCol_ScrollbarGrabHovered] = toImVec4(scrollbarThumbHovered);
    colors[ImGuiCol_ScrollbarGrabActive] = toImVec4(accent);
    colors[ImGuiCol_CheckMark] = toImVec4(sliderHandle);
    colors[ImGuiCol_SliderGrab] = toImVec4(sliderHandle);
    colors[ImGuiCol_SliderGrabActive] = toImVec4(accent);
    colors[ImGuiCol_Button] = toImVec4(button);
    colors[ImGuiCol_ButtonHovered] = toImVec4(buttonHovered);
    colors[ImGuiCol_ButtonActive] = toImVec4(buttonActive);
    colors[ImGuiCol_Header] = toImVec4(rowSelected);
    colors[ImGuiCol_HeaderHovered] = toImVec4(rowHovered);
    colors[ImGuiCol_HeaderActive] = toImVec4(rowSelectedHighlight);
    colors[ImGuiCol_Separator] = toImVec4(separator);
    colors[ImGuiCol_SeparatorHovered] = toImVec4(accent.withAlpha(0.6f));
    colors[ImGuiCol_SeparatorActive] = toImVec4(accent);
    colors[ImGuiCol_ResizeGrip] = toImVec4(button.withAlpha(0.3f));
    colors[ImGuiCol_ResizeGripHovered] = toImVec4(accent.withAlpha(0.6f));
    colors[ImGuiCol_ResizeGripActive] = toImVec4(accent);
    // Tabs sit in the header strip: the selected one is lifted to the section
    // grey with the accent along its top, as Blender's active area tab.
    colors[ImGuiCol_Tab] = toImVec4(header);
    colors[ImGuiCol_TabHovered] = toImVec4(buttonHovered);
    colors[ImGuiCol_TabSelected] = toImVec4(section);
    colors[ImGuiCol_TabSelectedOverline] = toImVec4(accent);
    colors[ImGuiCol_TabDimmed] = toImVec4(header);
    colors[ImGuiCol_TabDimmedSelected] = toImVec4(section);
    colors[ImGuiCol_TabDimmedSelectedOverline] = clear;
    colors[ImGuiCol_DockingPreview] = toImVec4(accent.withAlpha(0.5f));
    // Empty dock space shows the painted gap.
    colors[ImGuiCol_DockingEmptyBg] = clear;
    colors[ImGuiCol_PlotLines] = toImVec4(inkMuted);
    colors[ImGuiCol_PlotLinesHovered] = toImVec4(accent);
    colors[ImGuiCol_PlotHistogram] = toImVec4(sliderFill);
    colors[ImGuiCol_PlotHistogramHovered] = toImVec4(accent);
    colors[ImGuiCol_TableHeaderBg] = toImVec4(section);
    colors[ImGuiCol_TableBorderStrong] = toImVec4(outline);
    colors[ImGuiCol_TableBorderLight] = toImVec4(separator);
    colors[ImGuiCol_TableRowBg] = clear;
    colors[ImGuiCol_TableRowBgAlt] = toImVec4(paneAlternate);
    colors[ImGuiCol_TextLink] = toImVec4(info);
    colors[ImGuiCol_TextSelectedBg] = toImVec4(textSelection);
    colors[ImGuiCol_DragDropTarget] = toImVec4(accent);
    colors[ImGuiCol_NavCursor] = toImVec4(accent);
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
    gizmo.Colors[ImGuizmo::SELECTION] = toImVec4(accent);
}

ThemeColor editorAccent() {
    const ImVec4 accent = ImGui::GetStyle().Colors[ImGuiCol_NavCursor];
    return {accent.x, accent.y, accent.z, accent.w};
}

ImVec4 toImVec4(ThemeColor color) {
    return {color.r, color.g, color.b, color.a};
}

ImU32 toImU32(ThemeColor color) {
    return ImGui::ColorConvertFloat4ToU32(toImVec4(color));
}

EditorFontResult loadEditorFonts(ImGuiIO& io, const std::filesystem::path& fontDirectory,
                                 const std::filesystem::path& fallbackFont, float artPixel) {
    EditorFontResult result;
    std::error_code error;
    const std::filesystem::path pixelPath = fontDirectory / pixelFontFile;
    if (std::filesystem::is_regular_file(pixelPath, error)) {
        if (ImFont* font = addPixelFont(io, pixelPath, pixelFontSize * artPixel, fallbackFont)) {
            result.fonts = {.artPixel = artPixel, .font = font};
            useEditorFonts(io, result.fonts);
            result.success = true;
            return result;
        }
        result.warnings.push_back("Could not load the editor pixel font from " + fontDirectory.string());
    } else {
        result.warnings.push_back("Editor pixel font not found in " + fontDirectory.string() +
                                  "; using the fallback font");
    }
    if (!std::filesystem::is_regular_file(fallbackFont, error)) {
        result.error = "Editor font not found: " + fallbackFont.string();
        return result;
    }
    ImFontConfig config;
    config.OversampleH = 2;
    config.OversampleV = 2;
    config.GlyphRanges = editorGlyphRanges;
    ImFont* font = io.Fonts->AddFontFromFileTTF(utf8Path(fallbackFont).c_str(), fallbackFontSize * artPixel, &config);
    if (font == nullptr) {
        result.error = "Could not load editor font: " + fallbackFont.string();
        return result;
    }
    result.fonts = {.artPixel = artPixel, .font = font};
    useEditorFonts(io, result.fonts);
    result.success = true;
    return result;
}

void useEditorFonts(ImGuiIO& io, const EditorFonts& fonts) {
    io.FontDefault = fonts.font;
}
