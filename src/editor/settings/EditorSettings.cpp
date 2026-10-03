#include "EditorSettings.hpp"

#include <charconv>
#include <fstream>
#include <initializer_list>
#include <string_view>
#include <utility>

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

    EditorPanelVisibility loaded = m_panels;
    EditorToolSettings loadedTools = m_tools;
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

        std::string* text = nullptr;
        if (key == "build.command") {
            text = &loadedTools.buildCommand;
        } else if (key == "build.directory") {
            text = &loadedTools.buildDirectory;
        } else if (key == "ide.command") {
            text = &loadedTools.ideCommand;
        }
        if (text != nullptr) {
            text->assign(value);
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
        } else if (key == "panel.graphics") {
            destination = &loaded.graphics;
        } else if (key == "panel.build") {
            destination = &loaded.build;
        } else if (key == "panel.properties") {
            destination = &loaded.properties;
        } else if (key == "panel.info") {
            destination = &loaded.info;
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
    m_tools = std::move(loadedTools);
    m_layoutVersion = loadedLayoutVersion;
    return {.success = true};
}

EditorSettingsResult EditorSettings::save() const {
    for (const std::string* value : {&m_tools.buildCommand, &m_tools.buildDirectory, &m_tools.ideCommand}) {
        if (value->find_first_of("\r\n") != std::string::npos) {
            return {.error = "Editor tool settings cannot contain line breaks"};
        }
    }
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
           << "ide.command=" << m_tools.ideCommand << '\n';
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

uint32_t EditorSettings::layoutVersion() const {
    return m_layoutVersion;
}

void EditorSettings::setLayoutVersion(uint32_t version) {
    m_layoutVersion = version;
}

const std::filesystem::path& EditorSettings::path() const {
    return m_path;
}
