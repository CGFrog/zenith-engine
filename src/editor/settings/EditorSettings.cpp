#include "EditorSettings.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <utility>

namespace {
// Every value a settings file sets, read into a copy so a failed load leaves
// the current settings alone.
struct LoadedSettings {
    EditorPanelVisibility panels;
    EditorToolSettings tools;
    EditorAppearanceSettings appearance;
    EditorAutosaveSettings autosave;
    uint32_t layoutVersion{0};
};

bool parseBoolean(std::string_view value, bool& destination) {
    if (value == "true") {
        destination = true;
        return true;
    }
    if (value == "false") {
        destination = false;
        return true;
    }
    return false;
}

std::optional<uint32_t> parseWholeNumber(std::string_view value) {
    uint32_t parsed = 0;
    const char* end = value.data() + value.size();
    const auto [position, error] = std::from_chars(value.data(), end, parsed);
    if (value.empty() || error != std::errc{} || position != end) {
        return std::nullopt;
    }
    return parsed;
}

std::optional<float> parseNumber(std::string_view value) {
    float parsed = 0.0f;
    const char* end = value.data() + value.size();
    const auto [position, error] = std::from_chars(value.data(), end, parsed);
    if (value.empty() || error != std::errc{} || position != end || !std::isfinite(parsed)) {
        return std::nullopt;
    }
    return parsed;
}

std::string formatNumber(float value) {
    std::array<char, 32> text{};
    const auto [end, error] = std::to_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} ? std::string(text.data(), end) : std::string("0");
}

std::string lineError(std::string_view key, size_t lineNumber, std::string_view expected) {
    return "Invalid " + std::string(key) + " on line " + std::to_string(lineNumber) + ": expected " +
           std::string(expected);
}

// Text-valued tool settings, or null for another key.
std::string* toolText(std::string_view key, EditorToolSettings& tools) {
    if (key == "build.command") {
        return &tools.buildCommand;
    }
    if (key == "build.directory") {
        return &tools.buildDirectory;
    }
    if (key == "ide.command") {
        return &tools.ideCommand;
    }
    if (key == "package.directory") {
        return &tools.packageDirectory;
    }
    return nullptr;
}

bool* legacyPanelFlag(std::string_view key, EditorPanelVisibility& panels) {
    if (key == "panel.viewport") {
        return &panels.viewport;
    }
    if (key == "panel.hierarchy") {
        return &panels.hierarchy;
    }
    if (key == "panel.inspector") {
        return &panels.inspector;
    }
    if (key == "panel.diagnostics") {
        return &panels.diagnostics;
    }
    if (key == "panel.render_statistics") {
        return &panels.renderStatistics;
    }
    if (key == "panel.asset_browser") {
        return &panels.assetBrowser;
    }
    if (key == "panel.graphics") {
        return &panels.graphics;
    }
    if (key == "panel.build") {
        return &panels.build;
    }
    if (key == "panel.properties") {
        return &panels.properties;
    }
    if (key == "panel.info") {
        return &panels.info;
    }
    return nullptr;
}

