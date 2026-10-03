#include "editor/areas/WorkspaceFile.hpp"

#include "editor/areas/Workspace.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <limits>
#include <sstream>

namespace {
using Json = nlohmann::json;

// The first version; files from 1 up to the current one are outdated.
constexpr int firstVersion = 1;

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

WorkspaceFileResult parse(const std::string& text, WorkspaceFileContents& contents) {
    const Json document = Json::parse(text, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return {.error = "the file is not a JSON object"};
    }
    const auto version = document.find("version");
    if (version == document.end() || !version->is_number_integer() || version->get<int64_t>() < firstVersion ||
        version->get<int64_t>() > currentWorkspaceFileVersion) {
        return {.error = "unsupported version (expected " + std::to_string(currentWorkspaceFileVersion) + ")"};
    }
    if (version->get<int64_t>() < currentWorkspaceFileVersion) {
        return {.success = true, .outdated = true};
    }
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
    return {{"version", currentWorkspaceFileVersion},
            {"activeWorkspace", contents.activeWorkspace},
            {"workspaces", std::move(workspaces)}};
}
} // namespace

WorkspaceFileResult loadWorkspaceFile(const std::filesystem::path& path, WorkspaceFileContents& contents) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        if (error) {
            return {.error = "Cannot inspect workspaces " + path.string() + ": " + error.message()};
        }
        return {.success = true, .found = false};
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {.error = "Cannot read workspaces " + path.string()};
    }
    std::ostringstream text;
    text << input.rdbuf();
    WorkspaceFileResult result = parse(text.str(), contents);
    if (!result) {
        return {.error = "Invalid workspaces in " + path.string() + " (" + result.error + ")"};
    }
    return result;
}

WorkspaceFileResult saveWorkspaceFile(const std::filesystem::path& path, const WorkspaceFileContents& contents) {
    std::error_code error;
    if (const std::filesystem::path parent = path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) {
            return {.error = "Cannot create the workspaces folder: " + error.message()};
        }
    }
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            return {.error = "Cannot write workspaces " + temporary.string()};
        }
        output << toJson(contents).dump(2) << '\n';
        if (!output) {
            return {.error = "Cannot finish writing workspaces " + temporary.string()};
        }
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return {.error = "Cannot replace workspaces " + path.string() + ": " + error.message()};
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
