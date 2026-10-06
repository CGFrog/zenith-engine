#pragma once

// The keymap editor window: lists every operator with its shortcuts per region.

#include "editor/operators/Keymap.hpp"
#include "editor/operators/OperatorRegistry.hpp"

#include <cstddef>
#include <optional>
#include <string>

// The keymap editor window: lists every operator with its shortcuts per
// region. Clicking a shortcut captures the next key chord pressed (Escape
// cancels), or for a viewport shortcut also a mouse button with modifiers;
// each shortcut's trigger (Press, Twice, Hold, Release, Click, Drag) is
// chosen beside it. Conflicts are refused with a warning. Operators can gain
// extra shortcuts, lose them, or be reset to their defaults.
class KeymapEditor {
  public:
    // Draws the window while open is true. Returns whether the keymap
    // changed, so the caller can save it.
    bool draw(bool& open, Keymap& keymap, const OperatorRegistry& registry);

    // While capturing, every key press belongs to the editor, so the caller
    // must not dispatch shortcuts.
    bool capturing() const;
    // Starts capturing a new chord for keymap.bindings()[index].
    void captureRebind(size_t index);
    // Starts capturing a chord for a new binding of operatorId in region.
    void captureNewBinding(std::string operatorId, KeymapRegion region);
    // The last refused rebind or other feedback; empty when none.
    const std::string& message() const;

  private:
    struct Capture {
        std::optional<size_t> bindingIndex;
        std::string operatorId;
        KeymapRegion region{KeymapRegion::Global};
        // The ImGui frame capturing started in, whose click is not captured.
        int startFrame{0};
    };

    // The key or (for viewport bindings) mouse button pressed this frame.
    std::optional<ImGuiKey> capturedKey(const Keymap& keymap) const;
    bool updateCapture(Keymap& keymap);
    bool drawOperator(Keymap& keymap, const std::string& operatorId, const std::string& label,
                      const std::string& description);

    std::optional<Capture> m_capture;
    std::string m_message;
    std::string m_filter;
    KeymapRegion m_newBindingRegion{KeymapRegion::Global};
};
