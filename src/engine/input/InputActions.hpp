#pragma once

#include "InputManager.hpp"

#include <glm/glm.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

enum class MouseAxis : uint8_t {
    DeltaX,
    DeltaY,
    WheelX,
    WheelY,
};

enum class InputActionValueType : uint8_t {
    Button,
    Axis1D,
    Axis2D,
};

using InputBindingSource = std::variant<Key, MouseButton, MouseAxis, GamepadButton, GamepadAxis>;

struct InputBinding {
    InputBindingSource source{Key::W};
    glm::vec2 scale{1.0f, 0.0f};
    std::vector<Key> modifiers;
    float deadZone{0.15f};
};

struct InputActionValue {
    glm::vec2 value{0.0f};
    bool down{false};
    bool pressed{false};
    bool released{false};
};

class InputActionState {
  public:
    const InputActionValue& action(std::string_view name) const;
    bool down(std::string_view name) const;
    bool pressed(std::string_view name) const;
    bool released(std::string_view name) const;
    float axis(std::string_view name) const;
    glm::vec2 axis2D(std::string_view name) const;
    InputDevice activeDevice() const;

  private:
    friend class InputActionMap;
    std::unordered_map<std::string, InputActionValue> m_values;
    InputDevice m_activeDevice{InputDevice::KeyboardMouse};
};

class InputActionMap {
  public:
    bool addAction(std::string name, InputActionValueType type, bool clampValue = true);
    bool addBinding(std::string_view action, InputBinding binding);
    bool replaceBinding(std::string_view action, size_t index, InputBinding binding);
    bool clearBindings(std::string_view action);
    size_t bindingCount(std::string_view action) const;
    InputActionState evaluate(const InputState& input) const;

  private:
    struct ActionDefinition {
        InputActionValueType type{InputActionValueType::Button};
        bool clampValue{true};
        std::vector<InputBinding> bindings;
    };

    std::unordered_map<std::string, ActionDefinition> m_actions;
};
