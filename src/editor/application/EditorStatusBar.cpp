// EditorSession's status bar: the document, the active viewport's transform
// tools, frame statistics, and the play mode and module state.

#include "EditorSession.hpp"

#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/EditorWidgets.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <array>
#include <cstdio>

void EditorSession::drawStatusBar(const RenderStats& stats) {
    const float height = ImGui::GetFrameHeight() + editorArtPixel() * 2.0f;
    // A viewport side bar reserves its strip, so the dockspace ends above it
    // from the next frame on, as it does below the main menu bar.
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, toImVec4(palette::transparent));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {editorArtPixel() * 4.0f, editorArtPixel()});
    const bool open =
        ImGui::BeginViewportSideBar("##EditorStatus", ImGui::GetMainViewport(), ImGuiDir_Down, height, flags);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if (open) {
        m_themeChrome.paintBarWindow();
        constexpr const char* modeNames[] = {"Select", "Move", "Rotate", "Scale"};
        constexpr const char* spaceNames[] = {"Global", "Local"};
        std::array<char, 160> text{};
        const auto segment = [](const char* caption, ThemeColor ink) {
            ImGui::SameLine();
            widgets::CaptionText(caption, ink);
        };
        widgets::CaptionText(m_document.displayName().c_str(), palette::captionInk);
        if (m_diagnostics.hasUnacknowledgedError()) {
            segment("Diagnostics available", palette::warning);
        } else {
            segment("Ready", palette::success);
        }
        // The transform tools are the active viewport's.
        if (const ViewportPanel* activeViewport = m_viewports.active()) {
            const TransformToolSettings& tools = activeViewport->tools();
            std::snprintf(text.data(), text.size(), "Mode: %s  Space: %s  Snap: %s",
                          modeNames[static_cast<int>(tools.gizmo)], spaceNames[static_cast<int>(tools.space)],
                          tools.snapping ? "On" : "Off");
            segment(text.data(), palette::captionInk);
        } else {
            segment("No viewport", palette::inkMuted);
        }
        std::snprintf(text.data(), text.size(), "Selection: %zu  Draws %u  Visible %u  GPU %.2f ms", m_selection.size(),
                      stats.drawCalls, stats.visibleCount, stats.gpuFrameMilliseconds);
        segment(text.data(), palette::captionInk);
        ImGui::SameLine();
        drawPlayModePill();
        if (m_moduleReloadPending && m_playMode.mode() != EditorMode::Edit) {
            // The rebuilt module waits for Stop (EditorSession::updateGameModule).
            ImGui::SameLine();
            widgets::StatusPill(EditorIcon::Warning, "Reload pending - stop to apply",
                                widgets::StatusPillStyle::Warning);
        }
        if (m_moduleHost.status() != GameModuleStatus::None) {
            segment(gameModuleStatusText().c_str(), palette::captionInk);
        }
    }
    ImGui::End();
}

void EditorSession::drawPlayModePill() const {
    switch (m_playMode.mode()) {
    case EditorMode::Edit:
        if (m_standalone.running()) {
            widgets::StatusPill(EditorIcon::Play, "Standalone", widgets::StatusPillStyle::Lit);
        } else {
            widgets::StatusPill(EditorIcon::Stop, "Edit");
        }
        break;
    case EditorMode::Play:
        widgets::StatusPill(EditorIcon::Play, "Play", widgets::StatusPillStyle::Lit);
        break;
    case EditorMode::Simulate:
        widgets::StatusPill(EditorIcon::Play, "Simulate", widgets::StatusPillStyle::Lit);
        break;
    case EditorMode::Paused:
        widgets::StatusPill(EditorIcon::Pause, m_playMode.simulating() ? "Simulate Paused" : "Paused");
        break;
    }
}
