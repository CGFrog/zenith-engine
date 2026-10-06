#pragma once

// Key bindings per region and when they fire (press, double tap, hold, release, click, drag).

#include "editor/operators/KeymapRegion.hpp"

#include <imgui.h>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// When a binding runs relative to its chord's press. Every trigger works for
// keys and mouse buttons. A chord may carry one binding per trigger and each
// runs on its own (a press runs the Press binding even when the chord also
// has a Hold or Drag one), except that Click is skipped once Drag or Hold ran.
enum class KeyTrigger : uint8_t {
    Press,
    // A second press of the same chord within the double-click time. The
    // first press still runs the chord's Press binding.
    DoubleTap,
    // The chord stays down for keyHoldSeconds.
    Hold,
    // The chord's key or button is let go.
    Release,
    // Let go without the mouse moving past the drag threshold, and before a
    // Hold binding ran.
    Click,
    // The mouse moves past the drag threshold while the chord is down.
    Drag,
};

// Every trigger, in declaration order.
inline constexpr KeyTrigger allKeyTriggers[] = {KeyTrigger::Press,   KeyTrigger::DoubleTap, KeyTrigger::Hold,
                                                KeyTrigger::Release, KeyTrigger::Click,     KeyTrigger::Drag};

// How long a chord stays down before its Hold binding runs.
inline constexpr double keyHoldSeconds = 0.4;

// Binds a key or mouse chord in one region to an operator id, with optional
// property values as a compact JSON object (for example
// {"action":"deselect"}). Mouse chords (LMB, MMB, RMB, X1, X2, with
// modifiers) are valid only in the Viewport region, whose panel dispatches
// them.
struct KeymapBinding {
    std::string operatorId;
    KeymapRegion region{KeymapRegion::Global};
    ImGuiKeyChord chord{ImGuiKey_None};
    KeyTrigger trigger{KeyTrigger::Press};
    std::string properties;

    bool operator==(const KeymapBinding&) const = default;
};

struct KeymapResult {
    bool success{false};
    std::string error;
    // Problems that did not stop the operation, such as a saved binding that
    // was skipped because it conflicts.
    std::vector<std::string> warnings;

    explicit operator bool() const {
        return success;
    }
};

// "Ctrl+Shift+Z", "Numpad 1", "Shift+Tab", "Ctrl+RMB". Empty for
// ImGuiKey_None.
std::string formatKeyChord(ImGuiKeyChord chord);
// Accepts formatKeyChord's output; nullopt for anything else.
std::optional<ImGuiKeyChord> parseKeyChord(std::string_view text);
// Whether chord's key is a mouse button (ImGuiKey_MouseLeft to MouseX2).
bool isMouseChord(ImGuiKeyChord chord);
// The ImGuiKey of a mouse button (ImGuiMouseButton_Left and so on);
// ImGuiKey_None for anything else.
ImGuiKey mouseButtonKey(int button);
// "press", "double_tap", "hold", "release", "click", "drag", as keymap.json
// stores them.
const char* keyTriggerName(KeyTrigger trigger);
std::optional<KeyTrigger> parseKeyTrigger(std::string_view name);
const char* keymapRegionName(KeymapRegion region);
std::optional<KeymapRegion> parseKeymapRegion(std::string_view name);

// Key bindings per region, starting from code-defined defaults. Users rebind
// them in the keymap editor; only the differences from the defaults are saved,
// so new default bindings reach users who changed other keys. Two bindings
// conflict when they share chord and trigger and their regions overlap
// (the same region, or either is Global, since Global keys work everywhere).
class Keymap {
  public:
    // Normalizes each default; a conflicting or invalid default is dropped
    // and listed in defaultProblems().
    explicit Keymap(std::vector<KeymapBinding> defaults);

    const std::vector<KeymapBinding>& bindings() const;
    const std::vector<KeymapBinding>& defaults() const;
    const std::vector<std::string>& defaultProblems() const;

    // Adds a binding; rejects conflicts and invalid bindings.
    KeymapResult add(KeymapBinding binding);
    // Changes the chord of bindings()[index]; rejects conflicts.
    KeymapResult rebind(size_t index, ImGuiKeyChord chord);
    // Changes the trigger of bindings()[index]; rejects conflicts.
    KeymapResult setTrigger(size_t index, KeyTrigger trigger);
    void remove(size_t index);
    // Restores the default bindings of one operator, keeping other changes.
    // Defaults that would now conflict are skipped and returned as warnings.
    KeymapResult resetOperator(std::string_view operatorId);
    void resetToDefaults();

    // Index of a binding that conflicts with binding, ignoring bindings()[ignore].
    std::optional<size_t> findConflict(const KeymapBinding& binding, std::optional<size_t> ignore = std::nullopt) const;

    // Label for a menu item that runs operatorId with properties: the chord
    // of its first matching Press binding, preferring Global, then Viewport,
    // then Outliner. Empty when the operator is unbound.
    std::string shortcutLabel(std::string_view operatorId, std::string_view properties = {}) const;
    // Distinct keyboard chords bound in region, for polling which were
    // pressed.
    std::vector<ImGuiKeyChord> chords(KeymapRegion region) const;
    // Distinct mouse chords bound in region.
    std::vector<ImGuiKeyChord> mouseChords(KeymapRegion region) const;
    // The binding of chord with trigger in region, or null.
    const KeymapBinding* find(KeymapRegion region, ImGuiKeyChord chord, KeyTrigger trigger) const;
    // Whether chord has a binding of any trigger in region, so a press of it
    // belongs to the keymap (a bound mouse chord is not camera navigation).
    bool bindsChord(KeymapRegion region, ImGuiKeyChord chord) const;
    // Whether chord has a Hold, Release, Click, or Drag binding in region,
    // which needs its press followed until it ends.
    bool tracksChord(KeymapRegion region, ImGuiKeyChord chord) const;
    // The Press or DoubleTap binding a press of chord in region runs at
    // timeSeconds, tracking taps so a quick second press can run a DoubleTap
    // binding instead; null when the chord has neither.
    const KeymapBinding* resolve(KeymapRegion region, ImGuiKeyChord chord, double timeSeconds, double doubleTapSeconds);

    // keymap.json: {"version":1,"removed":[...],"added":[...]} relative to
    // the defaults.
    std::string serializeOverrides() const;
    // Replaces the user's changes with those in json. On an error the keymap
    // keeps its defaults; skipped entries are returned as warnings.
    KeymapResult loadOverrides(std::string_view json);
    // A missing file is not an error: the defaults stay in place.
    KeymapResult load(const std::filesystem::path& path);
    // Writes the overrides; an unchanged keymap removes the file instead.
    KeymapResult save(const std::filesystem::path& path) const;

  private:
    std::vector<KeymapBinding> m_defaults;
    std::vector<KeymapBinding> m_bindings;
    std::vector<std::string> m_defaultProblems;

    struct Tap {
        KeymapRegion region{KeymapRegion::Global};
        ImGuiKeyChord chord{ImGuiKey_None};
        double time{0.0};
    };

    std::optional<Tap> m_lastTap;
};
