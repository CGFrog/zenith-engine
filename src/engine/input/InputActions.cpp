#include "InputActions.hpp"

#include <algorithm>
#include <cmath>

namespace {
const InputActionValue emptyAction;

bool modifiersDown(const InputState& input, const InputBinding& binding) {
    return std::ranges::all_of(binding.modifiers, [&input](Key key) { return input.keyDown(key); });
}

float applyDeadZone(float value, float deadZone) {
    const float threshold = std::clamp(deadZone, 0.0f, 0.99f);
    const float magnitude = std::abs(value);
    if (magnitude <= threshold) {
        return 0.0f;
    }
    return std::copysign((magnitude - threshold) / (1.0f - threshold), value);
}

struct SampledBinding {
    float value{0.0f};
    bool pressed{false};
    bool released{false};
};

SampledBinding sample(const InputState& input, const InputBinding& binding) {
    if (!modifiersDown(input, binding)) {
        return {};
    }
    return std::visit(
        [&input, &binding](const auto& source) -> SampledBinding {
            using Source = std::decay_t<decltype(source)>;
            if constexpr (std::is_same_v<Source, Key>) {
                return {input.keyDown(source) ? 1.0f : 0.0f, input.keyPressed(source), input.keyReleased(source)};
            } else if constexpr (std::is_same_v<Source, MouseButton>) {
                return {input.mouseButtonDown(source) ? 1.0f : 0.0f, input.mouseButtonPressed(source),
                        input.mouseButtonReleased(source)};
            } else if constexpr (std::is_same_v<Source, GamepadButton>) {
                return {input.gamepadButtonDown(source) ? 1.0f : 0.0f, input.gamepadButtonPressed(source),
                        input.gamepadButtonReleased(source)};
            } else if constexpr (std::is_same_v<Source, GamepadAxis>) {
                return {.value = applyDeadZone(input.gamepadAxis(source), binding.deadZone)};
            } else {
                static_assert(std::is_same_v<Source, MouseAxis>);
                switch (source) {
                case MouseAxis::DeltaX:
                    return {.value = input.mouseDelta.x};
                case MouseAxis::DeltaY:
                    return {.value = input.mouseDelta.y};
                case MouseAxis::WheelX:
                    return {.value = input.mouseWheelDelta.x};
                case MouseAxis::WheelY:
                    return {.value = input.mouseWheelDelta.y};
                }
                return {};
            }
        },
        binding.source);
}
} // namespace

const InputActionValue& InputActionState::action(std::string_view name) const {
    const auto found = m_values.find(std::string(name));
    return found == m_values.end() ? emptyAction : found->second;
}

bool InputActionState::down(std::string_view name) const {
    return action(name).down;
}

bool InputActionState::pressed(std::string_view name) const {
    return action(name).pressed;
}

bool InputActionState::released(std::string_view name) const {
    return action(name).released;
}

float InputActionState::axis(std::string_view name) const {
    return action(name).value.x;
}

glm::vec2 InputActionState::axis2D(std::string_view name) const {
    return action(name).value;
}

InputDevice InputActionState::activeDevice() const {
    return m_activeDevice;
}

bool InputActionMap::addAction(std::string name, InputActionValueType type, bool clampValue) {
    if (name.empty()) {
        return false;
    }
    return m_actions.emplace(std::move(name), ActionDefinition{.type = type, .clampValue = clampValue}).second;
}

bool InputActionMap::merge(const InputActionMap& other, std::string& error) {
    for (const auto& [name, definition] : other.m_actions) {
        if (m_actions.contains(name)) {
            error = "Input action '" + name + "' is declared twice";
            return false;
        }
    }
    for (const auto& [name, definition] : other.m_actions) {
        m_actions.emplace(name, definition);
    }
    return true;
}

bool InputActionMap::contains(std::string_view action) const {
    return m_actions.contains(std::string(action));
}

std::vector<std::string> InputActionMap::actionNames() const {
    std::vector<std::string> names;
    names.reserve(m_actions.size());
    for (const auto& [name, definition] : m_actions) {
        names.push_back(name);
    }
    std::ranges::sort(names);
    return names;
}

bool InputActionMap::addBinding(std::string_view action, InputBinding binding) {
    const auto found = m_actions.find(std::string(action));
    if (found == m_actions.end()) {
        return false;
    }
    found->second.bindings.push_back(std::move(binding));
    return true;
}

bool InputActionMap::replaceBinding(std::string_view action, size_t index, InputBinding binding) {
    const auto found = m_actions.find(std::string(action));
    if (found == m_actions.end() || index >= found->second.bindings.size()) {
        return false;
    }
    found->second.bindings[index] = std::move(binding);
    return true;
}

bool InputActionMap::clearBindings(std::string_view action) {
    const auto found = m_actions.find(std::string(action));
    if (found == m_actions.end()) {
        return false;
    }
    found->second.bindings.clear();
    return true;
}

size_t InputActionMap::bindingCount(std::string_view action) const {
    const auto found = m_actions.find(std::string(action));
    return found == m_actions.end() ? 0 : found->second.bindings.size();
}

std::optional<InputActionValueType> InputActionMap::actionType(std::string_view action) const {
    const auto found = m_actions.find(std::string(action));
    if (found == m_actions.end()) {
        return std::nullopt;
    }
    return found->second.type;
}

bool InputActionMap::clampsValue(std::string_view action) const {
    const auto found = m_actions.find(std::string(action));
    return found != m_actions.end() && found->second.clampValue;
}

std::span<const InputBinding> InputActionMap::bindings(std::string_view action) const {
    const auto found = m_actions.find(std::string(action));
    if (found == m_actions.end()) {
        return {};
    }
    return found->second.bindings;
}

bool InputActionMap::removeAction(std::string_view action) {
    return m_actions.erase(std::string(action)) > 0;
}

void InputActionMap::overrideWith(const InputActionMap& other) {
    for (const auto& [name, definition] : other.m_actions) {
        m_actions.insert_or_assign(name, definition);
    }
}

InputActionState InputActionMap::evaluate(const InputState& input) const {
    InputActionState result;
    result.m_activeDevice = input.activeDevice;
    for (const auto& [name, definition] : m_actions) {
        InputActionValue value;
        for (const InputBinding& binding : definition.bindings) {
            const SampledBinding sampled = sample(input, binding);
            value.value += sampled.value * binding.scale;
            value.pressed |= sampled.pressed;
            value.released |= sampled.released;
        }
        if (definition.type == InputActionValueType::Button) {
            value.value = {value.value.x > 0.0f ? 1.0f : 0.0f, 0.0f};
        } else if (definition.type == InputActionValueType::Axis1D) {
            value.value.y = 0.0f;
        }
        if (definition.clampValue) {
            if (definition.type == InputActionValueType::Axis2D && glm::dot(value.value, value.value) > 1.0f) {
                value.value = glm::normalize(value.value);
            } else {
                value.value.x = std::clamp(value.value.x, -1.0f, 1.0f);
            }
        }
        value.down = glm::dot(value.value, value.value) > 0.0f;
        result.m_values.emplace(name, value);
    }
    return result;
}
