#include "editor/areas/DefaultWorkspaces.hpp"

#include "editor/settings/EditorSettings.hpp"

#include <algorithm>
#include <array>

namespace {
using enum DefaultDockSlot;

constexpr std::array layoutAreas{
    DefaultArea{"viewport", Center},          DefaultArea{"hierarchy", Left},         DefaultArea{"inspector", Right},
    DefaultArea{"graphics", RightBottom},     DefaultArea{"diagnostics", Bottom},     DefaultArea{"build", Bottom},
    DefaultArea{"render_statistics", Bottom}, DefaultArea{"asset_browser", Bottom},
};

constexpr std::array lightingAreas{
    DefaultArea{"viewport", Center},       DefaultArea{"hierarchy", Left},           DefaultArea{"graphics", Right},
    DefaultArea{"inspector", RightBottom}, DefaultArea{"render_statistics", Bottom},
};

constexpr std::array debugAreas{
    DefaultArea{"viewport", Center},       DefaultArea{"hierarchy", Left},     DefaultArea{"render_statistics", Right},
    DefaultArea{"inspector", RightBottom}, DefaultArea{"diagnostics", Bottom}, DefaultArea{"build", Bottom},
};

constexpr std::array workspaces{
    DefaultWorkspace{"Layout", layoutAreas},
    DefaultWorkspace{"Lighting", lightingAreas},
    DefaultWorkspace{"Debug", debugAreas},
};

// The panels editors before workspaces.json had, with their window titles
// (the keys in layout.ini) and their settings.ini visibility flags.
struct LegacyPanel {
    std::string_view type;
    std::string_view title;
    bool EditorPanelVisibility::* visible;
};

constexpr std::array legacyPanels{
    LegacyPanel{"viewport", "Viewport", &EditorPanelVisibility::viewport},
    LegacyPanel{"hierarchy", "Hierarchy", &EditorPanelVisibility::hierarchy},
    LegacyPanel{"inspector", "Inspector", &EditorPanelVisibility::inspector},
    LegacyPanel{"diagnostics", "Diagnostics", &EditorPanelVisibility::diagnostics},
    LegacyPanel{"render_statistics", "Render Statistics", &EditorPanelVisibility::renderStatistics},
    LegacyPanel{"asset_browser", "Asset Browser", &EditorPanelVisibility::assetBrowser},
    LegacyPanel{"graphics", "Graphics", &EditorPanelVisibility::graphics},
    LegacyPanel{"build", "Build", &EditorPanelVisibility::build},
};

// Layouts older than version 3 lack the Graphics panel below the Inspector;
// version 4 docked the Build panel beside Diagnostics.
constexpr uint32_t lastLegacyLayoutVersion = 4;

// Renames the [Window][<title>] sections of legacy panels to the windows of
// their first areas. Other sections, including the docking tree, are kept.
std::string renameLegacyWindows(std::string_view layout) {
    std::string renamed;
    renamed.reserve(layout.size() + 256);
    size_t lineStart = 0;
    while (lineStart < layout.size()) {
        size_t lineEnd = layout.find('\n', lineStart);
        const size_t next = lineEnd == std::string_view::npos ? layout.size() : lineEnd + 1;
        std::string_view line = layout.substr(lineStart, next - lineStart);
        std::string_view content = line;
        while (!content.empty() && (content.back() == '\n' || content.back() == '\r')) {
            content.remove_suffix(1);
        }
        const auto legacy = std::ranges::find_if(legacyPanels, [content](const LegacyPanel& panel) {
            return content.size() == panel.title.size() + 10 && content.starts_with("[Window][") &&
                   content.substr(9, panel.title.size()) == panel.title && content.back() == ']';
        });
        if (legacy == legacyPanels.end()) {
            renamed.append(line);
        } else {
            renamed.append("[Window][")
                .append(legacy->title)
                .append("##")
                .append(legacy->type)
                .append(".1]")
                .append(line.substr(content.size()));
        }
        lineStart = next;
    }
    return renamed;
}
} // namespace

std::span<const DefaultWorkspace> defaultWorkspaces() {
    return workspaces;
}

const DefaultWorkspace& defaultWorkspaceFor(std::string_view name) {
    const auto found = std::ranges::find(workspaces, name, &DefaultWorkspace::name);
    return found == workspaces.end() ? workspaces.front() : *found;
}

WorkspaceRecord defaultWorkspaceRecord(const DefaultWorkspace& workspace) {
    WorkspaceRecord record{.name = std::string(workspace.name)};
    for (const DefaultArea& area : workspace.areas) {
        record.areas.push_back({.type = std::string(area.type), .instance = 1, .visible = true});
    }
    return record;
}

WorkspaceFileContents defaultWorkspaceFile() {
    WorkspaceFileContents contents{.activeWorkspace = std::string(workspaces.front().name)};
    for (const DefaultWorkspace& workspace : workspaces) {
        contents.workspaces.push_back(defaultWorkspaceRecord(workspace));
    }
    return contents;
}

WorkspaceFileContents migrateLegacyLayout(const LegacyEditorLayout& legacy) {
    WorkspaceFileContents contents = defaultWorkspaceFile();
    if (legacy.layoutVersion < lastLegacyLayoutVersion || legacy.imguiLayout.empty()) {
        // The old editor reset panel visibility along with a stale layout.
        return contents;
    }
    WorkspaceRecord& layout = contents.workspaces.front();
    for (const LegacyPanel& panel : legacyPanels) {
        const bool visible = legacy.panels.*panel.visible;
        const auto area = std::ranges::find(layout.areas, panel.type, &AreaRecord::type);
        if (area == layout.areas.end()) {
            layout.areas.push_back({.type = std::string(panel.type), .instance = 1, .visible = visible});
        } else {
            area->visible = visible;
        }
    }
    layout.imguiLayout = renameLegacyWindows(legacy.imguiLayout);
    return contents;
}
