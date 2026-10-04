#pragma once

#include "editor/settings/EditorSettings.hpp"
#include "editor/theme/EditorTheme.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// What applying an appearance reported: a font that could not load (the
// editor keeps the fonts it had) and optional fonts that were missing.
struct EditorAppearanceResult {
    std::string error;
    std::vector<std::string> warnings;

    explicit operator bool() const {
        return error.empty();
    }
};

// The editor's look on the application's ImGui context: the preferences'
// UI scale (metrics and pixel fonts) and theme tuning (colours and accent)
// applied to the global ImGui style. The editor application owns one for its
// lifetime and both the Project Hub and every session apply through it, so
// it is the only owner of the font cache and the hub shows whatever look the
// last session chose.
//
// Fonts are added to the atlas when a UI scale first needs them, so apply
// must run between frames (before the first frame, or in an update).
class EditorAppearance {
  public:
    // fontPath is the fallback font file; the pixel fonts are read from its
    // folder. displayContentScale sizes the automatic UI scale (0 or less
    // counts as 1).
    EditorAppearance(std::filesystem::path fontPath, float displayContentScale);

    // Makes settings the current look. Applying the current look again does
    // nothing; a new UI scale rebuilds the metrics (and with them the
    // colours) and switches to that scale's fonts, loading them once.
    EditorAppearanceResult apply(const EditorAppearanceSettings& settings);

    // The look last applied; the defaults before the first apply.
    const EditorAppearanceSettings& current() const;
    // The theme tuning of the current look, for painting chrome.
    const ThemeParameters& theme() const;
    float displayContentScale() const;

  private:
    EditorAppearanceResult useFontsFor(float artPixel);

    std::filesystem::path m_fontPath;
    float m_displayContentScale{1.0f};
    std::vector<EditorFonts> m_fonts;
    std::optional<EditorAppearanceSettings> m_applied;
    float m_appliedArtPixel{0.0f};
};
