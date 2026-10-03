#pragma once

#include "editor/areas/AreaRegistry.hpp"

#include <vector>

class BuildPanel;
class ComponentInspectorLayout;
class InfoPanel;
class ProjectAssets;
class RenderSettingsController;
struct EditorToolSettings;

// What the editor's built-in area types show. Every object must outlive the
// panels the types create.
struct EditorAreaServices {
    // Shared by every Asset Browser area, and read by every Hierarchy for
    // its Orphan Data view.
    ProjectAssets& projectAssets;
    // Shared by every Properties and Graphics area.
    RenderSettingsController& renderSettings;
    // Shared by every Inspector and Properties area.
    ComponentInspectorLayout& componentLayout;
    const EditorToolSettings& tools;
    // Panels the session owns because other workflows use them: module
    // reloads wait for the build, and the smoke checks the engine log.
    BuildPanel& build;
    InfoPanel& info;
};

// The built-in editor types except the Viewport, which the session registers
// itself, in View menu order (after the Viewport):
//
// | id                  | instancing | panel                                    |
// | ------------------- | ---------- | ---------------------------------------- |
// | `hierarchy`         | Multiple   | HierarchyPanel (display mode per area)   |
// | `inspector`         | Multiple   | InspectorPanel                           |
// | `properties`        | Multiple   | PropertiesPanel (tab kept per area)      |
// | `diagnostics`       | Multiple   | DiagnosticsPanel                         |
// | `render_statistics` | Multiple   | RenderStatisticsPanel                    |
// | `asset_browser`     | Multiple   | AssetBrowserPanel over projectAssets     |
// | `graphics`          | Multiple   | GraphicsPanel over renderSettings        |
// | `build`             | Single     | the session's BuildPanel (one runner)    |
// | `info`              | Single     | the session's InfoPanel                  |
//
// Build stays single-instance because one BuildRunner owns the build process
// and its output; Info because the session reads its merged log, which each
// extra instance would copy and re-merge from the same streams.
std::vector<AreaType> builtInAreaTypes(const EditorAreaServices& services);
