// EditorSession's game module workflows: running the module's systems each
// frame, the Module menu, and the New Code dialog. Loading and reloading the
// module are GameModuleController's.

#include "EditorSession.hpp"

#include "editor/application/EditorSessionOperators.hpp"
#include "editor/panels/GameModulePanel.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <utility>

OperatorRegistrationResult registerModuleOperatorFamily(OperatorRegistry& registry,
                                                        const EditorOperatorServices& services) {
    const GameModuleHost& modules = services.modules;
    EditorObjectModeCommands& objectMode = services.objectModeCommands;
    EditorWindowCommands& windows = services.windowCommands;
    return registry.add(applicationOperator(
        "module.reload", "Reload Game Module",
        "Rebuild-friendly hot reload: unload the game module and load the library again, keeping the scene",
        [&modules, &objectMode](const OperatorContext& context) {
            return !modules.sourcePath().empty() && context.editMode() && !objectMode.modalTransformRunning();
        },
        // Deferred to the next update, so a module panel or operator that
        // requests it is not unloaded while it runs.
        [&windows](ApplicationContext&) { windows.requestModuleReload(); }));
}

void EditorSession::requestModuleReload() {
    m_modules.requestReload();
}

void EditorSession::openNewCodeDialog(GeneratedCodeKind kind) {
    m_newCodeDialog.open(kind);
}

void EditorSession::drawNewCodeDialog(ApplicationContext& context) {
    const std::optional<GenerateCodeProperties> create = m_newCodeDialog.draw();
    const Operator* generate = create ? m_operators.find("module.generate_code") : nullptr;
    if (generate == nullptr) {
        return;
    }
    OperatorProperties properties = generate->properties;
    if (GenerateCodeProperties* values = properties.get<GenerateCodeProperties>()) {
        *values = *create;
    }
    m_newCodeDialog.finish(invokeOperator(context, "module.generate_code", properties));
}

void EditorSession::updateGameModule(ApplicationContext& context, const InputState& input, float deltaSeconds) {
    m_modules.pollReload(context);
    if (m_playMode.mode() == EditorMode::Edit) {
        m_modules.host().runEditFrame(context.scene, deltaSeconds, context.physics.config().fixedTimeStep);
        return;
    }
    m_playback.update(context, input, deltaSeconds);
}

void EditorSession::drawModuleMenu(ApplicationContext& context) {
    if (!ImGui::BeginMenu("Module")) {
        return;
    }
    ImGui::TextDisabled("%s", m_modules.statusText().c_str());
    if (m_modules.host().status() == GameModuleStatus::Failed) {
        ImGui::SetItemTooltip("%s", m_modules.host().error().c_str());
    }
    operatorMenuItem(context, "build.run", "Build");
    operatorMenuItem(context, "module.reload", "Reload Game Module");
    ImGui::Separator();
    operatorMenuItem(context, "module.new_behaviour", "New Behaviour...");
    operatorMenuItem(context, "module.new_component", "New Component...");
    operatorMenuItem(context, "module.new_system", "New System...");
    ImGui::Separator();
    operatorMenuItem(context, "debug.attach", "Attach Debugger");
    operatorMenuItem(context, "debug.clear_watches", "Clear Watched Fields");
    // Module panels are area types (see syncAreaTypes), toggled like any
    // other in the active workspace.
    std::span<GameModuleHost::Panel> panels = m_modules.host().panels();
    if (!panels.empty()) {
        ImGui::Separator();
        for (const GameModuleHost::Panel& panel : panels) {
            const std::string typeId = gameModuleAreaTypeId(panel.panel.title);
            operatorMenuItem(context, "wm.toggle_panel", panel.panel.title.c_str(),
                             nlohmann::json{{"panel", typeId}}.dump(), m_workspaces.active().anyVisible(typeId));
        }
    }
    // The module's own menus open as popups, like any wm.call_menu binding.
    std::span<const EditorModuleMenu> menus = m_modules.host().menus();
    if (!menus.empty()) {
        ImGui::Separator();
        for (const EditorModuleMenu& menu : menus) {
            const std::string label = (menu.title.empty() ? menu.id : menu.title) + "...";
            operatorMenuItem(context, "wm.call_menu", label.c_str(), nlohmann::json{{"menu", menu.id}}.dump());
        }
    }
    ImGui::EndMenu();
}