// Reads one appearance or autosave key. Returns an error message for an
// invalid value, an empty string when the key was read, and nullopt when the
// key is not one of these.
std::optional<std::string> readPreference(std::string_view key, std::string_view value, size_t lineNumber,
                                          LoadedSettings& loaded) {
    ThemeParameters& theme = loaded.appearance.theme;
    if (key == "theme.wallpaper") {
        return parseBoolean(value, theme.wallpaper) ? "" : lineError(key, lineNumber, "true or false");
    }
    if (key == "theme.frost") {
        const std::optional<uint32_t> radius = parseWholeNumber(value);
        if (!radius || *radius > ThemeParameters::maximumFrostRadius) {
            return lineError(key, lineNumber,
                             "a whole number of cells from 0 to " +
                                 std::to_string(ThemeParameters::maximumFrostRadius));
        }
        theme.frostRadius = *radius;
        return "";
    }
    if (key == "theme.glass_opacity") {
        const std::optional<float> opacity = parseNumber(value);
        if (!opacity || *opacity < ThemeParameters::minimumGlassOpacity ||
            *opacity > ThemeParameters::maximumGlassOpacity) {
            return lineError(key, lineNumber,
                             "a number from " + formatNumber(ThemeParameters::minimumGlassOpacity) + " to " +
                                 formatNumber(ThemeParameters::maximumGlassOpacity));
        }
        theme.glassOpacity = *opacity;
        return "";
    }
    if (key == "theme.panel_opacity") {
        const std::optional<float> opacity = parseNumber(value);
        if (!opacity || *opacity < ThemeParameters::minimumPaneOpacity || *opacity > 1.0f) {
            return lineError(key, lineNumber,
                             "a number from " + formatNumber(ThemeParameters::minimumPaneOpacity) + " to 1");
        }
        theme.paneOpacity = *opacity;
        return "";
    }
    if (key == "theme.accent") {
        const std::optional<ThemeAccent> accent = parseThemeAccent(value);
        if (!accent) {
            std::string names;
            for (const ThemeAccentInfo& info : themeAccents()) {
                names += (names.empty() ? "" : ", ") + std::string(info.key);
            }
            return lineError(key, lineNumber, "one of " + names);
        }
        theme.accent = *accent;
        return "";
    }
    if (key == "ui.scale") {
        if (value == "auto") {
            loaded.appearance.uiScale = 0;
            return "";
        }
        const std::optional<uint32_t> scale = parseWholeNumber(value);
        if (!scale || *scale < 1 || *scale > maximumUiScale) {
            return lineError(key, lineNumber, "auto or a whole number from 1 to " + std::to_string(maximumUiScale));
        }
        loaded.appearance.uiScale = *scale;
        return "";
    }
    if (key == "build.on_save") {
        return parseBoolean(value, loaded.tools.buildOnSave) ? "" : lineError(key, lineNumber, "true or false");
    }
    if (key == "ui.pie_menus") {
        return parseBoolean(value, loaded.appearance.pieMenus) ? "" : lineError(key, lineNumber, "true or false");
    }
    if (key == "autosave.interval") {
        const std::optional<float> seconds = parseNumber(value);
        if (!seconds || *seconds < 0.0f || *seconds > EditorAutosaveSettings::maximumIntervalSeconds) {
            return lineError(key, lineNumber,
                             "seconds from 0 (off) to " + formatNumber(EditorAutosaveSettings::maximumIntervalSeconds));
        }
        loaded.autosave.intervalSeconds = *seconds;
        return "";
    }
    return std::nullopt;
}
} // namespace

std::filesystem::path resolveEditorSettingsDirectory(std::string_view utf8Override,
                                                     std::filesystem::path userDirectory) {
    if (utf8Override.empty()) {
        return userDirectory;
    }
    const std::u8string text(utf8Override.begin(), utf8Override.end());
    return std::filesystem::path(text);
}

EditorSettings::EditorSettings(std::filesystem::path settingsPath)
    : m_path(std::move(settingsPath)) {}

EditorSettingsResult EditorSettings::load() {
    std::ifstream input(m_path);
    if (!input) {
        if (!std::filesystem::exists(m_path)) {
            return {.success = true};
        }
        return {.error = "Could not open editor settings: " + m_path.string()};
    }

    LoadedSettings loaded{.panels = m_panels,
                          .tools = m_tools,
                          .appearance = m_appearance,
                          .autosave = m_autosave,
                          .layoutVersion = m_layoutVersion};
    std::string line;
    size_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        if (line.empty() || line.front() == '#') {
            continue;
        }

        const size_t separator = line.find('=');
        if (separator == std::string::npos) {
            return {.error = "Invalid editor setting on line " + std::to_string(lineNumber)};
        }

        const std::string_view key(line.data(), separator);
        const std::string_view value(line.data() + separator + 1, line.size() - separator - 1);
        if (key == "layout.version") {
            const std::optional<uint32_t> version = parseWholeNumber(value);
            if (!version) {
                return {.error = "Invalid layout version on line " + std::to_string(lineNumber)};
            }
            loaded.layoutVersion = *version;
            continue;
        }
        if (std::string* text = toolText(key, loaded.tools)) {
            text->assign(value);
            continue;
        }
        if (const std::optional<std::string> problem = readPreference(key, value, lineNumber, loaded)) {
            if (!problem->empty()) {
                return {.error = *problem};
            }
            continue;
        }
        bool* flag = legacyPanelFlag(key, loaded.panels);
        if (flag == nullptr) {
            continue;
        }
        if (!parseBoolean(value, *flag)) {
            return {.error = "Invalid boolean editor setting on line " + std::to_string(lineNumber)};
        }
    }

    if (input.bad()) {
        return {.error = "Could not read editor settings: " + m_path.string()};
    }
    m_panels = loaded.panels;
    m_tools = std::move(loaded.tools);
    m_appearance = loaded.appearance;
    m_autosave = loaded.autosave;
    m_layoutVersion = loaded.layoutVersion;
    return {.success = true};
}

