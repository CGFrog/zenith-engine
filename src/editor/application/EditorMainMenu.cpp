// EditorSession's main menu bar (with the playback toolbar) and the global keymap it dispatches first.

#include "EditorSession.hpp"

#include "editor/panels/SimulateSystemPicker.hpp"
#include "editor/panels/ViewportPanel.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/EditorWidgets.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

void EditorSession::handleGlobalShortcuts(ApplicationContext& context) {
    // Operator polls decide which shortcuts apply in the current mode.
    if (ImGui::GetIO().WantTextInput || m_keymapEditor.capturing()) {
        m_keymapDispatcher.cancel();
        return;
    }
    // Before any panel, so a press followed from a region finishes even
    // when the mouse has left it.
    updateKeymapGesture(context);
    // Global keys act on the document whose window last had focus.
    dispatchKeymap(context, m_prefabWindows.focused(), KeymapRegion::Global);
}

void EditorSession::drawDebugOverlaysMenu(ApplicationContext& context) {
    // The active viewport's navigation and physics overlays; each item runs
    // view3d.overlay_toggle, so it is greyed out with no viewport open.
    const ViewportPanel* viewport = m_viewports.active();
    if (!ImGui::BeginMenu("Overlays")) {
        return;
    }
    for (const DebugOverlayItem& item : debugOverlayItems()) {
        ViewportOverlays overlays = viewport != nullptr ? viewport->chrome().overlays : ViewportOverlays{};
        const bool* shown = viewportOverlayFlag(overlays, item.overlay);
        const std::string properties = nlohmann::json{{"overlay", item.key}}.dump();
        operatorMenuItem(context, "view3d.overlay_toggle", item.label, properties,
                         viewport != nullptr && shown != nullptr && *shown);
    }
    ImGui::EndMenu();
}

void EditorSession::drawRegisteredMenu(ApplicationContext& context, const char* menuId, const char* label,
                                       bool enabled) {
    if (!ImGui::BeginMenu(label, enabled)) {
        return;
    }
    if (const std::optional<ResolvedMenu> menu =
            m_menus.resolve({.menu = menuId, .category = {}}, operatorContext(context, KeymapRegion::Global))) {
        EditorContext editor = editorContext(context);
        if (const std::optional<MenuChoice> chosen = drawMenuItems(editor, *menu)) {
            editor.invoke(chosen->operatorId, chosen->properties);
        }
    }
    ImGui::EndMenu();
}

