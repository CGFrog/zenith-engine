// EditorSession's areas and workspaces: the editor types areas can show, the
// workspaces and their persistence, the wm.* operators that change them, and
// drawing the active workspace's areas.

#include "EditorSession.hpp"

#include "editor/areas/AreaWindows.hpp"
#include "editor/areas/DefaultWorkspaces.hpp"
#include "editor/areas/WorkspaceFile.hpp"
#include "editor/panels/DiagnosticsPanel.hpp"
#include "editor/panels/GameModulePanel.hpp"
#include "editor/panels/HierarchyPanel.hpp"
#include "editor/panels/InspectorPanel.hpp"
#include "editor/panels/PropertiesPanel.hpp"
#include "editor/panels/RenderStatisticsPanel.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <utility>

namespace {
constexpr const char* workspaceDiagnosticSource = "Workspaces";
constexpr std::string_view moduleAreaTypePrefix = "module:";

struct TogglePanelProperties {
    // The area type id, for example "inspector".
    std::string panel;
};

struct NewAreaProperties {
    std::string type;
};

struct ChangeAreaTypeProperties {
    // The area id, for example "hierarchy.2".
    std::string area;
    std::string type;
};

struct CloseAreaProperties {
    std::string area;
};

struct WorkspaceProperties {
    std::string workspace;
};

template <typename Properties, typename Function>
std::function<OperatorResult(OperatorContext&, OperatorProperties&)> typedExecute(Function run) {
    return [run = std::move(run)](OperatorContext&, OperatorProperties& properties) {
        Properties* values = properties.get<Properties>();
        if (values == nullptr) {
            return OperatorResult::failed("Operator properties have the wrong type");
        }
        return run(*values);
    };
}

OperatorResult areaOperatorResult(const AreaResult& result) {
    return result ? OperatorResult::finished() : OperatorResult::failed(result.error);
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream text;
    if (input) {
        text << input.rdbuf();
    }
    return text.str();
}

Workspace defaultWorkspace(std::string_view name) {
    const WorkspaceRecord record = defaultWorkspaceRecord(defaultWorkspaceFor(name));
    Workspace workspace{std::string(name)};
    for (const AreaRecord& area : record.areas) {
        workspace.addStoredArea(area.type, area.instance, area.visible);
    }
    return workspace;
}
} // namespace

void EditorSession::addAreaType(AreaType type) {
    if (const AreaRegistryResult result = m_areaTypes.add(std::move(type)); !result) {
        m_diagnostics.reportError(result.error);
    }
}

void EditorSession::registerAreaTypes() {
    addAreaType({.id = "viewport", .title = "Viewport", .instancing = AreaInstancing::Multiple, .create = [this] {
                     return std::make_unique<ViewportPanel>(m_viewports);
                 }});
    addAreaType({.id = "hierarchy", .title = "Hierarchy", .instancing = AreaInstancing::Multiple, .create = [] {
                     return std::make_unique<HierarchyPanel>();
                 }});
    addAreaType({.id = "inspector", .title = "Inspector", .instancing = AreaInstancing::Multiple, .create = [] {
                     return std::make_unique<InspectorPanel>();
                 }});
    addAreaType({.id = "properties", .title = "Properties", .instancing = AreaInstancing::Multiple, .create = [this] {
                     return std::make_unique<PropertiesPanel>(m_renderSettings);
                 }});
    addAreaType({.id = "diagnostics", .title = "Diagnostics", .instancing = AreaInstancing::Multiple, .create = [this] {
                     return std::make_unique<DiagnosticsPanel>(m_settings.tools());
                 }});
    addAreaType(
        {.id = "render_statistics", .title = "Render Statistics", .instancing = AreaInstancing::Multiple, .create = [] {
             return std::make_unique<RenderStatisticsPanel>();
         }});
    addAreaType(AreaType::shared("asset_browser", m_assetBrowser));
    addAreaType(AreaType::shared("graphics", m_graphics));
    addAreaType(AreaType::shared("build", m_buildPanel));
    addAreaType(AreaType::shared("info", m_infoPanel));
}

