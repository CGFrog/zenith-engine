#include "editor/panels/GameModulePanel.hpp"

#include "editor/modules/GameModuleHost.hpp"
#include "editor/panels/EditorContext.hpp"

#include <imgui.h>

#include <algorithm>
#include <utility>

std::string gameModuleAreaTypeId(std::string_view panelTitle) {
    std::string id = "module:" + std::string(panelTitle);
    std::ranges::replace_if(id, [](char character) { return character == '.' || character == '#'; }, '_');
    return id;
}

GameModulePanel::GameModulePanel(GameModuleHost& host, std::string panelTitle)
    : m_host(host)
    , m_panelTitle(std::move(panelTitle)) {}

const char* GameModulePanel::title() const {
    return m_panelTitle.c_str();
}

void GameModulePanel::draw(EditorContext& context) {
    // A reload requested while module code draws is deferred to the next
    // update, so the panel stays valid for this call.
    const auto panels = m_host.panels();
    const auto panel = std::ranges::find_if(
        panels, [this](const GameModuleHost::Panel& candidate) { return candidate.panel.title == m_panelTitle; });
    if (panel == panels.end() || !panel->panel.draw) {
        ImGui::TextDisabled("The game module has no panel named \"%s\".", m_panelTitle.c_str());
        return;
    }
    drawModulePanelContents(context, panel->panel.draw);
}
