#pragma once

// GameUiContext: immediate-mode panels, text, images, buttons, toggles, sliders, layout, anchors.

#include "engine/assets/Assets.hpp"
#include "engine/input/InputState.hpp"
#include "engine/ui/Accessibility.hpp"

#include <cstdint>
#include <glm/glm.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

enum class UiAnchor : uint8_t {
    TopLeft,
    TopCenter,
    TopRight,
    CenterLeft,
    Center,
    CenterRight,
    BottomLeft,
    BottomCenter,
    BottomRight,
};

struct UiRect {
    glm::vec2 position{0.0f};
    glm::vec2 size{0.0f};
};

struct UiStyle {
    glm::vec4 color{1.0f};
    float cornerRadius{0.0f};
};

// Menu navigation for the frame: next and previous move the focus,
// activate presses the focused widget, and left and right step the focused
// slider.
struct UiNavigationInput {
    bool nextPressed{false};
    bool previousPressed{false};
    bool activatePressed{false};
    bool leftPressed{false};
    bool rightPressed{false};
};

// Places widgets one after another in design units (before anchoring): down a
// column or along a row, with spacing between them.
class UiLayout {
  public:
    static UiLayout column(glm::vec2 origin, float spacing);
    static UiLayout row(glm::vec2 origin, float spacing);

    // The next widget's rect at the cursor; the cursor moves past it.
    UiRect next(glm::vec2 size);
    glm::vec2 cursor() const;

  private:
    UiLayout(glm::vec2 origin, float spacing, bool vertical);

    glm::vec2 m_cursor;
    float m_spacing;
    bool m_vertical;
};

struct UiPanelCommand {
    UiRect rect;
    UiStyle style;
};

struct UiTextCommand {
    glm::vec2 position{0.0f};
    std::string text;
    FontHandle font{};
    float size{18.0f};
    glm::vec4 color{1.0f};
};

struct UiImageCommand {
    UiRect rect;
    TextureHandle texture{};
    glm::vec4 tint{1.0f};
};

using UiCommandPayload = std::variant<UiPanelCommand, UiTextCommand, UiImageCommand>;

struct UiCommand {
    int32_t drawOrder{0};
    std::optional<UiRect> clip;
    UiCommandPayload payload;
};

class GameUiContext {
  public:
    void beginFrame(glm::uvec2 viewportSize, const InputState& input);
    void setAccessibility(AccessibilitySettings settings);
    const AccessibilitySettings& accessibility() const;
    void setNavigation(UiNavigationInput navigation);
    void panel(UiRect rect, UiStyle style, UiAnchor anchor = UiAnchor::TopLeft, int32_t drawOrder = 0,
               std::optional<UiRect> clip = std::nullopt);
    void text(glm::vec2 position, std::string text, float size = 18.0f, glm::vec4 color = glm::vec4(1.0f),
              UiAnchor anchor = UiAnchor::TopLeft, int32_t drawOrder = 1, FontHandle font = {},
              std::optional<UiRect> clip = std::nullopt);
    // Text placed in the viewport's pixels (top-left origin) with a size in
    // pixels, outside the design-unit layout, for overlays that follow
    // projected world positions, such as debug labels. The accessibility
    // text scale still applies.
    void pixelText(glm::vec2 position, std::string text, float size, glm::vec4 color = glm::vec4(1.0f),
                   int32_t drawOrder = 1);
    void image(UiRect rect, TextureHandle texture, glm::vec4 tint = glm::vec4(1.0f),
               UiAnchor anchor = UiAnchor::TopLeft, int32_t drawOrder = 1, std::optional<UiRect> clip = std::nullopt);
    bool button(std::string id, UiRect rect, std::string label, UiAnchor anchor = UiAnchor::TopLeft,
                int32_t drawOrder = 1);
    // A labelled on/off switch: clicking or activating it flips value.
    // Returns true when value changed this frame.
    bool toggle(std::string id, UiRect rect, std::string label, bool& value, UiAnchor anchor = UiAnchor::TopLeft,
                int32_t drawOrder = 1);
    // A labelled track from minimum to maximum: dragging it with the pointer
    // sets value (rounded to step when step is positive), and left and right
    // navigation step the focused slider. Returns true when value changed.
    bool slider(std::string id, UiRect rect, std::string label, float& value, float minimum, float maximum,
                float step = 0.0f, UiAnchor anchor = UiAnchor::TopLeft, int32_t drawOrder = 1);
    // A bar filled to fraction (clamped to 0..1) of its width.
    void progressBar(UiRect rect, float fraction, glm::vec4 fill = glm::vec4(0.25f, 0.65f, 1.0f, 1.0f),
                     UiStyle background = {.color = {0.08f, 0.09f, 0.12f, 0.9f}, .cornerRadius = 6.0f},
                     UiAnchor anchor = UiAnchor::TopLeft, int32_t drawOrder = 1);

    std::span<const UiCommand> commands() const;
    glm::uvec2 viewportSize() const;
    float scale() const;
    std::string_view focusedId() const;
    UiRect resolve(UiRect rect, UiAnchor anchor) const;

  private:
    struct Focus {
        bool hovered{false};
        bool focused{false};
    };

    bool isPointerInside(const UiRect& rect) const;
    // Registers a focusable widget and reports whether the pointer is over it
    // and whether it has the focus.
    Focus focusable(const std::string& id, const UiRect& resolved);
    // A widget's background and label, as every focusable widget draws them.
    void widgetFrame(const UiRect& resolved, std::string label, bool focused, int32_t drawOrder);

    glm::uvec2 m_viewportSize{1, 1};
    glm::vec2 m_designOrigin{0.0f};
    float m_scale{1.0f};
    glm::vec2 m_pointerPosition{0.0f};
    bool m_pointerPressed{false};
    bool m_pointerDown{false};
    bool m_activateFocused{false};
    // -1 or +1 when left or right navigation was pressed this frame.
    int m_adjustFocused{0};
    // The slider the pointer is dragging, until the button is released.
    std::string m_draggedId;
    std::string m_focusedId;
    std::vector<std::string> m_previousFocusableIds;
    std::vector<std::string> m_focusableIds;
    std::vector<UiCommand> m_commands;
    AccessibilitySettings m_accessibility;
};