void EditorSession::registerAreaOperators() {
    registerOperator({.id = "wm.toggle_panel",
                      .label = "Toggle Panel",
                      .description = "Show or hide an editor type in the active workspace",
                      .flags = OperatorFlags::Internal,
                      .properties = OperatorProperties::make(
                          TogglePanelProperties{},
                          std::move(Fields<TogglePanelProperties>().add("panel", &TogglePanelProperties::panel))),
                      .execute = typedExecute<TogglePanelProperties>([this](TogglePanelProperties& values) {
                          m_workspacesDirty = true;
                          return areaOperatorResult(m_workspaces.active().toggle(m_areaTypes, values.panel));
                      })});
    registerOperator(
        {.id = "wm.area_new",
         .label = "New Area",
         .description = "Open another area of an editor type beside the existing one",
         .flags = OperatorFlags::Internal,
         .properties = OperatorProperties::make(
             NewAreaProperties{}, std::move(Fields<NewAreaProperties>().add("type", &NewAreaProperties::type))),
         .execute = typedExecute<NewAreaProperties>([this](NewAreaProperties& values) {
             m_workspacesDirty = true;
             return areaOperatorResult(m_workspaces.active().openNew(m_areaTypes, values.type));
         })});
    registerOperator(
        {.id = "wm.area_change_type",
         .label = "Change Editor Type",
         .description = "Show another editor type in an area's place",
         .flags = OperatorFlags::Internal,
         .properties = OperatorProperties::make(ChangeAreaTypeProperties{},
                                                std::move(Fields<ChangeAreaTypeProperties>()
                                                              .add("area", &ChangeAreaTypeProperties::area)
                                                              .add("type", &ChangeAreaTypeProperties::type))),
         .execute = typedExecute<ChangeAreaTypeProperties>([this](ChangeAreaTypeProperties& values) {
             m_workspacesDirty = true;
             return areaOperatorResult(m_workspaces.active().changeType(m_areaTypes, values.area, values.type));
         })});
    registerOperator(
        {.id = "wm.area_close",
         .label = "Close Area",
         .description = "Close an area of the active workspace",
         .flags = OperatorFlags::Internal,
         .properties = OperatorProperties::make(
             CloseAreaProperties{}, std::move(Fields<CloseAreaProperties>().add("area", &CloseAreaProperties::area))),
         .execute = typedExecute<CloseAreaProperties>([this](CloseAreaProperties& values) {
             m_workspacesDirty = true;
             return areaOperatorResult(m_workspaces.active().close(values.area));
         })});
    registerOperator({.id = "wm.workspace_activate",
                      .label = "Switch Workspace",
                      .description = "Show another workspace's areas and layout",
                      .flags = OperatorFlags::Internal,
                      .properties = OperatorProperties::make(
                          WorkspaceProperties{},
                          std::move(Fields<WorkspaceProperties>().add("workspace", &WorkspaceProperties::workspace))),
                      .execute = typedExecute<WorkspaceProperties>([this](WorkspaceProperties& values) {
                          // Applied between frames by updateWorkspaces.
                          return areaOperatorResult(m_workspaces.requestActivation(values.workspace));
                      })});
    registerOperator(applicationOperator("wm.workspace_reset", "Reset Workspace",
                                         "Restore the active workspace's default areas and layout", {},
                                         [this](ApplicationContext&) {
                                             Workspace& active = m_workspaces.active();
                                             active = defaultWorkspace(active.name());
                                             active.sync(m_areaTypes);
                                             m_buildDefaultLayout = true;
                                             m_workspacesDirty = true;
                                         }));
}

void EditorSession::syncAreaTypes() {
    std::vector<std::string> moduleTypes;
    for (const GameModuleHost::Panel& panel : m_moduleHost.panels()) {
        std::string id = gameModuleAreaTypeId(panel.panel.title);
        // Titles that differ only in '.' or '#' share an id; the first wins.
        if (m_areaTypes.find(id) == nullptr) {
            addAreaType({.id = id,
                         .title = panel.panel.title,
                         .instancing = AreaInstancing::Single,
                         .create = [this, title = panel.panel.title] {
                             return std::make_unique<GameModulePanel>(m_moduleHost, title);
                         }});
        }
        moduleTypes.push_back(std::move(id));
    }
    std::vector<std::string> stale;
    for (const AreaType& type : m_areaTypes.types()) {
        if (type.id.starts_with(moduleAreaTypePrefix) && std::ranges::find(moduleTypes, type.id) == moduleTypes.end()) {
            stale.push_back(type.id);
        }
    }
    for (const std::string& id : stale) {
        m_areaTypes.remove(id);
    }
    m_workspaces.sync(m_areaTypes);
}

void EditorSession::loadWorkspaces() {
    WorkspaceFileContents contents;
    const WorkspaceFileResult loaded = loadWorkspaceFile(m_workspacesPath, contents);
    if (!loaded) {
        // Keep the unreadable file for the user to repair; the next save
        // would otherwise replace it with the defaults.
        std::filesystem::path backup = m_workspacesPath;
        backup += ".bad";
        std::error_code error;
        std::filesystem::rename(m_workspacesPath, backup, error);
        m_diagnostics.add(EditorDiagnosticSeverity::Warning, workspaceDiagnosticSource,
                          loaded.error + "; using the default workspaces" +
                              (error ? "." : " and keeping the file as " + backup.string() + "."));
        contents = defaultWorkspaceFile();
    } else if (!loaded.found) {
        contents = migrateLegacyLayout({.panels = m_settings.panels(),
                                        .layoutVersion = m_settings.layoutVersion(),
                                        .imguiLayout = readText(m_legacyLayoutPath)});
        m_workspacesDirty = true;
    }
    std::vector<std::string> warnings;
    restoreWorkspaces(contents, m_workspaces, warnings);
    for (const std::string& warning : warnings) {
        m_diagnostics.add(EditorDiagnosticSeverity::Warning, workspaceDiagnosticSource, warning);
    }
    m_workspaces.sync(m_areaTypes);
    restoreImGuiLayout(m_workspaces.active().layout());
    m_buildDefaultLayout = m_workspaces.active().layout().empty();
}

