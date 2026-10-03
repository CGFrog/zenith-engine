#include "BoxSelect.hpp"

void BoxSelect::begin(SelectionSetMode mode) {
    m_state = State::Waiting;
    m_mode = mode;
}

void BoxSelect::cancel() {
    m_state = State::Inactive;
}

BoxSelect::State BoxSelect::state() const {
    return m_state;
}

bool BoxSelect::active() const {
    return m_state != State::Inactive;
}

std::optional<RegionSelection> BoxSelect::update(const SelectionGestureInput& input) {
    if (m_state == State::Inactive) {
        return std::nullopt;
    }
    if (input.escapePressed || input.rightPressed) {
        cancel();
        return std::nullopt;
    }
    if (m_state == State::Waiting) {
        if (input.leftPressed && input.hovered) {
            m_state = State::Dragging;
            m_start = input.mouse;
            m_current = input.mouse;
        }
        return std::nullopt;
    }
    m_current = input.mouse;
    if (input.leftDown) {
        return std::nullopt;
    }
    m_state = State::Inactive;
    return RegionSelection{.shape = RegionShape::Box,
                           .points = {m_start, m_current},
                           .mode = resolveSelectionSetMode(m_mode, input.shift, input.ctrl)};
}

void BoxSelect::draw(ImDrawList& drawList, glm::vec2 origin) const {
    if (m_state != State::Dragging) {
        return;
    }
    const glm::vec2 corners[]{m_start, m_current};
    drawRegionOutline(drawList, origin, RegionShape::Box, corners);
}
