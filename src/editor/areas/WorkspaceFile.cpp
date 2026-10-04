#include "editor/areas/WorkspaceFile.hpp"

#include "editor/areas/Workspace.hpp"
#include "engine/platform/VersionedJson.hpp"

#include <nlohmann/json.hpp>

#include <limits>

namespace {
using Json = nlohmann::json;

// Files from version 1 up to the current one are outdated.
const VersionedJsonFormat workspaceFileFormat{.description = "workspaces",
                                              .currentVersion = currentWorkspaceFileVersion};

WorkspaceFileResult parseArea(const Json& entry, AreaRecord& area) {
    if (!entry.is_object()) {
        return {.error = "an area must be an object"};
    }
    const auto type = entry.find("type");
    if (type == entry.end() || !type->is_string()) {
        return {.error = "an area needs a \"type\" string"};
    }
    area.type = type->get<std::string>();
    if (const auto instance = entry.find("instance"); instance != entry.end()) {
        if (!instance->is_number_unsigned() || instance->get<uint64_t>() == 0 ||
            instance->get<uint64_t>() > std::numeric_limits<uint32_t>::max()) {
            return {.error = "area \"" + area.type + "\" has an invalid instance number"};
        }
        area.instance = instance->get<uint32_t>();
    }
    if (const auto visible = entry.find("visible"); visible != entry.end()) {
        if (!visible->is_boolean()) {
            return {.error = "area \"" + area.type + "\" visible must be true or false"};
        }
        area.visible = visible->get<bool>();
    }
    if (const auto state = entry.find("state"); state != entry.end()) {
        if (!state->is_string()) {
            return {.error = "area \"" + area.type + "\" state must be a string"};
        }
        area.state = state->get<std::string>();
    }
    return {.success = true};
}

WorkspaceFileResult parseWorkspace(const Json& entry, WorkspaceRecord& workspace) {
    if (!entry.is_object()) {
        return {.error = "a workspace must be an object"};
    }
    const auto name = entry.find("name");
    if (name == entry.end() || !name->is_string() || name->get<std::string>().empty()) {
        return {.error = "a workspace needs a non-empty \"name\""};
    }
    workspace.name = name->get<std::string>();
    if (const auto layout = entry.find("imguiLayout"); layout != entry.end()) {
        if (!layout->is_string()) {
            return {.error = "workspace \"" + workspace.name + "\" imguiLayout must be a string"};
        }
        workspace.imguiLayout = layout->get<std::string>();
    }
    const auto areas = entry.find("areas");
    if (areas == entry.end()) {
        return {.success = true};
    }
    if (!areas->is_array()) {
        return {.error = "workspace \"" + workspace.name + "\" areas must be an array"};
    }
    for (const Json& areaEntry : *areas) {
        AreaRecord area;
        if (WorkspaceFileResult result = parseArea(areaEntry, area); !result) {
            return {.error = "workspace \"" + workspace.name + "\": " + result.error};
        }
        workspace.areas.push_back(std::move(area));
    }
    return {.success = true};
}

WorkspaceFileResult parse(const Json& document, WorkspaceFileContents& contents) {
    WorkspaceFileContents parsed;
    const auto workspaces = document.find("workspaces");
    if (workspaces == document.end() || !workspaces->is_array() || workspaces->empty()) {
        return {.error = "\"workspaces\" must be a non-empty array"};
    }
    for (const Json& entry : *workspaces) {
        WorkspaceRecord workspace;
        if (WorkspaceFileResult result = parseWorkspace(entry, workspace); !result) {
            return result;
        }
        for (const WorkspaceRecord& existing : parsed.workspaces) {
            if (existing.name == workspace.name) {
                return {.error = "workspace \"" + workspace.name + "\" appears twice"};
            }
        }
        parsed.workspaces.push_back(std::move(workspace));
    }
    if (const auto active = document.find("activeWorkspace"); active != document.end()) {
        if (!active->is_string()) {
            return {.error = "activeWorkspace must be a workspace name"};
        }
        parsed.activeWorkspace = active->get<std::string>();
    }
    contents = std::move(parsed);
    return {.success = true};
}

Json toJson(const WorkspaceFileContents& contents) {
    Json workspaces = Json::array();
    for (const WorkspaceRecord& workspace : contents.workspaces) {
        Json areas = Json::array();
        for (const AreaRecord& area : workspace.areas) {
            Json entry{{"type", area.type}, {"instance", area.instance}, {"visible", area.visible}};
            if (!area.state.empty()) {
                entry["state"] = area.state;
            }
            areas.push_back(std::move(entry));
        }
        workspaces.push_back(
            {{"name", workspace.name}, {"areas", std::move(areas)}, {"imguiLayout", workspace.imguiLayout}});
    }
    return {{"activeWorkspace", contents.activeWorkspace}, {"workspaces", std::move(workspaces)}};
}
} // namespace

WorkspaceFileResult loadWorkspaceFile(const std::filesystem::path& path, WorkspaceFileContents& contents) {
    const VersionedJsonResult read = readVersionedJson(path, workspaceFileFormat);
    if (!read) {
        return {.error = read.error};
    }
    if (!read.found) {
        return {.success = true, .found = false};
    }
    if (read.version < currentWorkspaceFileVersion) {
        return {.success = true, .outdated = true};
    }
    WorkspaceFileResult result = parse(read.document, contents);
    if (!result) {
        return {.error = "Invalid workspaces in " + path.string() + " (" + result.error + ")"};
    }
    return result;
}

WorkspaceFileResult saveWorkspaceFile(const std::filesystem::path& path, const WorkspaceFileContents& contents) {
    if (const PlatformResult written = writeVersionedJson(path, workspaceFileFormat, toJson(contents)); !written) {
        return {.error = written.error};
    }
    return {.success = true};
}

WorkspaceFileContents captureWorkspaces(const WorkspaceSet& workspaces) {
    WorkspaceFileContents contents;
    if (workspaces.empty()) {
        return contents;
    }
    contents.activeWorkspace = workspaces.active().name();
    for (const Workspace& workspace : workspaces.workspaces()) {
        WorkspaceRecord record{.name = workspace.name(), .imguiLayout = workspace.layout()};
        for (const AreaInstance& area : workspace.areas()) {
            record.areas.push_back(
                {.type = area.typeId, .instance = area.number, .visible = area.visible, .state = area.state()});
        }
        contents.workspaces.push_back(std::move(record));
    }
    return contents;
}

void restoreWorkspaces(const WorkspaceFileContents& contents, WorkspaceSet& workspaces,
                       std::vector<std::string>& warnings) {
    std::vector<Workspace> restored;
    for (const WorkspaceRecord& record : contents.workspaces) {
        Workspace& workspace = restored.emplace_back(record.name);
        workspace.setLayout(record.imguiLayout);
        for (const AreaRecord& area : record.areas) {
            if (AreaResult result = workspace.addStoredArea(area.type, area.instance, area.visible, area.state);
                !result) {
                warnings.push_back(result.error);
            }
        }
    }
    workspaces.assign(std::move(restored), contents.activeWorkspace);
}
