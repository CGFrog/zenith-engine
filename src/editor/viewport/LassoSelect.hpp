#pragma once

// Blender's lasso select as a modal tool over one viewport.

#include "editor/viewport/RegionSelect.hpp"

#include <optional>
#include <vector>

// Blender's lasso select as a modal tool over one viewport. Begun from the
// Ctrl+RMB gesture it draws while the right button stays down; begun any
// other way (F3 search) it waits for a left press and draws while the left
// button stays down. Releasing the drawing button finishes the outline; Esc
// cancels at any time, as does a right press while waiting or drawing with
// the left button. An outline of fewer than three distinct points selects
// nothing and is dropped.
class LassoSelect {
  public:
    enum class State : uint8_t {
        Inactive,
        Waiting,
        Drawing,
    };

    enum class Button : uint8_t {
        Left,
        Right,
    };

    // Points closer than this (view pixels) to the previous one are skipped.
    static constexpr float minimumPointSpacing = 2.0f;

    // rightDrag: the right button is already held (the Ctrl+RMB gesture), so
    // drawing starts at once from `start` (view pixels). Mode Auto resolves
    // from Shift (extend) and Ctrl (subtract) held at release.
    void begin(SelectionSetMode mode, bool rightDrag, glm::vec2 start = glm::vec2(0.0f));
    void cancel();
    State state() const;
    bool active() const;
    // Whether the right button draws the outline (camera navigation must
    // wait for its release).
    bool drawsWithRightButton() const;

    std::optional<RegionSelection> update(const SelectionGestureInput& input);
    void draw(ImDrawList& drawList, glm::vec2 origin) const;

  private:
    void addPoint(glm::vec2 point);

    State m_state{State::Inactive};
    SelectionSetMode m_mode{SelectionSetMode::Auto};
    Button m_button{Button::Left};
    std::vector<glm::vec2> m_points;
};
