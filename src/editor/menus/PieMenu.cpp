#include "PieMenu.hpp"

#include "editor/panels/EditorContext.hpp"
#include "editor/theme/EditorPalette.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/EditorWidgets.hpp"
#include "editor/theme/PixelPainter.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace {
constexpr float diagonal = 0.70710678f;

constexpr std::array<PieDirection, maximumPieItems> fillOrder{
    PieDirection::West,      PieDirection::East,      PieDirection::South,     PieDirection::North,
    PieDirection::NorthWest, PieDirection::NorthEast, PieDirection::SouthWest, PieDirection::SouthEast};

void collectLeaves(const std::vector<MenuItem>& items, std::vector<MenuItem>& leaves) {
    for (const MenuItem& item : items) {
        if (leaves.size() == maximumPieItems) {
            return;
        }
        if (item.isGroup()) {
            collectLeaves(item.children, leaves);
        } else {
            leaves.push_back(item);
        }
    }
}

bool modifierKey(ImGuiKey key) {
    return key >= ImGuiKey_LeftCtrl && key <= ImGuiKey_RightSuper;
}

// The key or mouse button that opened the pie: one pressed this frame (the
// keymap runs wm.call_pie on the press), else one held down; ImGuiKey_None
// when nothing is held (the pie was opened from a menu).
ImGuiKey heldKey() {
    const auto find = [](bool pressedThisFrame) {
        const auto matches = [pressedThisFrame](ImGuiKey key) {
            return pressedThisFrame ? ImGui::IsKeyPressed(key, false) : ImGui::IsKeyDown(key);
        };
        for (int value = ImGuiKey_NamedKey_BEGIN; value < ImGuiKey_GamepadStart; ++value) {
            const ImGuiKey key = static_cast<ImGuiKey>(value);
            if (!modifierKey(key) && matches(key)) {
                return key;
            }
        }
        for (int value = ImGuiKey_MouseLeft; value <= ImGuiKey_MouseX2; ++value) {
            if (matches(static_cast<ImGuiKey>(value))) {
                return static_cast<ImGuiKey>(value);
            }
        }
        return ImGuiKey_None;
    };
    const ImGuiKey pressed = find(true);
    return pressed != ImGuiKey_None ? pressed : find(false);
}

ImVec2 add(ImVec2 left, ImVec2 right) {
    return {left.x + right.x, left.y + right.y};
}

ImVec2 scale(ImVec2 vector, float factor) {
    return {vector.x * factor, vector.y * factor};
}

// Rounds a length up to whole art pixels.
float artPixels(float length, float artPixel) {
    return std::ceil(length / artPixel) * artPixel;
}
} // namespace

PieDirection pieDirection(size_t index) {
    return fillOrder[std::min(index, maximumPieItems - 1)];
}

ImVec2 pieDirectionVector(PieDirection direction) {
    switch (direction) {
    case PieDirection::West:
        return {-1.0f, 0.0f};
    case PieDirection::East:
        return {1.0f, 0.0f};
    case PieDirection::South:
        return {0.0f, 1.0f};
    case PieDirection::North:
        return {0.0f, -1.0f};
    case PieDirection::NorthWest:
        return {-diagonal, -diagonal};
    case PieDirection::NorthEast:
        return {diagonal, -diagonal};
    case PieDirection::SouthWest:
        return {-diagonal, diagonal};
    case PieDirection::SouthEast:
        return {diagonal, diagonal};
    }
    return {1.0f, 0.0f};
}

std::optional<size_t> pieItemAt(ImVec2 offset, size_t itemCount, float deadZone) {
    const float length = std::hypot(offset.x, offset.y);
    if (itemCount == 0 || length < deadZone || length <= 0.0f) {
        return std::nullopt;
    }
    std::optional<size_t> nearest;
    float bestAlignment = -2.0f;
    for (size_t index = 0; index < std::min(itemCount, maximumPieItems); ++index) {
        const ImVec2 direction = pieDirectionVector(pieDirection(index));
        const float alignment = (offset.x * direction.x + offset.y * direction.y) / length;
        if (alignment > bestAlignment) {
            bestAlignment = alignment;
            nearest = index;
        }
    }
    return nearest;
}

