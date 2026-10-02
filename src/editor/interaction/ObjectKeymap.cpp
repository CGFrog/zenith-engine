#include "ObjectKeymap.hpp"

#include <array>

namespace {
constexpr uint8_t viewport = static_cast<uint8_t>(KeymapRegion::Viewport);
constexpr uint8_t outliner = static_cast<uint8_t>(KeymapRegion::Outliner);
constexpr uint8_t both = viewport | outliner;

// Order matters only for labels: the first binding of a command names it.
constexpr std::array bindings{
    ObjectKeyBinding{ImGuiKey_G, ObjectCommand::Grab, viewport, "G"},
    ObjectKeyBinding{ImGuiKey_R, ObjectCommand::Rotate, viewport, "R"},
    ObjectKeyBinding{ImGuiKey_S, ObjectCommand::Scale, viewport, "S"},
    ObjectKeyBinding{ImGuiMod_Alt | ImGuiKey_G, ObjectCommand::ClearLocation, both, "Alt G"},
    ObjectKeyBinding{ImGuiMod_Alt | ImGuiKey_R, ObjectCommand::ClearRotation, both, "Alt R"},
    ObjectKeyBinding{ImGuiMod_Alt | ImGuiKey_S, ObjectCommand::ClearScale, both, "Alt S"},
    ObjectKeyBinding{ImGuiMod_Shift | ImGuiKey_D, ObjectCommand::Duplicate, both, "Shift D"},
    ObjectKeyBinding{ImGuiKey_X, ObjectCommand::DeleteWithConfirmation, both, "X"},
    ObjectKeyBinding{ImGuiKey_Delete, ObjectCommand::Delete, both, "Delete"},
    ObjectKeyBinding{ImGuiKey_A, ObjectCommand::SelectAll, both, "A"},
    ObjectKeyBinding{ImGuiMod_Alt | ImGuiKey_A, ObjectCommand::DeselectAll, both, "Alt A"},
    ObjectKeyBinding{ImGuiMod_Ctrl | ImGuiKey_I, ObjectCommand::InvertSelection, both, "Ctrl I"},
    ObjectKeyBinding{ImGuiMod_Shift | ImGuiKey_A, ObjectCommand::AddMenu, viewport, "Shift A"},
    ObjectKeyBinding{ImGuiKey_F2, ObjectCommand::Rename, both, "F2"},
    ObjectKeyBinding{ImGuiMod_Ctrl | ImGuiKey_C, ObjectCommand::Copy, both, "Ctrl C"},
    ObjectKeyBinding{ImGuiMod_Ctrl | ImGuiKey_V, ObjectCommand::Paste, both, "Ctrl V"},
    ObjectKeyBinding{ImGuiMod_Shift | ImGuiKey_Tab, ObjectCommand::ToggleSnapping, viewport, "Shift Tab"},
    ObjectKeyBinding{ImGuiKey_KeypadDecimal, ObjectCommand::FrameSelected, both, "Numpad ."},
    ObjectKeyBinding{ImGuiKey_Home, ObjectCommand::FrameAll, viewport, "Home"},
    ObjectKeyBinding{ImGuiKey_Keypad1, ObjectCommand::ViewFront, viewport, "Numpad 1"},
    ObjectKeyBinding{ImGuiMod_Ctrl | ImGuiKey_Keypad1, ObjectCommand::ViewBack, viewport, "Ctrl Numpad 1"},
    ObjectKeyBinding{ImGuiKey_Keypad3, ObjectCommand::ViewRight, viewport, "Numpad 3"},
    ObjectKeyBinding{ImGuiMod_Ctrl | ImGuiKey_Keypad3, ObjectCommand::ViewLeft, viewport, "Ctrl Numpad 3"},
    ObjectKeyBinding{ImGuiKey_Keypad7, ObjectCommand::ViewTop, viewport, "Numpad 7"},
    ObjectKeyBinding{ImGuiMod_Ctrl | ImGuiKey_Keypad7, ObjectCommand::ViewBottom, viewport, "Ctrl Numpad 7"},
    ObjectKeyBinding{ImGuiKey_Keypad9, ObjectCommand::ViewOpposite, viewport, "Numpad 9"},
    ObjectKeyBinding{ImGuiKey_Keypad4, ObjectCommand::OrbitLeft, viewport, "Numpad 4"},
    ObjectKeyBinding{ImGuiKey_Keypad6, ObjectCommand::OrbitRight, viewport, "Numpad 6"},
    ObjectKeyBinding{ImGuiKey_Keypad8, ObjectCommand::OrbitUp, viewport, "Numpad 8"},
    ObjectKeyBinding{ImGuiKey_Keypad2, ObjectCommand::OrbitDown, viewport, "Numpad 2"},
};
} // namespace

std::span<const ObjectKeyBinding> objectKeymap() {
    return bindings;
}

std::optional<ObjectCommand> findObjectCommand(ImGuiKeyChord chord, KeymapRegion region) {
    for (const ObjectKeyBinding& binding : bindings) {
        if (binding.chord == chord && (binding.regions & static_cast<uint8_t>(region)) != 0) {
            return binding.command;
        }
    }
    return std::nullopt;
}

const char* objectCommandLabel(ObjectCommand command) {
    for (const ObjectKeyBinding& binding : bindings) {
        if (binding.command == command) {
            return binding.label;
        }
    }
    return "";
}
