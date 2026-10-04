#include "DemoInput.hpp"

namespace {
InputBinding keyBinding(Key key, glm::vec2 scale = {1.0f, 0.0f}) {
    return {.source = key, .scale = scale};
}

InputBinding gamepadButtonBinding(GamepadButton button) {
    return {.source = button};
}

InputBinding gamepadAxisBinding(GamepadAxis axis, glm::vec2 scale, float deadZone = 0.15f) {
    return {.source = axis, .scale = scale, .deadZone = deadZone};
}
} // namespace

std::shared_ptr<InputActionMap> createDemoInputMap() {
    auto actions = std::make_shared<InputActionMap>();
    actions->addAction(std::string(DemoAction::Move), InputActionValueType::Axis2D);
    actions->addBinding(DemoAction::Move, keyBinding(Key::W, {0.0f, 1.0f}));
    actions->addBinding(DemoAction::Move, keyBinding(Key::S, {0.0f, -1.0f}));
    actions->addBinding(DemoAction::Move, keyBinding(Key::D, {1.0f, 0.0f}));
    actions->addBinding(DemoAction::Move, keyBinding(Key::A, {-1.0f, 0.0f}));
    actions->addBinding(DemoAction::Move, gamepadAxisBinding(GamepadAxis::LeftX, {1.0f, 0.0f}));
    actions->addBinding(DemoAction::Move, gamepadAxisBinding(GamepadAxis::LeftY, {0.0f, -1.0f}));

    actions->addAction(std::string(DemoAction::Look), InputActionValueType::Axis2D, false);
    actions->addBinding(DemoAction::Look, {.source = MouseAxis::DeltaX, .scale = {1.0f, 0.0f}});
    actions->addBinding(DemoAction::Look, {.source = MouseAxis::DeltaY, .scale = {0.0f, 1.0f}});
    actions->addBinding(DemoAction::Look, gamepadAxisBinding(GamepadAxis::RightX, {1.0f, 0.0f}, 0.12f));
    actions->addBinding(DemoAction::Look, gamepadAxisBinding(GamepadAxis::RightY, {0.0f, 1.0f}, 0.12f));

    actions->addAction(std::string(DemoAction::Jump), InputActionValueType::Button);
    actions->addBinding(DemoAction::Jump, keyBinding(Key::Space));
    actions->addBinding(DemoAction::Jump, gamepadButtonBinding(GamepadButton::South));

    actions->addAction(std::string(DemoAction::Sprint), InputActionValueType::Button);
    actions->addBinding(DemoAction::Sprint, keyBinding(Key::LeftShift));
    actions->addBinding(DemoAction::Sprint, keyBinding(Key::RightShift));
    actions->addBinding(DemoAction::Sprint, gamepadButtonBinding(GamepadButton::LeftStick));

    actions->addAction(std::string(DemoAction::Interact), InputActionValueType::Button);
    actions->addBinding(DemoAction::Interact, keyBinding(Key::E));
    actions->addBinding(DemoAction::Interact, gamepadButtonBinding(GamepadButton::West));

    actions->addAction(std::string(DemoAction::Pause), InputActionValueType::Button);
    actions->addBinding(DemoAction::Pause, keyBinding(Key::Escape));
    actions->addBinding(DemoAction::Pause, gamepadButtonBinding(GamepadButton::Start));

    actions->addAction(std::string(DemoAction::Reload), InputActionValueType::Button);
    actions->addBinding(DemoAction::Reload, keyBinding(Key::R));
    actions->addBinding(DemoAction::Reload, gamepadButtonBinding(GamepadButton::North));

    actions->addAction(std::string(DemoAction::MenuNext), InputActionValueType::Button);
    actions->addBinding(DemoAction::MenuNext, keyBinding(Key::S));
    actions->addBinding(DemoAction::MenuNext, gamepadButtonBinding(GamepadButton::DpadDown));
    actions->addAction(std::string(DemoAction::MenuPrevious), InputActionValueType::Button);
    actions->addBinding(DemoAction::MenuPrevious, keyBinding(Key::W));
    actions->addBinding(DemoAction::MenuPrevious, gamepadButtonBinding(GamepadButton::DpadUp));
    actions->addAction(std::string(DemoAction::MenuAccept), InputActionValueType::Button);
    actions->addBinding(DemoAction::MenuAccept, keyBinding(Key::Space));
    actions->addBinding(DemoAction::MenuAccept, gamepadButtonBinding(GamepadButton::South));
    actions->addAction(std::string(DemoAction::MenuLeft), InputActionValueType::Button);
    actions->addBinding(DemoAction::MenuLeft, keyBinding(Key::A));
    actions->addBinding(DemoAction::MenuLeft, gamepadButtonBinding(GamepadButton::DpadLeft));
    actions->addAction(std::string(DemoAction::MenuRight), InputActionValueType::Button);
    actions->addBinding(DemoAction::MenuRight, keyBinding(Key::D));
    actions->addBinding(DemoAction::MenuRight, gamepadButtonBinding(GamepadButton::DpadRight));
    return actions;
}
