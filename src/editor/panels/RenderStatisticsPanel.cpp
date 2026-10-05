#include "RenderStatisticsPanel.hpp"

#include "editor/panels/EditorContext.hpp"

#include <imgui.h>

const char* RenderStatisticsPanel::title() const {
    return "Render Statistics";
}

void RenderStatisticsPanel::draw(EditorContext& context) {
    const RenderStats& stats = context.app.rendering.stats();
    ImGui::Text("Draw calls: %u", stats.drawCalls);
    ImGui::Text("Visible: %u / %u", stats.visibleCount, stats.renderableCount);
    ImGui::Text("Triangles: %llu", static_cast<unsigned long long>(stats.triangleCount));
    ImGui::Text("GPU frame: %.2f ms", stats.gpuFrameMilliseconds);
    ImGui::Text("CPU render packets: %.3f ms%s", stats.cpuRenderPacketMilliseconds,
                stats.renderPacketsRebuilt ? " (rebuilt)" : " (cached)");
    // Per-frame capacities the scene exceeded; shown only while it does.
    if (stats.droppedRenderItems > 0) {
        ImGui::Text("Over capacity, not drawn: %u", stats.droppedRenderItems);
    }
    if (stats.droppedSkinnedRenderItems > 0) {
        ImGui::Text("Over capacity, unskinned: %u", stats.droppedSkinnedRenderItems);
    }
    if (stats.droppedParticleEmitters > 0) {
        ImGui::Text("Over capacity, emitters skipped: %u", stats.droppedParticleEmitters);
    }
}
