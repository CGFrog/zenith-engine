#include "GameUi.hpp"

#include <algorithm>
#include <cmath>

namespace {
constexpr glm::vec2 designSize{1920.0f, 1080.0f};

glm::vec2 anchorPoint(UiAnchor anchor, glm::vec2 size) {
    switch (anchor) {
    case UiAnchor::TopLeft:
        return {0.0f, 0.0f};
    case UiAnchor::TopCenter:
        return {size.x * 0.5f, 0.0f};
    case UiAnchor::TopRight:
        return {size.x, 0.0f};
    case UiAnchor::CenterLeft:
        return {0.0f, size.y * 0.5f};
    case UiAnchor::Center:
        return size * 0.5f;
    case UiAnchor::CenterRight:
        return {size.x, size.y * 0.5f};
    case UiAnchor::BottomLeft:
        return {0.0f, size.y};
    case UiAnchor::BottomCenter:
        return {size.x * 0.5f, size.y};
    case UiAnchor::BottomRight:
        return size;
    }
    return {};
}
} // namespace

void GameUiContext::beginFrame(glm::uvec2 viewportSize, const InputState& input) {
    m_viewportSize = glm::max(viewportSize, glm::uvec2(1));
    m_scale = std::min(static_cast<float>(m_viewportSize.x) / designSize.x,
                       static_cast<float>(m_viewportSize.y) / designSize.y) *
              m_accessibility.uiScale;
    m_designOrigin = (glm::vec2(m_viewportSize) - designSize * m_scale) * 0.5f;
    m_pointerPosition = input.mousePosition;
    m_pointerPressed = input.mouseButtonPressed(MouseButton::Left);
    m_pointerDown = input.mouseButtonDown(MouseButton::Left);
    if (!m_pointerDown) {
        m_draggedId.clear();
    }
    m_activateFocused = false;
    m_adjustFocused = 0;
    m_previousFocusableIds = std::move(m_focusableIds);
    m_focusableIds.clear();
    m_commands.clear();
}

void GameUiContext::setAccessibility(AccessibilitySettings settings) {
    m_accessibility = settings;
}

const AccessibilitySettings& GameUiContext::accessibility() const {
    return m_accessibility;
}

void GameUiContext::setNavigation(UiNavigationInput navigation) {
    if (!m_previousFocusableIds.empty()) {
        auto found = std::ranges::find(m_previousFocusableIds, m_focusedId);
        size_t index = found == m_previousFocusableIds.end()
                           ? 0
                           : static_cast<size_t>(std::distance(m_previousFocusableIds.begin(), found));
        if (navigation.nextPressed) {
            index = (index + 1) % m_previousFocusableIds.size();
        } else if (navigation.previousPressed) {
            index = (index + m_previousFocusableIds.size() - 1) % m_previousFocusableIds.size();
        }
        m_focusedId = m_previousFocusableIds[index];
    }
    m_activateFocused = navigation.activatePressed;
    m_adjustFocused = (navigation.rightPressed ? 1 : 0) - (navigation.leftPressed ? 1 : 0);
}

void GameUiContext::panel(UiRect rect, UiStyle style, UiAnchor anchor, int32_t drawOrder, std::optional<UiRect> clip) {
    if (clip) {
        clip = resolve(*clip, anchor);
    }
    m_commands.push_back(
        {.drawOrder = drawOrder, .clip = clip, .payload = UiPanelCommand{resolve(rect, anchor), style}});
}

void GameUiContext::text(glm::vec2 position, std::string value, float size, glm::vec4 color, UiAnchor anchor,
                         int32_t drawOrder, FontHandle font, std::optional<UiRect> clip) {
    const glm::vec2 resolved = m_designOrigin + (anchorPoint(anchor, designSize) + position) * m_scale;
    if (clip) {
        clip = resolve(*clip, anchor);
    }
    m_commands.push_back({.drawOrder = drawOrder,
                          .clip = clip,
                          .payload = UiTextCommand{resolved, std::move(value), font,
                                                   size * m_scale * m_accessibility.textScale, color}});
}

void GameUiContext::image(UiRect rect, TextureHandle texture, glm::vec4 tint, UiAnchor anchor, int32_t drawOrder,
                          std::optional<UiRect> clip) {
    if (clip) {
        clip = resolve(*clip, anchor);
    }
    m_commands.push_back(
        {.drawOrder = drawOrder, .clip = clip, .payload = UiImageCommand{resolve(rect, anchor), texture, tint}});
}

