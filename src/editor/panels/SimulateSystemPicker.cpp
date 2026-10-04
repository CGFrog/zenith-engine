#include "SimulateSystemPicker.hpp"

#include "editor/modules/GameModuleHost.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/theme/EditorWidgets.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

std::vector<SimulateSystemChoice> simulateSystemChoices(const GameModuleHost& modules) {
    std::vector<SimulateSystemChoice> choices;
    const SimulateSystemMask& mask = modules.simulateMask();
    for (const SystemPhase phase : systemPhases) {
        for (const SystemDescriptor* system : modules.systems(phase)) {
            // Simulate considers only systems that run in Play.
            if (runsIn(system->runIn, SimulationMode::Simulate)) {
                choices.push_back({.name = system->name, .phase = phase, .enabled = mask.enabled(system->name, phase)});
            }
        }
    }
    return choices;
}

std::string simulateSystemLabel(const SimulateSystemChoice& choice) {
    const char* suffix = "";
    switch (choice.phase) {
    case SystemPhase::Update:
        break;
    case SystemPhase::FixedUpdate:
        suffix = " (fixed)";
        break;
    case SystemPhase::LateUpdate:
        suffix = " (late)";
        break;
    case SystemPhase::Ui:
        suffix = " (UI)";
        break;
    }
    return choice.name + suffix + "##simulate";
}

void drawSimulateSystemPicker(EditorContext& context, const GameModuleHost& modules) {
    const std::vector<SimulateSystemChoice> choices = simulateSystemChoices(modules);
    if (choices.empty()) {
        ImGui::TextDisabled("The game module has no systems Simulate can run.");
        return;
    }
    ImGui::TextDisabled("Physics always runs; Simulate also runs:");
    for (const SimulateSystemChoice& choice : choices) {
        // The Simulate mask names systems, which are unique across phases.
        ImGui::PushID(choice.name.c_str());
        bool enabled = choice.enabled;
        const std::string label = simulateSystemLabel(choice);
        if (ImGui::Checkbox(label.c_str(), &enabled)) {
            context.invoke("play.set_simulate_system",
                           nlohmann::json{{"system", choice.name}, {"enabled", enabled}}.dump());
        }
        ImGui::SetItemTooltip("Run %s in Simulate", choice.name.c_str());
        ImGui::PopID();
    }
}

void drawSimulateSystemPickerButton(EditorContext& context, const GameModuleHost& modules, const char* label) {
    ImGui::PushID("##SimulateSystemPicker");
    if (widgets::Button(label)) {
        ImGui::OpenPopup("##SimulateSystems");
    }
    if (ImGui::BeginPopup("##SimulateSystems")) {
        drawSimulateSystemPicker(context, modules);
        ImGui::EndPopup();
    }
    ImGui::PopID();
}
