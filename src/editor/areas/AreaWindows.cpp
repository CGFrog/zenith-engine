#include "editor/areas/AreaWindows.hpp"

#include "editor/areas/AreaRegistry.hpp"
#include "editor/areas/DefaultWorkspaces.hpp"
#include "editor/areas/Workspace.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

namespace {
AreaWindowRequest drawAreaHeader(const AreaType& type, const AreaRegistry& registry) {
    AreaWindowRequest request;
    // The label is the editor type; the id keeps it stable across types.
    const std::string label = type.title + "###editor_type";
    if (!ImGui::BeginMenu(label.c_str())) {
        ImGui::SetItemTooltip("Editor type: choose what this area shows");
        return request;
    }
    for (const AreaType& candidate : registry.types()) {
        ImGui::PushID(candidate.id.c_str());
        const bool current = candidate.id == type.id;
        if (ImGui::MenuItem(candidate.title.c_str(), nullptr, current) && !current) {
            request = {.kind = AreaWindowRequest::Kind::ChangeType, .typeId = candidate.id};
        }
        ImGui::PopID();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("New Area", nullptr, false, type.instancing == AreaInstancing::Multiple)) {
        request = {.kind = AreaWindowRequest::Kind::NewArea, .typeId = type.id};
    }
    ImGui::SetItemTooltip("Open another %s area beside this one", type.title.c_str());
    if (ImGui::MenuItem("Close Area")) {
        request = {.kind = AreaWindowRequest::Kind::Close};
    }
    ImGui::EndMenu();
    return request;
}
} // namespace

AreaWindowResult drawAreaWindow(AreaInstance& area, const AreaType& type, const AreaRegistry& registry, int windowFlags,
                                const std::function<void()>& drawContents) {
    if (area.dockRequest != 0) {
        ImGui::SetNextWindowDockID(area.dockRequest, ImGuiCond_Always);
        area.dockRequest = 0;
    }
    AreaWindowResult result;
    bool open = true;
    const std::string name = area.windowName(type.title);
    if (ImGui::Begin(name.c_str(), &open, windowFlags | ImGuiWindowFlags_MenuBar)) {
        if (ImGui::BeginMenuBar()) {
            result.request = drawAreaHeader(type, registry);
            ImGui::EndMenuBar();
        }
        drawContents();
    }
    result.dockId = ImGui::GetWindowDockID();
    ImGui::End();
    if (!open) {
        result.request = {.kind = AreaWindowRequest::Kind::Close};
    }
    return result;
}

std::string captureImGuiLayout() {
    size_t size = 0;
    const char* text = ImGui::SaveIniSettingsToMemory(&size);
    ImGui::GetIO().WantSaveIniSettings = false;
    return {text, size};
}

void restoreImGuiLayout(std::string_view layout) {
    ImGui::ClearIniSettings();
    if (!layout.empty()) {
        ImGui::LoadIniSettingsFromMemory(layout.data(), layout.size());
    }
}

bool applyRequestedWorkspace(WorkspaceSet& workspaces) {
    if (!workspaces.requestedActivation()) {
        return false;
    }
    workspaces.active().setLayout(captureImGuiLayout());
    workspaces.completeActivation();
    restoreImGuiLayout(workspaces.active().layout());
    return true;
}

void buildDefaultDockLayout(uint32_t dockspaceId, const DefaultWorkspace& defaults, const Workspace& workspace,
                            const AreaRegistry& registry) {
    using enum DefaultDockSlot;
    // The default placement of each visible area of a registered type.
    std::vector<std::pair<std::string, DefaultDockSlot>> placements;
    for (const AreaInstance& area : workspace.areas()) {
        const AreaType* type = registry.find(area.typeId);
        const DefaultArea* placement = defaultAreaFor(defaults, area.typeId, area.number);
        if (area.visible && type != nullptr && placement != nullptr && placement->slot != Floating) {
            placements.emplace_back(area.windowName(type->title), placement->slot);
        }
    }
    const auto slotUsed = [&](DefaultDockSlot slot) {
        return std::ranges::any_of(placements, [slot](const auto& placement) { return placement.second == slot; });
    };

    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

    std::array<ImGuiID, 6> nodes{};
    ImGuiID center = dockspaceId;
    if (slotUsed(Left)) {
        nodes[static_cast<size_t>(Left)] = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20f, nullptr, &center);
    }
    if (slotUsed(Right) || slotUsed(RightBottom)) {
        ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.25f, nullptr, &center);
        nodes[static_cast<size_t>(Right)] = right;
        nodes[static_cast<size_t>(RightBottom)] = right;
        if (slotUsed(Right) && slotUsed(RightBottom)) {
            nodes[static_cast<size_t>(RightBottom)] =
                ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.70f, nullptr, &right);
            nodes[static_cast<size_t>(Right)] = right;
        }
    }
    if (slotUsed(Bottom)) {
        nodes[static_cast<size_t>(Bottom)] =
            ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.25f, nullptr, &center);
    }
    nodes[static_cast<size_t>(Center)] = center;

    for (const auto& [windowName, slot] : placements) {
        ImGui::DockBuilderDockWindow(windowName.c_str(), nodes[static_cast<size_t>(slot)]);
    }
    ImGui::DockBuilderFinish(dockspaceId);
}
