#pragma once

#include <imgui.h>

#include <cstdint>
#include <optional>
#include <span>

// Blender's object-mode keymap for the editor features that exist. The editor
// session decides what each command does; this table only names the chords.

enum class ObjectCommand : uint8_t {
    Grab,
    Rotate,
    Scale,
    ClearLocation,
    ClearRotation,
    ClearScale,
    Duplicate,
    DeleteWithConfirmation,
    Delete,
    SelectAll,
    DeselectAll,
    InvertSelection,
    AddMenu,
    Rename,
    Copy,
    Paste,
    ToggleSnapping,
    FrameSelected,
    FrameAll,
    ViewFront,
    ViewBack,
    ViewRight,
    ViewLeft,
    ViewTop,
    ViewBottom,
    ViewOpposite,
    OrbitLeft,
    OrbitRight,
    OrbitUp,
    OrbitDown,
};

// Editors under the mouse that receive object commands, as in Blender where
// keys go to the region the cursor is over.
enum class KeymapRegion : uint8_t {
    Viewport = 1u << 0u,
    Outliner = 1u << 1u,
};

struct ObjectKeyBinding {
    ImGuiKeyChord chord{ImGuiKey_None};
    ObjectCommand command{};
    uint8_t regions{0};
    // Shown beside menu items, for example "Shift D".
    const char* label{""};
};

std::span<const ObjectKeyBinding> objectKeymap();
std::optional<ObjectCommand> findObjectCommand(ImGuiKeyChord chord, KeymapRegion region);
const char* objectCommandLabel(ObjectCommand command);
