#include "GameUi.hpp"

#include <algorithm>

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
                       static_cast<float>(m_viewportSize.y) / designSize.y);
    m_designOrigin = (glm::vec2(m_viewportSize) - designSize * m_scale) * 0.5f;
    m_pointerPosition = input.mousePosition;
    m_pointerPressed = input.mouseButtonPressed(MouseButton::Left);
    m_activateFocused = false;
    m_previousFocusableIds = std::move(m_focusableIds);
    m_focusableIds.clear();
    m_commands.clear();
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
                          .payload = UiTextCommand{resolved, std::move(value), font, size * m_scale, color}});
}

void GameUiContext::image(UiRect rect, TextureHandle texture, glm::vec4 tint, UiAnchor anchor, int32_t drawOrder,
                          std::optional<UiRect> clip) {
    if (clip) {
        clip = resolve(*clip, anchor);
    }
    m_commands.push_back(
        {.drawOrder = drawOrder, .clip = clip, .payload = UiImageCommand{resolve(rect, anchor), texture, tint}});
}

bool GameUiContext::button(std::string id, UiRect rect, std::string label, UiAnchor anchor, int32_t drawOrder) {
    const UiRect resolved = resolve(rect, anchor);
    m_focusableIds.push_back(id);
    if (m_focusedId.empty()) {
        m_focusedId = id;
    }
    const bool hovered = isPointerInside(resolved);
    if (hovered) {
        m_focusedId = id;
    }
    const bool focused = m_focusedId == id;
    const glm::vec4 color = focused ? glm::vec4(0.18f, 0.42f, 0.72f, 0.96f) : glm::vec4(0.12f, 0.14f, 0.18f, 0.92f);
    m_commands.push_back({.drawOrder = drawOrder,
                          .payload = UiPanelCommand{resolved, {.color = color, .cornerRadius = 8.0f * m_scale}}});
    m_commands.push_back({.drawOrder = drawOrder + 1,
                          .payload = UiTextCommand{
                              {resolved.position.x + 18.0f * m_scale, resolved.position.y + resolved.size.y * 0.28f},
                              std::move(label),
                              {},
                              24.0f * m_scale,
                              glm::vec4(1.0f)}});
    return (hovered && m_pointerPressed) || (focused && m_activateFocused);
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
