#include "SystemProfilerPanel.hpp"

#include "editor/application/EditorPlayMode.hpp"
#include "editor/interaction/GraphicsSettingsEditor.hpp"
#include "editor/modules/GameModuleHost.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/panels/SimulateSystemPicker.hpp"
#include "editor/theme/EditorWidgets.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdio>

namespace {
constexpr ImGuiTableFlags tableFlags =
    ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;

const char* modeName(const EditorPlayMode& playMode) {
    switch (playMode.mode()) {
    case EditorMode::Edit:
        return "Edit";
    case EditorMode::Play:
        return "Play";
    case EditorMode::Simulate:
        return "Simulate";
    case EditorMode::Paused:
        return playMode.simulating() ? "Simulate (paused)" : "Play (paused)";
    }
    return "Edit";
}

const char* runInName(RunIn runIn) {
    const auto flags = static_cast<uint32_t>(runIn);
    const bool edit = (flags & static_cast<uint32_t>(RunIn::Edit)) != 0;
    const bool play = (flags & static_cast<uint32_t>(RunIn::Play)) != 0;
    if (edit && play) {
        return "Edit, Play";
    }
    return edit ? "Edit" : (play ? "Play" : "None");
}

void timingCell(double milliseconds) {
    ImGui::TableNextColumn();
    ImGui::Text("%.3f", milliseconds);
}

void drawEngineTimings(const EngineFrameTimings* timings) {
    if (timings == nullptr || timings->frame == 0) {
        ImGui::TextDisabled("Engine timings are not available yet.");
        return;
    }
    std::array<char, 48> fixedLabel{};
    std::snprintf(fixedLabel.data(), fixedLabel.size(), "Fixed update (%u steps)", timings->fixedSteps);
    const std::array<std::pair<const char*, double>, 10> rows{{
        {"Input", timings->inputMilliseconds},
        {"Update", timings->updateMilliseconds},
        {"Animation", timings->animationMilliseconds},
        {fixedLabel.data(), timings->fixedUpdateMilliseconds},
        {"  Physics", timings->physicsMilliseconds},
        {"Transforms", timings->transformsMilliseconds},
        {"UI build", timings->uiBuildMilliseconds},
        {"Renderer", timings->rendererMilliseconds},
        {"Audio", timings->audioMilliseconds},
        {"Frame", timings->totalMilliseconds},
    }};
    if (!ImGui::BeginTable("Engine CPU timings", 2, tableFlags)) {
        return;
    }
    ImGui::TableSetupColumn("Phase", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn("CPU ms", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();
    for (const auto& [name, milliseconds] : rows) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(name);
        timingCell(milliseconds);
    }
    ImGui::EndTable();
}
} // namespace

SystemProfilerPanel::SystemProfilerPanel(const GameModuleHost& modules, const EditorPlayMode& playMode)
    : m_modules(modules)
    , m_playMode(playMode) {}

const char* SystemProfilerPanel::title() const {
    return "System Profiler";
}

void SystemProfilerPanel::draw(EditorContext& context) {
    drawRuntimeControls(context);
    if (widgets::SectionHeader("Systems", ImGuiTreeNodeFlags_DefaultOpen)) {
        drawSystems(context);
    }
    if (widgets::SectionHeader("Engine CPU", ImGuiTreeNodeFlags_DefaultOpen)) {
        drawEngineTimings(context.app.frameTimings);
    }
    if (widgets::SectionHeader("GPU Passes", ImGuiTreeNodeFlags_DefaultOpen)) {
        GraphicsSettingsEditor::drawPassTimings(context.app.rendering.stats());
    }
}

void SystemProfilerPanel::drawRuntimeControls(EditorContext& context) const {
    ImGui::Text("Mode: %s", modeName(m_playMode));
    ImGui::SameLine();
    float scale = m_playMode.timeScale();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
    if (ImGui::DragFloat("Time scale", &scale, 0.01f, SimulationClock::minimumTimeScale,
                         SimulationClock::maximumTimeScale, "%.2fx", ImGuiSliderFlags_AlwaysClamp)) {
        context.invoke("play.set_time_scale", nlohmann::json{{"scale", scale}}.dump());
    }
    ImGui::SameLine();
    if (widgets::Button("Reset Max")) {
        context.invoke("profiler.reset_max");
    }
    ImGui::SameLine();
    drawSimulateSystemPickerButton(context, m_modules);
}

void SystemProfilerPanel::drawSystems(EditorContext& context) const {
    const std::span<const SystemDescriptor* const> update = m_modules.systems(SystemPhase::Update);
    const std::span<const SystemDescriptor* const> fixed = m_modules.systems(SystemPhase::FixedUpdate);
    if (update.empty() && fixed.empty()) {
        ImGui::TextDisabled("The game module has no systems.");
        return;
    }
    const SystemProfile& profile = m_modules.systemProfile();
    const SimulateSystemMask& mask = m_modules.simulateMask();
    if (!ImGui::BeginTable("Game module systems", 7, tableFlags)) {
        return;
    }
    ImGui::TableSetupColumn("Sim", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("System", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn("Phase", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Runs in", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Avg", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();
    for (const std::span<const SystemDescriptor* const> phase : {update, fixed}) {
        for (const SystemDescriptor* system : phase) {
            ImGui::TableNextRow();
            ImGui::PushID(system->name.c_str());
            ImGui::TableNextColumn();
            // Simulate considers only systems that run in Play.
            if (runsIn(system->runIn, SimulationMode::Simulate)) {
                bool enabled = mask.enabled(system->name, system->phase);
                if (ImGui::Checkbox("##simulate", &enabled)) {
                    context.invoke("play.set_simulate_system",
                                   nlohmann::json{{"system", system->name}, {"enabled", enabled}}.dump());
                }
                ImGui::SetItemTooltip("Run %s in Simulate", system->name.c_str());
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(system->name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(system->phase == SystemPhase::Update ? "Update" : "Fixed");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(runInName(system->runIn));
            const SystemTiming* timing = profile.find(system->name);
            timingCell(timing != nullptr ? timing->frameMilliseconds : 0.0);
            timingCell(timing != nullptr ? timing->averageMilliseconds : 0.0);
            timingCell(timing != nullptr ? timing->maximumMilliseconds : 0.0);
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}