void EditorSession::drawMainMenu(ApplicationContext& context) {
    handleGlobalShortcuts(context);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, toImVec4(palette::transparent));
    const bool menuBarOpen = ImGui::BeginMainMenuBar();
    ImGui::PopStyleColor();
    if (!menuBarOpen) {
        return;
    }
    m_themeChrome.paintBarWindow();

    if (ImGui::BeginMenu("File")) {
        operatorMenuItem(context, "file.new", "New");
        operatorMenuItem(context, "file.open", "Open...");
        operatorMenuItem(context, "file.save", "Save");
        operatorMenuItem(context, "file.save_as", "Save As...");
        ImGui::Separator();
        operatorMenuItem(context, "wm.open_project", "Open Project...");
        operatorMenuItem(context, "wm.project_hub", "Project Hub");
        drawBuildPackageMenu(context);
        ImGui::Separator();
        operatorMenuItem(context, "file.quit", "Quit");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        const bool editing = m_playMode.mode() == EditorMode::Edit;
        operatorMenuItem(context, "edit.undo", "Undo");
        operatorMenuItem(context, "edit.redo", "Redo");
        ImGui::Separator();
        operatorMenuItem(context, "edit.copy", "Copy");
        operatorMenuItem(context, "edit.paste", "Paste");
        operatorMenuItem(context, "object.duplicate", "Duplicate");
        operatorMenuItem(context, "object.delete", "Delete");
        ImGui::Separator();
        operatorMenuItem(context, "object.select_all", "Select All", R"({"action":"select"})");
        operatorMenuItem(context, "object.select_all", "Deselect All", R"({"action":"deselect"})");
        operatorMenuItem(context, "object.select_all", "Invert Selection", R"({"action":"invert"})");
        operatorMenuItem(context, "object.select_hierarchy", "Select Children", R"({"direction":"children"})");
        operatorMenuItem(context, "object.select_hierarchy", "Select Parent", R"({"direction":"parent"})");
        drawRegisteredMenu(context, selectGroupedMenuId, "Select Grouped", !m_selection.empty());
        ImGui::Separator();
        if (ImGui::BeginMenu("Clear", editing && !m_selection.empty())) {
            operatorMenuItem(context, "object.location_clear", "Location");
            operatorMenuItem(context, "object.rotation_clear", "Rotation");
            operatorMenuItem(context, "object.scale_clear", "Scale");
            ImGui::EndMenu();
        }
        drawRegisteredMenu(context, addMenuId, "Add", editing);
        ImGui::Separator();
        operatorMenuItem(context, "wm.search_menu", "Search Operators...");
        operatorMenuItem(context, "wm.keymap_editor", "Keymap...");
        operatorMenuItem(context, "wm.preferences", "Preferences...");
        operatorMenuItem(context, "project.settings", "Project Settings...");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Object")) {
        operatorMenuItem(context, "object.array", "Array");
        operatorMenuItem(context, "object.scatter", "Scatter");
        ImGui::Separator();
        if (ImGui::BeginMenu("Prefab")) {
            operatorMenuItem(context, "prefab.create_from_selection", "Create from Selection");
            ImGui::Separator();
            operatorMenuItem(context, "prefab.apply", "Apply");
            operatorMenuItem(context, "prefab.revert", "Revert");
            operatorMenuItem(context, "prefab.unpack", "Unpack");
            operatorMenuItem(context, "prefab.select_asset", "Select Prefab Asset");
            ImGui::Separator();
            operatorMenuItem(context, "prefab.edit", "Edit Prefab");
            operatorMenuItem(context, "prefab.exit_edit", "Close Prefab");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Navigation")) {
            operatorMenuItem(context, "navigation.bake", "Bake");
            operatorMenuItem(context, "navigation.clear", "Clear");
            ImGui::Separator();
            operatorMenuItem(context, "project.settings", "Settings...", R"({"tab":"navigation"})");
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        drawAreaMenuItems(context);
        ImGui::Separator();
        operatorMenuItem(context, "view3d.frame_selected", "Frame Selected");
        operatorMenuItem(context, "view3d.frame_all", "Frame All");
        if (ImGui::BeginMenu("Viewpoint")) {
            for (const char* axis : {"front", "back", "right", "left", "top", "bottom"}) {
                const std::string label = labelFromKey(axis);
                operatorMenuItem(context, "view3d.view_axis", label.c_str(), nlohmann::json{{"type", axis}}.dump());
            }
            ImGui::EndMenu();
        }
        drawDebugOverlaysMenu(context);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Play")) {
        operatorMenuItem(context, "play.start", "Play");
        for (const PlayTargetInfo& target : playTargets()) {
            const std::string label = "Play in " + std::string(target.label);
            const std::string properties = nlohmann::json{{"target", target.key}}.dump();
            operatorMenuItem(context, "play.start", label.c_str(), properties,
                             target.target == m_playback.lastPlayTarget());
        }
        operatorMenuItem(context, "play.simulate", "Simulate");
        ImGui::Separator();
        operatorMenuItem(context, "play.toggle_pause", m_playMode.mode() == EditorMode::Paused ? "Resume" : "Pause");
        operatorMenuItem(context, "play.step", "Step Frame");
        operatorMenuItem(context, "play.stop", "Stop");
        ImGui::Separator();
        operatorMenuItem(context, "play.apply_to_edit", "Apply Selection to Edit Scene");
        if (ImGui::BeginMenu("Simulate Systems")) {
            EditorContext editor = editorContext(context);
            drawSimulateSystemPicker(editor, m_modules.host());
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    drawModuleMenu(context);
    drawPlaybackToolbar(context);
    drawWorkspaceTabs(context);
    ImGui::EndMainMenuBar();
}

void EditorSession::drawPlaybackToolbar(ApplicationContext& context) {
    ImGui::Separator();
    const float side = ImGui::GetFrameHeight();
    const ImVec2 square{side, side};
    const EditorMode mode = m_playMode.mode();
    const auto button = [&](const char* id, EditorIcon icon, bool active, std::string_view operatorId,
                            const char* tooltip, std::string_view properties = {}) {
        ImGui::BeginDisabled(!operatorAvailable(context, operatorId));
        if (widgets::ToolButton(id, icon, active, square)) {
            invokeOperator(context, operatorId, properties, KeymapRegion::Global);
        }
        ImGui::EndDisabled();
        const std::string shortcut = m_keymap.shortcutLabel(operatorId, properties);
        ImGui::SetItemTooltip("%s%s%s", tooltip, shortcut.empty() ? "" : "  ", shortcut.c_str());
    };
    const auto dropdown = [&](const char* id, const char* popup) {
        ImGui::SameLine(0.0f, 0.0f);
        if (widgets::Button(id, {side * 0.6f, side})) {
            ImGui::OpenPopup(popup);
        }
    };

    const bool simulating = mode == EditorMode::Simulate || (mode == EditorMode::Paused && m_playMode.simulating());
    // One button starts and stops: while anything runs it is Stop.
    const bool running = mode != EditorMode::Edit || m_playback.standaloneRunning();
    if (running) {
        button("##toolbar_play", EditorIcon::Stop, true, "play.stop", "Stop");
    } else {
        const std::string playTooltip = "Play in " + std::string(playTargetInfo(m_playback.lastPlayTarget()).label);
        button("##toolbar_play", EditorIcon::Play, false, "play.start", playTooltip.c_str());
    }
    ImGui::BeginDisabled(running);
    dropdown("v##toolbar_play_targets", "##PlayTargetMenu");
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("##PlayTargetMenu")) {
        for (const PlayTargetInfo& target : playTargets()) {
            operatorMenuItem(context, "play.start", std::string(target.label).c_str(),
                             nlohmann::json{{"target", target.key}}.dump(),
                             target.target == m_playback.lastPlayTarget());
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    button("##toolbar_simulate", EditorIcon::Physics, simulating, "play.simulate", "Simulate");
    dropdown("v##toolbar_simulate_options", "##SimulateOptions");
    if (ImGui::BeginPopup("##SimulateOptions")) {
        drawSimulateOptions(context);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    button("##toolbar_pause", EditorIcon::Pause, mode == EditorMode::Paused, "play.toggle_pause",
           mode == EditorMode::Paused ? "Resume" : "Pause");
    ImGui::SameLine();
    ImGui::BeginDisabled(!operatorAvailable(context, "play.step"));
    if (widgets::Button("Step##toolbar_step", {0.0f, side})) {
        invokeOperator(context, "play.step", {}, KeymapRegion::Global);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Step one frame while paused  %s", m_keymap.shortcutLabel("play.step", {}).c_str());
    ImGui::SameLine();
    float scale = m_playMode.timeScale();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.0f);
    if (ImGui::DragFloat("##toolbar_time_scale", &scale, 0.01f, SimulationClock::minimumTimeScale,
                         SimulationClock::maximumTimeScale, "x%.2f")) {
        invokeOperator(context, "play.set_time_scale", nlohmann::json{{"scale", scale}}.dump(), KeymapRegion::Global);
    }
    ImGui::SetItemTooltip("Time scale for Play and Simulate");
}

void EditorSession::drawSimulateOptions(ApplicationContext& context) {
    operatorMenuItem(context, "play.simulate", "Simulate");
    ImGui::Separator();
    EditorContext editor = editorContext(context);
    drawSimulateSystemPicker(editor, m_modules.host());
}