EditorSettingsResult EditorSettings::save() const {
    for (const std::string* value :
         {&m_tools.buildCommand, &m_tools.buildDirectory, &m_tools.ideCommand, &m_tools.packageDirectory}) {
        if (value->find_first_of("\r\n") != std::string::npos) {
            return {.error = "Editor tool settings cannot contain line breaks"};
        }
    }
    // Out-of-range values would make the next load fail, so they are stored
    // clamped.
    const ThemeParameters theme = clampedThemeParameters(m_appearance.theme);
    const uint32_t uiScale = std::min(m_appearance.uiScale, maximumUiScale);
    const float autosaveInterval =
        std::isfinite(m_autosave.intervalSeconds)
            ? std::clamp(m_autosave.intervalSeconds, 0.0f, EditorAutosaveSettings::maximumIntervalSeconds)
            : EditorAutosaveSettings{}.intervalSeconds;

    std::error_code error;
    if (const std::filesystem::path parent = m_path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) {
            return {.error = "Could not create editor settings directory: " + error.message()};
        }
    }

    std::ofstream output(m_path, std::ios::trunc);
    if (!output) {
        return {.error = "Could not write editor settings: " + m_path.string()};
    }
    // Panel visibility and layout live in workspaces.json; the legacy keys
    // are only read, for migration.
    output << "# Zenith editor settings\n"
           << "build.command=" << m_tools.buildCommand << '\n'
           << "build.directory=" << m_tools.buildDirectory << '\n'
           << "ide.command=" << m_tools.ideCommand << '\n'
           << "package.directory=" << m_tools.packageDirectory << '\n'
           << "build.on_save=" << (m_tools.buildOnSave ? "true" : "false") << '\n'
           << "theme.wallpaper=" << (theme.wallpaper ? "true" : "false") << '\n'
           << "theme.frost=" << theme.frostRadius << '\n'
           << "theme.glass_opacity=" << formatNumber(theme.glassOpacity) << '\n'
           << "theme.panel_opacity=" << formatNumber(theme.paneOpacity) << '\n'
           << "theme.accent=" << themeAccentInfo(theme.accent).key << '\n'
           << "ui.scale=" << (uiScale == 0 ? std::string("auto") : std::to_string(uiScale)) << '\n'
           << "ui.pie_menus=" << (m_appearance.pieMenus ? "true" : "false") << '\n'
           << "autosave.interval=" << formatNumber(autosaveInterval) << '\n';
    if (!output) {
        return {.error = "Could not finish writing editor settings: " + m_path.string()};
    }
    return {.success = true};
}

EditorPanelVisibility& EditorSettings::panels() {
    return m_panels;
}

const EditorPanelVisibility& EditorSettings::panels() const {
    return m_panels;
}

EditorToolSettings& EditorSettings::tools() {
    return m_tools;
}

const EditorToolSettings& EditorSettings::tools() const {
    return m_tools;
}

EditorAppearanceSettings& EditorSettings::appearance() {
    return m_appearance;
}

const EditorAppearanceSettings& EditorSettings::appearance() const {
    return m_appearance;
}

EditorAutosaveSettings& EditorSettings::autosave() {
    return m_autosave;
}

const EditorAutosaveSettings& EditorSettings::autosave() const {
    return m_autosave;
}

uint32_t EditorSettings::layoutVersion() const {
    return m_layoutVersion;
}

void EditorSettings::setLayoutVersion(uint32_t version) {
    m_layoutVersion = version;
}

const std::filesystem::path& EditorSettings::path() const {
    return m_path;
}