std::vector<MenuItem> pieItems(const ResolvedMenu& menu) {
    std::vector<MenuItem> leaves;
    collectLeaves(menu.items, leaves);
    return leaves;
}

void PieMenu::open() {
    m_openRequested = true;
    m_closeRequested = false;
}

void PieMenu::close() {
    m_openRequested = false;
    m_closeRequested = true;
}

bool PieMenu::isOpen() const {
    return m_open || m_openRequested;
}

bool PieMenu::followsKey() const {
    return isOpen() && m_mode == Mode::Held;
}

std::optional<size_t> PieMenu::highlighted() const {
    return m_highlighted;
}

ImVec2 PieMenu::center() const {
    return m_center;
}

float PieMenu::radius() {
    return std::floor(ImGui::GetFontSize() * 6.0f);
}

float PieMenu::deadZone() {
    return std::floor(ImGui::GetFontSize() * 1.25f);
}

std::optional<MenuChoice> PieMenu::draw(const char* popupId, EditorContext& context, const ResolvedMenu& menu) {
    const ImGuiIO& io = ImGui::GetIO();
    if (m_openRequested) {
        m_openRequested = false;
        ImGui::OpenPopup(popupId);
        m_openedSeconds = ImGui::GetTime();
        const ImVec2 display = io.DisplaySize;
        const ImVec2 mouse = ImGui::IsMousePosValid() ? io.MousePos : scale(display, 0.5f);
        m_openMouse = mouse;
        // Keep the ring on screen.
        const float margin = std::min(radius(), std::min(display.x, display.y) * 0.5f);
        m_center = pixel::snap({std::clamp(mouse.x, margin, std::max(margin, display.x - margin)),
                                std::clamp(mouse.y, margin, std::max(margin, display.y - margin))});
        m_key = heldKey();
        m_mode = m_key != ImGuiKey_None ? Mode::Held : Mode::Clicking;
        m_highlighted.reset();
    }
    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowSize(io.DisplaySize);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoNav;
    m_open = ImGui::BeginPopup(popupId, flags);
    if (!m_open) {
        m_closeRequested = false;
        return std::nullopt;
    }
    if (m_closeRequested) {
        m_closeRequested = false;
        m_open = false;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return std::nullopt;
    }

    const std::vector<MenuItem> items = pieItems(menu);
    const ImVec2 mouse = ImGui::IsMousePosValid() ? io.MousePos : m_openMouse;
    m_mouse = mouse;
    m_highlighted = pieItemAt({mouse.x - m_center.x, mouse.y - m_center.y}, items.size(), deadZone());
    bool closing = false;
    const std::optional<MenuChoice> chosen = decide(context, items, closing);
    drawPie(context, menu, items);
    if (chosen || closing) {
        ImGui::CloseCurrentPopup();
        m_open = false;
    }
    ImGui::EndPopup();
    return chosen;
}

std::optional<MenuChoice> PieMenu::decide(EditorContext& context, const std::vector<MenuItem>& items, bool& close) {
    // The right button cancels, unless it is the held key that opened the pie.
    const bool rightCancels = !(m_mode == Mode::Held && m_key == ImGuiKey_MouseRight);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
        (rightCancels && ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
        close = true;
        return std::nullopt;
    }
    const auto pick = [&]() -> std::optional<MenuChoice> {
        if (!m_highlighted || !context.canInvoke(items[*m_highlighted].operatorId)) {
            return std::nullopt;
        }
        const MenuItem& item = items[*m_highlighted];
        return MenuChoice{.operatorId = item.operatorId, .properties = item.properties};
    };
    if (m_mode == Mode::Held) {
        if (ImGui::IsKeyDown(m_key)) {
            return std::nullopt;
        }
        // A quick tap without moving the mouse keeps the pie open for a click
        // (near a screen edge the centre moves off the mouse, so the dead
        // zone alone cannot tell).
        const bool still = std::hypot(m_mouse.x - m_openMouse.x, m_mouse.y - m_openMouse.y) < deadZone();
        if (still && ImGui::GetTime() - m_openedSeconds < tapSeconds) {
            m_mode = Mode::Clicking;
            return std::nullopt;
        }
        close = true;
        return pick();
    }
    if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        return std::nullopt;
    }
    if (!m_highlighted) {
        // A click in the middle dismisses the pie.
        close = true;
        return std::nullopt;
    }
    std::optional<MenuChoice> chosen = pick();
    close = chosen.has_value();
    return chosen;
}