GameUiContext::Focus GameUiContext::focusable(const std::string& id, const UiRect& resolved) {
    m_focusableIds.push_back(id);
    if (m_focusedId.empty()) {
        m_focusedId = id;
    }
    const bool hovered = isPointerInside(resolved);
    if (hovered) {
        m_focusedId = id;
    }
    return {.hovered = hovered, .focused = m_focusedId == id};
}

void GameUiContext::widgetFrame(const UiRect& resolved, std::string label, bool focused, int32_t drawOrder) {
    const glm::vec4 color =
        m_accessibility.highContrast
            ? (focused ? glm::vec4(1.0f, 0.8f, 0.0f, 1.0f) : glm::vec4(0.0f, 0.0f, 0.0f, 1.0f))
            : (focused ? glm::vec4(0.18f, 0.42f, 0.72f, 0.96f) : glm::vec4(0.12f, 0.14f, 0.18f, 0.92f));
    m_commands.push_back({.drawOrder = drawOrder,
                          .payload = UiPanelCommand{resolved, {.color = color, .cornerRadius = 8.0f * m_scale}}});
    m_commands.push_back({.drawOrder = drawOrder + 1,
                          .payload = UiTextCommand{
                              {resolved.position.x + 18.0f * m_scale, resolved.position.y + resolved.size.y * 0.28f},
                              std::move(label),
                              {},
                              24.0f * m_scale * m_accessibility.textScale,
                              glm::vec4(1.0f)}});
}

bool GameUiContext::button(std::string id, UiRect rect, std::string label, UiAnchor anchor, int32_t drawOrder) {
    const UiRect resolved = resolve(rect, anchor);
    const Focus focus = focusable(id, resolved);
    widgetFrame(resolved, std::move(label), focus.focused, drawOrder);
    return (focus.hovered && m_pointerPressed) || (focus.focused && m_activateFocused);
}

bool GameUiContext::toggle(std::string id, UiRect rect, std::string label, bool& value, UiAnchor anchor,
                           int32_t drawOrder) {
    const UiRect resolved = resolve(rect, anchor);
    const Focus focus = focusable(id, resolved);
    widgetFrame(resolved, std::move(label), focus.focused, drawOrder);
    const bool changed = (focus.hovered && m_pointerPressed) || (focus.focused && m_activateFocused);
    if (changed) {
        value = !value;
    }
    // A box at the right end, filled while the toggle is on.
    const float box = resolved.size.y * 0.5f;
    const UiRect outline{{resolved.position.x + resolved.size.x - box * 1.5f, resolved.position.y + box * 0.5f},
                         {box, box}};
    m_commands.push_back(
        {.drawOrder = drawOrder + 1,
         .payload = UiPanelCommand{outline, {.color = glm::vec4(0.9f), .cornerRadius = 4.0f * m_scale}}});
    const float inset = box * 0.2f;
    const glm::vec4 fill =
        value ? (m_accessibility.highContrast ? glm::vec4(0.0f, 0.0f, 0.0f, 1.0f) : glm::vec4(0.25f, 0.65f, 1.0f, 1.0f))
              : glm::vec4(0.1f, 0.1f, 0.12f, 1.0f);
    m_commands.push_back({.drawOrder = drawOrder + 2,
                          .payload = UiPanelCommand{{outline.position + inset, outline.size - inset * 2.0f},
                                                    {.color = fill, .cornerRadius = 3.0f * m_scale}}});
    return changed;
}

