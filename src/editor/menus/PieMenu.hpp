#pragma once

// A menu shown as a Blender pie around the mouse, drawn in the pixel theme.

#include "editor/menus/MenuPopup.hpp"
#include "editor/menus/MenuRegistry.hpp"

#include <imgui.h>

#include <cstddef>
#include <optional>
#include <vector>

struct EditorContext;

// The eight directions a pie places items in, in Blender's fill order: the
// first item goes left, then right, bottom, top, and the diagonals.
enum class PieDirection : uint8_t {
    West,
    East,
    South,
    North,
    NorthWest,
    NorthEast,
    SouthWest,
    SouthEast,
};

inline constexpr size_t maximumPieItems = 8;

// The direction of the item at index (0 to maximumPieItems - 1).
PieDirection pieDirection(size_t index);
// Unit vector of direction in screen space (y down).
ImVec2 pieDirectionVector(PieDirection direction);
// The item of a pie with itemCount items that the mouse at offset from the
// pie's centre (screen pixels) points at: the one whose direction is nearest
// in angle. nullopt inside the dead zone (deadZone pixels) or for an empty
// pie.
std::optional<size_t> pieItemAt(ImVec2 offset, size_t itemCount, float deadZone);
// The operator leaves of menu in order, categories flattened, at most
// maximumPieItems (later ones are left out).
std::vector<MenuItem> pieItems(const ResolvedMenu& menu);

// A menu shown as a Blender pie around the mouse, drawn in the pixel theme.
// Opened by a key press (wm.call_pie), it follows that key: holding it and
// letting go picks the item the mouse points at (or closes in the dead
// zone), while a quick tap leaves the pie open so a left click picks. Escape
// or a right click cancels. Items whose operator's poll fails are shown
// disabled and cannot be picked.
class PieMenu {
  public:
    // A release sooner than this, with the mouse moved less than the dead
    // zone since the pie opened, is
    // a tap.
    static constexpr double tapSeconds = 0.25;

    // Opens the pie at the next draw, centred at the mouse. The key or mouse
    // button down at that draw (the one that ran wm.call_pie) is followed
    // for press-hold-release; with none the pie waits for a click.
    void open();
    void close();
    bool isOpen() const;
    // Whether the pie waits for the key that opened it to be let go.
    bool followsKey() const;
    // The item the mouse points at in the last draw.
    std::optional<size_t> highlighted() const;
    // Screen position of the pie's centre.
    ImVec2 center() const;
    // The pie's radius and dead zone in screen pixels, from the font size.
    static float radius();
    static float deadZone();

    // Draws the open pie as a popup at the top level of the frame (the same
    // ImGui id stack for every call) and returns the item picked this frame;
    // the pie then closes.
    std::optional<MenuChoice> draw(const char* popupId, EditorContext& context, const ResolvedMenu& menu);

  private:
    enum class Mode : uint8_t {
        // The opening key is down: its release picks.
        Held,
        // Waiting for a click.
        Clicking,
    };

    std::optional<MenuChoice> decide(EditorContext& context, const std::vector<MenuItem>& items, bool& close);
    void drawPie(EditorContext& context, const ResolvedMenu& menu, const std::vector<MenuItem>& items) const;

    bool m_openRequested{false};
    bool m_closeRequested{false};
    bool m_open{false};
    Mode m_mode{Mode::Clicking};
    ImGuiKey m_key{ImGuiKey_None};
    double m_openedSeconds{0.0};
    ImVec2 m_center{0.0f, 0.0f};
    // The mouse when the pie opened and in the latest draw (screen pixels).
    ImVec2 m_openMouse{0.0f, 0.0f};
    ImVec2 m_mouse{0.0f, 0.0f};
    std::optional<size_t> m_highlighted;
};
