#pragma once

#include "engine/assets/Assets.hpp"
#include "engine/input/InputManager.hpp"

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

struct UiNavigationInput {
    bool nextPressed{false};
    bool previousPressed{false};
    bool activatePressed{false};
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
    void setNavigation(UiNavigationInput navigation);
    void panel(UiRect rect, UiStyle style, UiAnchor anchor = UiAnchor::TopLeft, int32_t drawOrder = 0,
               std::optional<UiRect> clip = std::nullopt);
    void text(glm::vec2 position, std::string text, float size = 18.0f, glm::vec4 color = glm::vec4(1.0f),
              UiAnchor anchor = UiAnchor::TopLeft, int32_t drawOrder = 1, FontHandle font = {},
              std::optional<UiRect> clip = std::nullopt);
    void image(UiRect rect, TextureHandle texture, glm::vec4 tint = glm::vec4(1.0f),
               UiAnchor anchor = UiAnchor::TopLeft, int32_t drawOrder = 1, std::optional<UiRect> clip = std::nullopt);
    bool button(std::string id, UiRect rect, std::string label, UiAnchor anchor = UiAnchor::TopLeft,
                int32_t drawOrder = 1);

    std::span<const UiCommand> commands() const;
    glm::uvec2 viewportSize() const;
    float scale() const;
    std::string_view focusedId() const;
    UiRect resolve(UiRect rect, UiAnchor anchor) const;

  private:
    bool isPointerInside(const UiRect& rect) const;

    glm::uvec2 m_viewportSize{1, 1};
    glm::vec2 m_designOrigin{0.0f};
    float m_scale{1.0f};
    glm::vec2 m_pointerPosition{0.0f};
    bool m_pointerPressed{false};
    bool m_activateFocused{false};
    std::string m_focusedId;
    std::vector<std::string> m_previousFocusableIds;
    std::vector<std::string> m_focusableIds;
    std::vector<UiCommand> m_commands;
};