void EditorSession::saveWorkspaces() {
    if (const WorkspaceFileResult result = saveWorkspaceFile(m_workspacesPath, captureWorkspaces(m_workspaces));
        !result) {
        m_diagnostics.reportError(result.error);
    }
    m_workspacesDirty = false;
}

void EditorSession::updateWorkspaces() {
    if (m_workspaces.empty()) {
        return;
    }
    if (applyRequestedWorkspace(m_workspaces)) {
        m_buildDefaultLayout = m_workspaces.active().layout().empty();
        m_workspacesDirty = true;
    }
    // ImGui asks to save (at most every few seconds) after windows move or
    // docking changes; it has no file of its own, so the workspace keeps it.
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantSaveIniSettings || m_workspacesDirty) {
        m_workspaces.active().setLayout(captureImGuiLayout());
        saveWorkspaces();
    }
}

void EditorSession::updateAreas(ApplicationContext& context, const InputState& input, float deltaSeconds) {
    EditorContext editor = editorContext(context);
    // Snapshot: an update may run an operator that closes or adds areas.
    std::vector<std::shared_ptr<EditorPanel>> panels;
    for (const AreaInstance& area : m_workspaces.active().areas()) {
        if (area.panel) {
            panels.push_back(area.panel);
        }
    }
    for (const std::shared_ptr<EditorPanel>& panel : panels) {
        panel->update(editor, input, deltaSeconds);
    }
}

void EditorSession::drawAreas(ApplicationContext& context) {
    EditorContext editor = editorContext(context);
    Workspace& workspace = m_workspaces.active();
    // Snapshot: a panel may run an operator that closes, adds, or retypes
    // areas while it draws, so each area is looked up again by id.
    std::vector<std::pair<std::string, std::shared_ptr<EditorPanel>>> areas;
    for (const AreaInstance& area : workspace.areas()) {
        areas.emplace_back(area.id(), area.panel);
    }
    std::vector<std::pair<std::string, AreaWindowRequest>> requests;
    for (const auto& entry : areas) {
        const std::string& id = entry.first;
        const std::shared_ptr<EditorPanel>& panel = entry.second;
        AreaInstance* area = workspace.find(id);
        if (area == nullptr || !panel || area->panel != panel) {
            continue;
        }
        const AreaType* type = m_areaTypes.find(area->typeId);
        if (type == nullptr) {
            continue;
        }
        if (!area->visible) {
            panel->hidden(editor);
            continue;
        }
        const AreaWindowResult result = drawAreaWindow(*area, *type, m_areaTypes, panel->windowFlags(), [&] {
            const bool readOnly = panel->editModeOnly() && !editor.editMode();
            ImGui::BeginDisabled(readOnly);
            panel->draw(editor);
            ImGui::EndDisabled();
        });
        if (AreaInstance* drawn = workspace.find(id)) {
            drawn->dockId = result.dockId;
        }
        if (result.request.kind != AreaWindowRequest::Kind::None) {
            requests.emplace_back(id, result.request);
        }
    }
    for (const auto& [id, request] : requests) {
        switch (request.kind) {
        case AreaWindowRequest::Kind::None:
            break;
        case AreaWindowRequest::Kind::Close:
            invokeOperator(context, "wm.area_close", nlohmann::json{{"area", id}}.dump(), KeymapRegion::Global);
            break;
        case AreaWindowRequest::Kind::ChangeType:
            invokeOperator(context, "wm.area_change_type",
                           nlohmann::json{{"area", id}, {"type", request.typeId}}.dump(), KeymapRegion::Global);
            break;
        case AreaWindowRequest::Kind::NewArea:
            invokeOperator(context, "wm.area_new", nlohmann::json{{"type", request.typeId}}.dump(),
                           KeymapRegion::Global);
            break;
        }
    }
}

