#include "EditorAppearance.hpp"

#include <imgui.h>

#include <algorithm>
#include <utility>

EditorAppearance::EditorAppearance(std::filesystem::path fontPath, float displayContentScale)
    : m_fontPath(std::move(fontPath))
    , m_displayContentScale(displayContentScale > 0.0f ? displayContentScale : 1.0f) {}

EditorAppearanceResult EditorAppearance::apply(const EditorAppearanceSettings& settings) {
    if (m_applied == settings) {
        return {};
    }
    EditorAppearanceResult result;
    ImGuiStyle& style = ImGui::GetStyle();
    const float artPixel = artPixelForUiScale(settings.uiScale, m_displayContentScale);
    if (artPixel != m_appliedArtPixel) {
        // Metrics reset the whole style, so the colours follow.
        applyEditorMetrics(style, artPixel);
        result = useFontsFor(artPixel);
        m_appliedArtPixel = artPixel;
    }
    applyEditorColors(style, settings.theme);
    m_applied = settings;
    return result;
}

const EditorAppearanceSettings& EditorAppearance::current() const {
    static const EditorAppearanceSettings defaults;
    return m_applied ? *m_applied : defaults;
}

const ThemeParameters& EditorAppearance::theme() const {
    return current().theme;
}

float EditorAppearance::displayContentScale() const {
    return m_displayContentScale;
}

EditorAppearanceResult EditorAppearance::useFontsFor(float artPixel) {
    ImGuiIO& io = ImGui::GetIO();
    const auto loaded =
        std::ranges::find_if(m_fonts, [artPixel](const EditorFonts& fonts) { return fonts.artPixel == artPixel; });
    if (loaded != m_fonts.end()) {
        useEditorFonts(io, *loaded);
        return {};
    }
    // Pixel fonts are rasterised for one art pixel; the renderer uploads the
    // grown atlas before the next frame.
    const EditorFontResult fonts = loadEditorFonts(io, m_fontPath.parent_path(), m_fontPath, artPixel);
    EditorAppearanceResult result{.error = fonts.error, .warnings = fonts.warnings};
    if (fonts) {
        m_fonts.push_back(fonts.fonts);
    }
    return result;
}
