#pragma once

#include "editor/operators/Keymap.hpp"

#include <imgui.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// A binding to run now. It is a copy, so running it may change the keymap.
struct KeymapInvocation {
    std::string operatorId;
    std::string properties;
    KeymapRegion region{KeymapRegion::Global};

    bool operator==(const KeymapInvocation&) const = default;
};

// One frame of input for the chord a gesture follows.
struct KeymapGestureInput {
    // Whether the chord's key or mouse button is still down (modifiers may
    // have been let go).
    bool down{false};
    double timeSeconds{0.0};
    // Screen pixels.
    ImVec2 mouse{0.0f, 0.0f};
};

// Follows one press of a chord until its key or button is let go, running the
// chord's Hold, Drag, Release, and Click bindings as KeyTrigger describes.
// Bindings are looked up when they fire, so a keymap edit mid-gesture takes
// effect at once.
class KeymapGesture {
  public:
    KeymapGesture(KeymapRegion region, ImGuiKeyChord chord, double pressSeconds, ImVec2 pressMouse);

    // Advances by one frame and returns the bindings to run, in order. After
    // the frame the key is let go, the gesture is finished and returns
    // nothing more. dragThreshold is in screen pixels.
    std::vector<KeymapInvocation> advance(const Keymap& keymap, const KeymapGestureInput& input, float dragThreshold);
    bool finished() const;
    KeymapRegion region() const;
    ImGuiKeyChord chord() const;

  private:
    // Appends the binding of trigger, if the chord has one; returns whether
    // it did.
    bool fire(const Keymap& keymap, KeyTrigger trigger, std::vector<KeymapInvocation>& fired) const;

    KeymapRegion m_region;
    ImGuiKeyChord m_chord;
    double m_pressSeconds;
    ImVec2 m_pressMouse;
    bool m_moved{false};
    bool m_holdChecked{false};
    bool m_holdRan{false};
    bool m_finished{false};
};

// The buttons a mouse press claimed and the bindings it runs.
struct KeymapMousePress {
    // A bit (1 << ImGuiMouseButton) per pressed button bound in the region,
    // whether or not it has a Press binding: the press belongs to the keymap
    // (it is not camera navigation).
    uint32_t claimed{0};
    std::vector<KeymapInvocation> invocations;
};

// Every mouse button bit, for KeymapDispatcher::pressMouse.
inline constexpr uint32_t allMouseButtons = (1u << ImGuiMouseButton_COUNT) - 1u;

// Who a frame's keyboard input belongs to: the editor, or the game playing in
// a game view that has input focus.
enum class KeyboardOwner : uint8_t {
    Editor,
    Game,
};

// Whether the game could use the chord: any key without Ctrl, Alt, or Super
// except function keys and Escape. While the game owns the keyboard these
// chords are never editor shortcuts (Space opens operator search in the
// editor but jumps in the game).
bool chordReachesGame(ImGuiKeyChord chord);

// Turns this frame's ImGui input into keymap invocations: chord presses run
// their Press or DoubleTap binding, and a press of a chord with Hold,
// Release, Click, or Drag bindings is followed (one at a time; a newer
// tracked press replaces it) until it ends. The caller runs the invocations.
class KeymapDispatcher {
  public:
    // The binding of the first keyboard chord of region pressed this frame.
    // Global chords are routed globally, so they work whichever window has
    // focus, below focused text fields; region chords need the caller to
    // check that the mouse is over the region. While the game owns the
    // keyboard, chords the game could use (chordReachesGame) are skipped.
    std::optional<KeymapInvocation> pressKeys(Keymap& keymap, KeymapRegion region,
                                              KeyboardOwner owner = KeyboardOwner::Editor);
    // Mouse buttons pressed this frame whose bit is set in allowedButtons,
    // with the modifiers held now.
    KeymapMousePress pressMouse(Keymap& keymap, KeymapRegion region, uint32_t allowedButtons);
    // Advances the followed press; runs once per ImGui frame, so later calls
    // in the same frame return nothing.
    std::vector<KeymapInvocation> update(const Keymap& keymap);
    // update, then pressMouse, passing every invocation to run; returns the
    // claimed buttons.
    uint32_t dispatchMouse(Keymap& keymap, KeymapRegion region, uint32_t allowedButtons,
                           const std::function<void(const KeymapInvocation&)>& run);
    // Stops following the current press without running anything.
    void cancel();
    const std::optional<KeymapGesture>& gesture() const;
    // Counts the presses followed so far: compare it before and after a
    // press call to learn whether that press started the current gesture.
    uint64_t gestureSerial() const;

    // Whether a binding in region uses button (an ImGuiMouseButton) with the
    // modifiers held now.
    bool mouseButtonBound(const Keymap& keymap, KeymapRegion region, int button) const;
    // Mouse bindings whose operator id fails registered neither claim a press
    // nor run, so a gesture bound to an operator nobody registered (yet)
    // leaves the press to camera navigation. Unset, every binding counts.
    void setOperatorFilter(std::function<bool(std::string_view operatorId)> registered);

  private:
    void follow(KeymapRegion region, ImGuiKeyChord chord);
    bool registered(std::string_view operatorId) const;
    // Whether chord has a binding in region whose operator is registered.
    bool claims(const Keymap& keymap, KeymapRegion region, ImGuiKeyChord chord) const;

    std::function<bool(std::string_view)> m_registered;
    std::optional<KeymapGesture> m_gesture;
    uint64_t m_gestureSerial{0};
    int m_updatedFrame{-1};
};
