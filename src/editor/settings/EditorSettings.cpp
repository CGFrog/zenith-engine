#include "EditorSettings.hpp"

#include <charconv>
#include <fstream>
#include <string_view>

namespace {
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
} // namespace

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

    EditorPanelVisibility loaded = m_panels;
    uint32_t loadedLayoutVersion = m_layoutVersion;
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
            const char* begin = value.data();
            const char* end = begin + value.size();
            const auto [position, error] = std::from_chars(begin, end, loadedLayoutVersion);
            if (error != std::errc{} || position != end) {
                return {.error = "Invalid layout version on line " + std::to_string(lineNumber)};
            }
            continue;
        }

        bool* destination = nullptr;
        if (key == "panel.viewport") {
            destination = &loaded.viewport;
        } else if (key == "panel.hierarchy") {
            destination = &loaded.hierarchy;
        } else if (key == "panel.inspector") {
            destination = &loaded.inspector;
        } else if (key == "panel.diagnostics") {
            destination = &loaded.diagnostics;
        } else if (key == "panel.render_statistics") {
            destination = &loaded.renderStatistics;
        } else if (key == "panel.asset_browser") {
            destination = &loaded.assetBrowser;
        } else {
            continue;
        }

        if (!parseBoolean(value, *destination)) {
            return {.error = "Invalid boolean editor setting on line " + std::to_string(lineNumber)};
        }
    }

    if (input.bad()) {
        return {.error = "Could not read editor settings: " + m_path.string()};
    }
    m_panels = loaded;
    m_layoutVersion = loadedLayoutVersion;
    return {.success = true};
}

EditorSettingsResult EditorSettings::save() const {
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
    output << "# Zenith editor settings\n"
           << "layout.version=" << m_layoutVersion << '\n'
           << "panel.viewport=" << (m_panels.viewport ? "true" : "false") << '\n'
           << "panel.hierarchy=" << (m_panels.hierarchy ? "true" : "false") << '\n'
           << "panel.inspector=" << (m_panels.inspector ? "true" : "false") << '\n'
           << "panel.diagnostics=" << (m_panels.diagnostics ? "true" : "false") << '\n'
           << "panel.render_statistics=" << (m_panels.renderStatistics ? "true" : "false") << '\n'
           << "panel.asset_browser=" << (m_panels.assetBrowser ? "true" : "false") << '\n';
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

uint32_t EditorSettings::layoutVersion() const {
    return m_layoutVersion;
}

void EditorSettings::setLayoutVersion(uint32_t version) {
    m_layoutVersion = version;
}

const std::filesystem::path& EditorSettings::path() const {
    return m_path;
}