bool GameUiContext::slider(std::string id, UiRect rect, std::string label, float& value, float minimum, float maximum,
                           float step, UiAnchor anchor, int32_t drawOrder) {
    const UiRect resolved = resolve(rect, anchor);
    const Focus focus = focusable(id, resolved);
    widgetFrame(resolved, std::move(label), focus.focused, drawOrder);
    if (!(maximum > minimum)) {
        return false;
    }
    // The track fills the right 40% of the widget.
    const float trackHeight = resolved.size.y * 0.2f;
    const UiRect track{
        {resolved.position.x + resolved.size.x * 0.56f, resolved.position.y + (resolved.size.y - trackHeight) * 0.5f},
        {resolved.size.x * 0.4f, trackHeight}};

    const float before = value;
    if (focus.hovered && m_pointerPressed) {
        m_draggedId = id;
    }
    if (m_draggedId == id && m_pointerDown) {
        const float along = std::clamp((m_pointerPosition.x - track.position.x) / track.size.x, 0.0f, 1.0f);
        value = minimum + along * (maximum - minimum);
    } else if (focus.focused && m_adjustFocused != 0) {
        value += static_cast<float>(m_adjustFocused) * (step > 0.0f ? step : (maximum - minimum) * 0.05f);
    }
    if (step > 0.0f) {
        value = minimum + std::round((value - minimum) / step) * step;
    }
    value = std::clamp(value, minimum, maximum);

    const float fraction = (value - minimum) / (maximum - minimum);
    m_commands.push_back(
        {.drawOrder = drawOrder + 1,
         .payload = UiPanelCommand{
             track, {.color = glm::vec4(0.05f, 0.06f, 0.08f, 1.0f), .cornerRadius = trackHeight * 0.5f}}});
    m_commands.push_back(
        {.drawOrder = drawOrder + 2,
         .payload = UiPanelCommand{
             {track.position, {track.size.x * fraction, track.size.y}},
             {.color = m_accessibility.highContrast ? glm::vec4(1.0f) : glm::vec4(0.25f, 0.65f, 1.0f, 1.0f),
              .cornerRadius = trackHeight * 0.5f}}});
    const float knob = resolved.size.y * 0.4f;
    m_commands.push_back({.drawOrder = drawOrder + 3,
                          .payload = UiPanelCommand{{{track.position.x + track.size.x * fraction - knob * 0.5f,
                                                      resolved.position.y + (resolved.size.y - knob) * 0.5f},
                                                     {knob, knob}},
                                                    {.color = glm::vec4(1.0f), .cornerRadius = knob * 0.5f}}});
    return value != before;
}

void GameUiContext::progressBar(UiRect rect, float fraction, glm::vec4 fill, UiStyle background, UiAnchor anchor,
                                int32_t drawOrder) {
    const UiRect resolved = resolve(rect, anchor);
    m_commands.push_back({.drawOrder = drawOrder, .payload = UiPanelCommand{resolved, background}});
    const float filled = std::clamp(std::isfinite(fraction) ? fraction : 0.0f, 0.0f, 1.0f);
    if (filled > 0.0f) {
        m_commands.push_back(
            {.drawOrder = drawOrder + 1,
             .payload = UiPanelCommand{{resolved.position, {resolved.size.x * filled, resolved.size.y}},
                                       {.color = fill, .cornerRadius = background.cornerRadius}}});
    }
}

UiLayout::UiLayout(glm::vec2 origin, float spacing, bool vertical)
    : m_cursor(origin)
    , m_spacing(spacing)
    , m_vertical(vertical) {}

UiLayout UiLayout::column(glm::vec2 origin, float spacing) {
    return {origin, spacing, true};
}

UiLayout UiLayout::row(glm::vec2 origin, float spacing) {
    return {origin, spacing, false};
}

UiRect UiLayout::next(glm::vec2 size) {
    const UiRect rect{m_cursor, size};
    if (m_vertical) {
        m_cursor.y += size.y + m_spacing;
    } else {
        m_cursor.x += size.x + m_spacing;
    }
    return rect;
}

glm::vec2 UiLayout::cursor() const {
    return m_cursor;
}

std::span<const UiCommand> GameUiContext::commands() const {
    return m_commands;
}

glm::uvec2 GameUiContext::viewportSize() const {
    return m_viewportSize;
}

float GameUiContext::scale() const {
    return m_scale;
}

std::string_view GameUiContext::focusedId() const {
    return m_focusedId;
}

UiRect GameUiContext::resolve(UiRect rect, UiAnchor anchor) const {
    const glm::vec2 anchored = anchorPoint(anchor, designSize) + rect.position;
    return {.position = m_designOrigin + anchored * m_scale, .size = rect.size * m_scale};
}

bool GameUiContext::isPointerInside(const UiRect& rect) const {
    return m_pointerPosition.x >= rect.position.x && m_pointerPosition.y >= rect.position.y &&
           m_pointerPosition.x <= rect.position.x + rect.size.x && m_pointerPosition.y <= rect.position.y + rect.size.y;
}
