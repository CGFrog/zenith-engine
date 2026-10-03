#pragma once

#include "editor/viewport/RegionSelect.hpp"

#include <optional>

// Blender's box select (B) as a modal tool over one viewport: after begin it
// waits for a left press on the view image, follows the drag, and finishes
// with the rectangle on release. Esc or a right press cancels it at any time.
// The tool only tracks the gesture; the viewport turns the finished region
// into a pick (ViewportPicker) and applies it to the selection.
class BoxSelect {
  public:
    enum class State : uint8_t {
        Inactive,
        Waiting,
        Dragging,
    };

    // Starts waiting for the drag; mode Auto is resolved from Shift (extend)
    // and Ctrl (subtract) held at release.
    void begin(SelectionSetMode mode);
    void cancel();
    State state() const;
    bool active() const;

    // Advances the gesture by one frame; returns the box when the drag ends.
    // A cancelled gesture returns nothing and leaves the tool inactive.
    std::optional<RegionSelection> update(const SelectionGestureInput& input);
    // Draws the rectangle being dragged; origin is the view image's top-left
    // corner on screen.
    void draw(ImDrawList& drawList, glm::vec2 origin) const;

  private:
    State m_state{State::Inactive};
    SelectionSetMode m_mode{SelectionSetMode::Auto};
    glm::vec2 m_start{0.0f};
    glm::vec2 m_current{0.0f};
};