void PieMenu::drawPie(EditorContext& context, const ResolvedMenu& menu, const std::vector<MenuItem>& items) const {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float artPixel = editorArtPixel();
    const float fontSize = ImGui::GetFontSize();
    const float zone = deadZone();

    // The dead zone ring, with a lit cell on it pointing at the mouse.
    const ImVec2 zoneMin{m_center.x - zone, m_center.y - zone};
    const ImVec2 zoneMax{m_center.x + zone, m_center.y + zone};
    pixel::rect(drawList, zoneMin, zoneMax, widgets::color(palette::overlayPanel));
    pixel::outline(drawList, zoneMin, zoneMax, widgets::color(palette::bevelHighlight), artPixel);
    if (m_highlighted) {
        const ImVec2 direction = pieDirectionVector(pieDirection(*m_highlighted));
        const ImVec2 cell = pixel::snap(add(m_center, scale(direction, zone - artPixel * 2.0f)));
        drawList->AddRectFilled({cell.x - artPixel, cell.y - artPixel}, {cell.x + artPixel, cell.y + artPixel},
                                widgets::color(editorAccent()));
    }

    const float paddingX = artPixel * 4.0f;
    const float paddingY = artPixel * 2.0f;
    for (size_t index = 0; index < items.size(); ++index) {
        const MenuItem& item = items[index];
        const PieDirection direction = pieDirection(index);
        const ImVec2 vector = pieDirectionVector(direction);
        const ImVec2 anchor = add(m_center, scale(vector, radius()));
        const ImVec2 textSize = ImGui::CalcTextSize(item.label.c_str());
        const ImVec2 size{artPixels(textSize.x + paddingX * 2.0f, artPixel),
                          artPixels(textSize.y + paddingY * 2.0f, artPixel)};
        ImVec2 min{anchor.x - size.x * 0.5f, anchor.y - size.y * 0.5f};
        if (vector.x < -0.1f) {
            min.x = anchor.x - size.x;
        } else if (vector.x > 0.1f) {
            min.x = anchor.x;
        }
        if (direction == PieDirection::North) {
            min.y = anchor.y - size.y;
        } else if (direction == PieDirection::South) {
            min.y = anchor.y;
        }
        min = pixel::snap(min);
        const ImVec2 max = add(min, size);
        const bool enabled = context.canInvoke(item.operatorId);
        const bool lit = enabled && m_highlighted == index;
        widgets::PaintButtonFrame(drawList, min, max,
                                  !enabled ? widgets::ButtonFrame::Disabled
                                           : (lit ? widgets::ButtonFrame::Active : widgets::ButtonFrame::Normal));
        const ThemeColor ink = !enabled ? palette::inkDisabled : (lit ? palette::activeInk : palette::ink);
        drawList->AddText(pixel::snap({min.x + (size.x - textSize.x) * 0.5f, min.y + (size.y - textSize.y) * 0.5f}),
                          widgets::color(ink), item.label.c_str());
    }

    if (!menu.title.empty()) {
        const ImVec2 titleSize = ImGui::CalcTextSize(menu.title.c_str());
        const ImVec2 position = pixel::snap({m_center.x - titleSize.x * 0.5f, m_center.y - radius() - fontSize * 3.0f});
        pixel::outlinedText(drawList, ImGui::GetFont(), fontSize, position, widgets::color(palette::captionInk),
                            widgets::color(palette::captionOutline), menu.title.c_str(), artPixel);
    }
}
