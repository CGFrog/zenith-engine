#include "LassoSelect.hpp"

#include <glm/geometric.hpp>

void LassoSelect::begin(SelectionSetMode mode, bool rightDrag, glm::vec2 start) {
    m_mode = mode;
    m_points.clear();
    m_button = rightDrag ? Button::Right : Button::Left;
    m_state = rightDrag ? State::Drawing : State::Waiting;
    if (rightDrag) {
        addPoint(start);
    }
}

void LassoSelect::cancel() {
    m_state = State::Inactive;
    m_points.clear();
}

LassoSelect::State LassoSelect::state() const {
    return m_state;
}

bool LassoSelect::active() const {
    return m_state != State::Inactive;
}

bool LassoSelect::drawsWithRightButton() const {
    return m_state == State::Drawing && m_button == Button::Right;
}

void LassoSelect::addPoint(glm::vec2 point) {
    if (m_points.empty() || glm::distance(m_points.back(), point) >= minimumPointSpacing) {
        m_points.push_back(point);
    }
}

std::optional<RegionSelection> LassoSelect::update(const SelectionGestureInput& input) {
    if (m_state == State::Inactive) {
        return std::nullopt;
    }
    const bool rightCancels = m_state == State::Waiting || m_button == Button::Left;
    if (input.escapePressed || (rightCancels && input.rightPressed)) {
        cancel();
        return std::nullopt;
    }
    if (m_state == State::Waiting) {
        if (input.leftPressed && input.hovered) {
            m_state = State::Drawing;
            addPoint(input.mouse);
        }
        return std::nullopt;
    }
    addPoint(input.mouse);
    const bool buttonDown = m_button == Button::Right ? input.rightDown : input.leftDown;
    if (buttonDown) {
        return std::nullopt;
    }
    std::vector<glm::vec2> points = std::move(m_points);
    cancel();
    if (points.size() < 3) {
        return std::nullopt;
    }
    return RegionSelection{.shape = RegionShape::Lasso,
                           .points = std::move(points),
                           .mode = resolveSelectionSetMode(m_mode, input.shift, input.ctrl)};
}

void LassoSelect::draw(ImDrawList& drawList, glm::vec2 origin) const {
    if (m_state == State::Drawing) {
        drawRegionOutline(drawList, origin, RegionShape::Lasso, m_points);
    }
}
