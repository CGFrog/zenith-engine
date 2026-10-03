#include "editor/application/EditorAreaTypes.hpp"

#include "editor/panels/AssetBrowserPanel.hpp"
#include "editor/panels/BuildPanel.hpp"
#include "editor/panels/DiagnosticsPanel.hpp"
#include "editor/panels/GraphicsPanel.hpp"
#include "editor/panels/HierarchyPanel.hpp"
#include "editor/panels/InfoPanel.hpp"
#include "editor/panels/InspectorPanel.hpp"
#include "editor/panels/PropertiesPanel.hpp"
#include "editor/panels/RenderStatisticsPanel.hpp"

#include <memory>
#include <string>
#include <utility>

namespace {
AreaType perArea(std::string id, std::string title, AreaPanelFactory create) {
    return {.id = std::move(id),
            .title = std::move(title),
            .instancing = AreaInstancing::Multiple,
            .create = std::move(create)};
}
} // namespace

std::vector<AreaType> builtInAreaTypes(const EditorAreaServices& services) {
    ProjectAssets& projectAssets = services.projectAssets;
    RenderSettingsController& renderSettings = services.renderSettings;
    const EditorToolSettings& tools = services.tools;
    std::vector<AreaType> types;
    types.push_back(perArea("hierarchy", "Hierarchy", [] { return std::make_unique<HierarchyPanel>(); }));
    types.push_back(perArea("inspector", "Inspector", [] { return std::make_unique<InspectorPanel>(); }));
    types.push_back(perArea("properties", "Properties",
                            [&renderSettings] { return std::make_unique<PropertiesPanel>(renderSettings); }));
    types.push_back(
        perArea("diagnostics", "Diagnostics", [&tools] { return std::make_unique<DiagnosticsPanel>(tools); }));
    types.push_back(
        perArea("render_statistics", "Render Statistics", [] { return std::make_unique<RenderStatisticsPanel>(); }));
    // Browsers start secondary (hidden areas and other workspaces create
    // them too); the first one drawn while nobody is primary takes the role.
    types.push_back(perArea("asset_browser", "Asset Browser", [&projectAssets] {
        return std::make_unique<AssetBrowserPanel>(projectAssets, AssetBrowserRole::Secondary);
    }));
    types.push_back(
        perArea("graphics", "Graphics", [&renderSettings] { return std::make_unique<GraphicsPanel>(renderSettings); }));
    types.push_back(AreaType::shared("build", services.build));
    types.push_back(AreaType::shared("info", services.info));
    return types;
}