void EditorSession::drawAreaMenuItems(ApplicationContext& context) {
    const Workspace& workspace = m_workspaces.active();
    // Module panels are listed in the Module menu.
    for (const AreaType& type : m_areaTypes.types()) {
        if (!type.id.starts_with(moduleAreaTypePrefix)) {
            operatorMenuItem(context, "wm.toggle_panel", type.title.c_str(), nlohmann::json{{"panel", type.id}}.dump(),
                             workspace.anyVisible(type.id));
        }
    }
    if (ImGui::BeginMenu("New Area")) {
        for (const AreaType& type : m_areaTypes.types()) {
            if (type.instancing == AreaInstancing::Multiple) {
                operatorMenuItem(context, "wm.area_new", type.title.c_str(), nlohmann::json{{"type", type.id}}.dump());
            }
        }
        ImGui::EndMenu();
    }
    operatorMenuItem(context, "wm.workspace_reset", "Reset Workspace");
}

void EditorSession::drawWorkspaceTabs(ApplicationContext& context) {
    ImGui::Separator();
    if (!ImGui::BeginTabBar("##workspaces", ImGuiTabBarFlags_FittingPolicyScroll)) {
        return;
    }
    const std::string active = m_workspaces.active().name();
    std::optional<std::string> chosen;
    for (const Workspace& workspace : m_workspaces.workspaces()) {
        const bool isActive = workspace.name() == active;
        // The active workspace decides the selected tab; a click requests a
        // switch, which selects its tab once applied.
        const bool open =
            ImGui::BeginTabItem(workspace.name().c_str(), nullptr, isActive ? ImGuiTabItemFlags_SetSelected : 0);
        if (!isActive && ImGui::IsItemClicked()) {
            chosen = workspace.name();
        }
        if (open) {
            ImGui::EndTabItem();
        }
    }
    ImGui::EndTabBar();
    if (chosen) {
        invokeOperator(context, "wm.workspace_activate", nlohmann::json{{"workspace", *chosen}}.dump(),
                       KeymapRegion::Global);
    }
}

bool EditorSession::updateSmokeAreas(ApplicationContext& context) {
    // Frames 6-9: open a second Hierarchy, switch workspace and back, check
    // the area survived, and restore the areas it changed (the smoke shares
    // the user's workspaces.json).
    constexpr uint32_t firstFrame = 6;
    const auto fail = [&](const std::string& message) {
        const std::string error = "Workspace smoke failed: " + message;
        std::cerr << error << '\n';
        m_diagnostics.reportError(error);
        context.control.requestSceneChange({std::numeric_limits<uint32_t>::max()});
    };
    const auto hierarchyAreas = [this] {
        std::vector<std::string> ids;
        for (const AreaInstance& area : m_workspaces.active().areas()) {
            if (area.typeId == "hierarchy" && area.visible) {
                ids.push_back(area.id());
            }
        }
        return ids;
    };
    if (m_smokeFrame == firstFrame) {
        m_smokeWorkspace = m_workspaces.active().name();
        if (!m_workspaces.active().anyVisible("hierarchy")) {
            m_smokeShowedHierarchy = true;
            invokeOperator(context, "wm.toggle_panel", R"({"panel":"hierarchy"})", KeymapRegion::Global);
        }
        const std::vector<std::string> before = hierarchyAreas();
        invokeOperator(context, "wm.area_new", R"({"type":"hierarchy"})", KeymapRegion::Global);
        for (const std::string& id : hierarchyAreas()) {
            if (std::ranges::find(before, id) == before.end()) {
                m_smokeArea = id;
            }
        }
        if (m_smokeArea.empty()) {
            fail("wm.area_new did not open a second Hierarchy");
        }
    } else if (m_smokeFrame == firstFrame + 1) {
        const std::string other = m_smokeWorkspace == "Debug" ? "Layout" : "Debug";
        invokeOperator(context, "wm.workspace_activate", nlohmann::json{{"workspace", other}}.dump(),
                       KeymapRegion::Global);
    } else if (m_smokeFrame == firstFrame + 2) {
        if (m_workspaces.active().name() == m_smokeWorkspace) {
            fail("the workspace did not switch");
        }
        invokeOperator(context, "wm.workspace_activate", nlohmann::json{{"workspace", m_smokeWorkspace}}.dump(),
                       KeymapRegion::Global);
    } else if (m_smokeFrame == firstFrame + 3) {
        const AreaInstance* area = m_workspaces.active().find(m_smokeArea);
        if (m_workspaces.active().name() != m_smokeWorkspace || area == nullptr || !area->visible) {
            fail("switching back did not restore the second Hierarchy");
        }
        invokeOperator(context, "wm.area_close", nlohmann::json{{"area", m_smokeArea}}.dump(), KeymapRegion::Global);
        if (m_smokeShowedHierarchy) {
            invokeOperator(context, "wm.toggle_panel", R"({"panel":"hierarchy"})", KeymapRegion::Global);
        }
    }
    return m_smokeFrame > firstFrame + 3;
}
