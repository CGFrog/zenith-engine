#include "NavigationSidebarTab.hpp"

#include "editor/panels/EditorContext.hpp"
#include "editor/theme/EditorWidgets.hpp"
#include "engine/module/GameServices.hpp"
#include "engine/navigation/NavigationWorld.hpp"

#include <imgui.h>

namespace {
void drawStats(const NavigationWorld& navigation) {
    const NavMeshStats& stats = navigation.stats();
    ImGui::Text("%u polygons, %u vertices", stats.polygons, stats.vertices);
    ImGui::TextDisabled("From %u input triangles", stats.inputTriangles);
}
} // namespace

void drawNavigationSidebarTab(EditorContext& context, bool& showNavigationMesh) {
    ImGui::SeparatorText("Preview");
    if (context.navigationPreview == nullptr) {
        // A prefab window: navigation.bake bakes the level, not this view.
        ImGui::TextDisabled("Navigation meshes are baked from the level.");
        return;
    }
    if (context.navigationPreview != nullptr && context.navigationPreview->baked()) {
        drawStats(*context.navigationPreview);
    } else {
        ImGui::TextDisabled("Not baked.");
    }
    ImGui::BeginDisabled(!context.canInvoke("navigation.bake"));
    if (widgets::Button("Bake")) {
        context.invoke("navigation.bake");
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Bake the edit scene's static colliders with the project's settings (Edit mode)");
    ImGui::SameLine();
    ImGui::BeginDisabled(!context.canInvoke("navigation.clear"));
    if (widgets::Button("Clear")) {
        context.invoke("navigation.clear");
    }
    ImGui::EndDisabled();
    ImGui::Checkbox("Show Navigation Mesh", &showNavigationMesh);

    if (context.gameServices != nullptr) {
        ImGui::SeparatorText("Game");
        if (const NavigationWorld* game = context.gameServices->navigationIfBaked(); game != nullptr && game->baked()) {
            drawStats(*game);
            ImGui::Text("%zu agents", context.gameServices->navigationCrowd().agentCount());
        } else {
            ImGui::TextDisabled("The game has not baked a mesh.");
        }
    }

    ImGui::SeparatorText("Settings");
    if (context.navigationSettings != nullptr) {
        const NavMeshBuildSettings& settings = *context.navigationSettings;
        ImGui::Text("Agent %.2f m tall, %.2f m radius", settings.agentHeight, settings.agentRadius);
        ImGui::Text("Climbs %.2f m, slopes to %.0f deg", settings.agentMaxClimb, settings.agentMaxSlopeDegrees);
        ImGui::TextDisabled("Cells %.2f x %.2f m", settings.cellSize, settings.cellHeight);
    }
    ImGui::BeginDisabled(!context.canInvoke("project.settings"));
    if (widgets::Button("Edit Settings...")) {
        context.invoke("project.settings", R"({"tab":"navigation"})");
    }
    ImGui::EndDisabled();
}
