#include "DefaultKeymap.hpp"

#include <initializer_list>
#include <string>
#include <utility>

namespace {
KeymapBinding binding(std::string operatorId, KeymapRegion region, ImGuiKeyChord chord, std::string properties = {},
                      KeyTrigger trigger = KeyTrigger::Press) {
    return {.operatorId = std::move(operatorId),
            .region = region,
            .chord = chord,
            .trigger = trigger,
            .properties = std::move(properties)};
}

// Object keys work in both the viewport and the outliner.
void addObjectKey(std::vector<KeymapBinding>& bindings, const char* operatorId, ImGuiKeyChord chord,
                  const char* properties = "") {
    for (const KeymapRegion region : {KeymapRegion::Viewport, KeymapRegion::Outliner}) {
        bindings.push_back(binding(operatorId, region, chord, properties));
    }
}
} // namespace

std::vector<KeymapBinding> defaultEditorKeymap() {
    constexpr KeymapRegion global = KeymapRegion::Global;
    constexpr KeymapRegion viewport = KeymapRegion::Viewport;
    constexpr KeymapRegion outliner = KeymapRegion::Outliner;
    std::vector<KeymapBinding> bindings{
        binding("file.new", global, ImGuiMod_Ctrl | ImGuiKey_N),
        binding("file.open", global, ImGuiMod_Ctrl | ImGuiKey_O),
        binding("file.save", global, ImGuiMod_Ctrl | ImGuiKey_S),
        binding("file.save_as", global, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S),
        binding("file.quit", global, ImGuiMod_Ctrl | ImGuiKey_Q),
        binding("edit.undo", global, ImGuiMod_Ctrl | ImGuiKey_Z),
        binding("edit.redo", global, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z),
        // Blender redoes with Ctrl Shift Z; Ctrl Y is kept as the Windows alias.
        binding("edit.redo", global, ImGuiMod_Ctrl | ImGuiKey_Y),
        binding("wm.search_menu", global, ImGuiKey_F3),
        binding("build.run", global, ImGuiMod_Ctrl | ImGuiKey_B),
        binding("play.start", global, ImGuiKey_F5),
        binding("play.simulate", global, ImGuiMod_Alt | ImGuiKey_F5),
        binding("play.toggle_pause", global, ImGuiKey_F6),
        binding("play.step", global, ImGuiKey_F7),
        binding("play.stop", global, ImGuiMod_Shift | ImGuiKey_F5),

        binding("transform.translate", viewport, ImGuiKey_G),
        binding("transform.rotate", viewport, ImGuiKey_R),
        binding("transform.resize", viewport, ImGuiKey_S),
        // In the viewport the copy follows the mouse; the outliner copies in place.
        binding("object.duplicate_move", viewport, ImGuiMod_Shift | ImGuiKey_D),
        binding("object.duplicate", outliner, ImGuiMod_Shift | ImGuiKey_D),
        binding("object.add_menu", viewport, ImGuiMod_Shift | ImGuiKey_A),
        binding("view3d.toggle_snapping", viewport, ImGuiMod_Shift | ImGuiKey_Tab),
        binding("view3d.toggle_sidebar", viewport, ImGuiKey_N),
        binding("view3d.toggle_toolbar", viewport, ImGuiKey_T),
        binding("view3d.frame_all", viewport, ImGuiKey_Home),
        binding("view3d.view_axis", viewport, ImGuiKey_Keypad1, R"({"type":"front"})"),
        binding("view3d.view_axis", viewport, ImGuiMod_Ctrl | ImGuiKey_Keypad1, R"({"type":"back"})"),
        binding("view3d.view_axis", viewport, ImGuiKey_Keypad3, R"({"type":"right"})"),
        binding("view3d.view_axis", viewport, ImGuiMod_Ctrl | ImGuiKey_Keypad3, R"({"type":"left"})"),
        binding("view3d.view_axis", viewport, ImGuiKey_Keypad7, R"({"type":"top"})"),
        binding("view3d.view_axis", viewport, ImGuiMod_Ctrl | ImGuiKey_Keypad7, R"({"type":"bottom"})"),
        binding("view3d.view_opposite", viewport, ImGuiKey_Keypad9),
        binding("view3d.view_camera", viewport, ImGuiKey_Keypad0),
        binding("view3d.view_orbit", viewport, ImGuiKey_Keypad4, R"({"direction":"left"})"),
        binding("view3d.view_orbit", viewport, ImGuiKey_Keypad6, R"({"direction":"right"})"),
        binding("view3d.view_orbit", viewport, ImGuiKey_Keypad8, R"({"direction":"up"})"),
        binding("view3d.view_orbit", viewport, ImGuiKey_Keypad2, R"({"direction":"down"})"),
        binding("view3d.select_box", viewport, ImGuiKey_B),
        binding("view3d.localview", viewport, ImGuiKey_KeypadDivide),
        // Pie menus; with pies off in Preferences they open as searchable
        // menus. Numpad keys stay the direct views.
        binding("wm.call_pie", viewport, ImGuiKey_Z, R"({"menu":"VIEW3D_MT_shading_pie"})"),
        binding("wm.call_pie", viewport, ImGuiKey_GraveAccent, R"({"menu":"VIEW3D_MT_view_pie"})"),
        binding("wm.call_pie", viewport, ImGuiKey_Period, R"({"menu":"VIEW3D_MT_pivot_pie"})"),

        // Mouse gestures: a click selects at the mouse, Alt cycles through
        // the objects under it, a Ctrl+RMB drag lassos (Shift extends, Alt
        // subtracts), and Shift+RMB places the 3D cursor. Unbound presses
        // (RMB, MMB) navigate the camera.
        binding("view3d.select", viewport, ImGuiKey_MouseLeft),
        binding("view3d.select", viewport, ImGuiMod_Shift | ImGuiKey_MouseLeft, R"({"extend":true})"),
        binding("view3d.select", viewport, ImGuiMod_Ctrl | ImGuiKey_MouseLeft),
        binding("view3d.select", viewport, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_MouseLeft, R"({"extend":true})"),
        binding("view3d.select_cycle", viewport, ImGuiMod_Alt | ImGuiKey_MouseLeft, {}, KeyTrigger::Click),
        binding("view3d.select_cycle", viewport, ImGuiMod_Shift | ImGuiMod_Alt | ImGuiKey_MouseLeft,
                R"({"extend":true})", KeyTrigger::Click),
        binding("view3d.select_lasso", viewport, ImGuiMod_Ctrl | ImGuiKey_MouseRight,
                R"({"mode":"set","right_drag":true})", KeyTrigger::Drag),
        binding("view3d.select_lasso", viewport, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_MouseRight,
                R"({"mode":"extend","right_drag":true})", KeyTrigger::Drag),
        binding("view3d.select_lasso", viewport, ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_MouseRight,
                R"({"mode":"subtract","right_drag":true})", KeyTrigger::Drag),
        binding("view3d.cursor3d_place", viewport, ImGuiMod_Shift | ImGuiKey_MouseRight, {}, KeyTrigger::Click),
    };
    addObjectKey(bindings, "object.location_clear", ImGuiMod_Alt | ImGuiKey_G);
    addObjectKey(bindings, "object.rotation_clear", ImGuiMod_Alt | ImGuiKey_R);
    addObjectKey(bindings, "object.scale_clear", ImGuiMod_Alt | ImGuiKey_S);
    addObjectKey(bindings, "object.delete_menu", ImGuiKey_X);
    addObjectKey(bindings, "object.delete", ImGuiKey_Delete);
    addObjectKey(bindings, "object.select_all", ImGuiKey_A, R"({"action":"select"})");
    addObjectKey(bindings, "object.select_all", ImGuiMod_Alt | ImGuiKey_A, R"({"action":"deselect"})");
    addObjectKey(bindings, "object.select_all", ImGuiMod_Ctrl | ImGuiKey_I, R"({"action":"invert"})");
    addObjectKey(bindings, "object.select_hierarchy", ImGuiKey_RightBracket, R"({"direction":"children"})");
    addObjectKey(bindings, "object.select_hierarchy", ImGuiMod_Shift | ImGuiKey_RightBracket,
                 R"({"direction":"children","extend":true})");
    addObjectKey(bindings, "object.select_hierarchy", ImGuiKey_LeftBracket, R"({"direction":"parent"})");
    addObjectKey(bindings, "object.select_hierarchy", ImGuiMod_Shift | ImGuiKey_LeftBracket,
                 R"({"direction":"parent","extend":true})");
    addObjectKey(bindings, "wm.call_menu", ImGuiMod_Shift | ImGuiKey_G, R"({"menu":"VIEW3D_MT_select_grouped"})");
    addObjectKey(bindings, "object.rename_popup", ImGuiKey_F2);
    addObjectKey(bindings, "edit.copy", ImGuiMod_Ctrl | ImGuiKey_C);
    addObjectKey(bindings, "edit.paste", ImGuiMod_Ctrl | ImGuiKey_V);
    addObjectKey(bindings, "view3d.frame_selected", ImGuiKey_KeypadDecimal);
    addObjectKey(bindings, "object.hide", ImGuiKey_H);
    addObjectKey(bindings, "object.unhide", ImGuiMod_Alt | ImGuiKey_H);
    addObjectKey(bindings, "object.isolate", ImGuiMod_Shift | ImGuiKey_H);
    // Tapping A twice quickly deselects everything.
    for (const KeymapRegion region : {viewport, outliner}) {
        KeymapBinding deselect = binding("object.select_all", region, ImGuiKey_A, R"({"action":"deselect"})");
        deselect.trigger = KeyTrigger::DoubleTap;
        bindings.push_back(std::move(deselect));
    }
    return bindings;
}
