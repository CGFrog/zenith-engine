// Drives InputManager through SDL's real event queue: events are pushed with
// SDL_PushEvent and gamepads are SDL virtual joysticks, so the translation,
// capture, and device rules run exactly as they do in the engine loop. The
// window comes from SDL's offscreen video driver; the test reports skipped
// (exit code 77) where no window can be created.

#include "engine/input/InputManager.hpp"
#include "engine/window/Window.hpp"

#include "support/Skip.hpp"
#include "support/StandaloneChecks.hpp"

#include <SDL3/SDL.h>

#include <cmath>
#include <iostream>

namespace {
testsupport::StandaloneChecks checks;

bool near(float value, float expected) {
    return std::abs(value - expected) < 1e-4f;
}

void pushKey(SDL_EventType type, SDL_Scancode scancode, bool repeat = false) {
    SDL_Event event{};
    event.type = type;
    event.key.scancode = scancode;
    event.key.key = SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, false);
    event.key.down = type == SDL_EVENT_KEY_DOWN;
    event.key.repeat = repeat;
    SDL_PushEvent(&event);
}

void pushMouseButton(SDL_EventType type, uint8_t button) {
    SDL_Event event{};
    event.type = type;
    event.button.button = button;
    event.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
    SDL_PushEvent(&event);
}

void pushWheel(float x, float y, SDL_MouseWheelDirection direction) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_WHEEL;
    event.wheel.x = x;
    event.wheel.y = y;
    event.wheel.direction = direction;
    SDL_PushEvent(&event);
}

void pushType(SDL_EventType type) {
    SDL_Event event{};
    event.type = type;
    SDL_PushEvent(&event);
}

struct Harness {
    Window& window;
    InputManager input;

    explicit Harness(Window& target, bool captureMouse = false)
        : window(target) {
        input.initialize(window, captureMouse);
    }

    const InputState& update(InputCapture capture = {}, MouseCapturePolicy policy = MouseCapturePolicy::Persistent) {
        input.update(window, {}, capture, policy);
        return input.state();
    }
};

void testKeyEdges(Window& window) {
    Harness harness(window);
    pushKey(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_W);
    pushKey(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_LSHIFT);
    pushKey(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_Z);
    const InputState& pressed = harness.update();
    checks.check(pressed.keyPressed(Key::W) && pressed.keyPressed(Key::LeftShift),
                 "keys: mapped scancodes report a press");
    checks.check(!pressed.keyPressed(Key::RightShift) && !pressed.keyReleased(Key::W),
                 "keys: unrelated keys and edges stay clear");

    pushKey(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_W, true);
    const InputState& repeated = harness.update();
    checks.check(!repeated.keyPressed(Key::W), "keys: auto-repeat is not a new press, and presses last one update");

    pushKey(SDL_EVENT_KEY_UP, SDL_SCANCODE_W);
    const InputState& released = harness.update();
    checks.check(released.keyReleased(Key::W) && !released.keyPressed(Key::W), "keys: key up reports a release");

    pushKey(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE);
    const InputState& captured = harness.update({.keyboard = true});
    checks.check(!captured.keyPressed(Key::Space), "keys: keyboard capture (UI focus) hides key presses");
}

void testMouseButtonsAndWheel(Window& window) {
    Harness harness(window, true);
    pushMouseButton(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT);
    pushMouseButton(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_X2);
    pushWheel(1.0f, 2.0f, SDL_MOUSEWHEEL_NORMAL);
    pushWheel(0.0f, 1.0f, SDL_MOUSEWHEEL_FLIPPED);
    const InputState& state = harness.update();
    checks.check(state.mouseButtonPressed(MouseButton::Left) && state.mouseButtonPressed(MouseButton::X2) &&
                     !state.mouseButtonPressed(MouseButton::Right),
                 "mouse: SDL buttons map to engine buttons");
    checks.check(state.mouseWheelDelta == glm::vec2(1.0f, 1.0f),
                 "mouse: wheel deltas sum and flipped wheels are negated");

    pushMouseButton(SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT);
    checks.check(harness.update().mouseButtonReleased(MouseButton::Left), "mouse: button up reports a release");

    pushMouseButton(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT);
    pushWheel(0.0f, 3.0f, SDL_MOUSEWHEEL_NORMAL);
    const InputState& captured = harness.update({.mouse = true});
    checks.check(!captured.mouseButtonPressed(MouseButton::Left) && captured.mouseWheelDelta == glm::vec2(0.0f),
                 "mouse: mouse capture (UI hover) hides buttons and wheel");
}

void testMouseCapturePolicies(Window& window) {
    {
        Harness harness(window, false);
        pushMouseButton(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT);
        const InputState& state = harness.update();
        checks.check(state.mouseCaptured && !state.mouseButtonPressed(MouseButton::Left),
                     "capture: with the persistent policy, the click that captures the mouse is consumed");

        pushKey(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_ESCAPE);
        const InputState& escaped = harness.update();
        checks.check(!escaped.mouseCaptured && escaped.keyPressed(Key::Escape),
                     "capture: Escape releases the mouse and is still reported");
    }
    {
        Harness harness(window, false);
        pushMouseButton(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT);
        const InputState& left = harness.update({}, MouseCapturePolicy::WhileRightMouseDown);
        checks.check(!left.mouseCaptured && left.mouseButtonPressed(MouseButton::Left),
                     "capture: while-right-down ignores other buttons for capture and reports them");

        pushMouseButton(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_RIGHT);
        checks.check(harness.update({}, MouseCapturePolicy::WhileRightMouseDown).mouseCaptured,
                     "capture: while-right-down captures on the right button");

        pushMouseButton(SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_RIGHT);
        const InputState& released = harness.update({}, MouseCapturePolicy::WhileRightMouseDown);
        checks.check(!released.mouseCaptured && released.mouseButtonReleased(MouseButton::Right),
                     "capture: releasing the right button releases the mouse");
    }
    {
        Harness harness(window, false);
        pushMouseButton(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT);
        checks.check(!harness.update({.mouse = true}).mouseCaptured,
                     "capture: a click over UI never captures the mouse");
    }
    {
        Harness harness(window, false);
        pushMouseButton(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT);
        checks.check(harness.update().mouseCaptured, "capture: a persistent click captures the mouse");
        checks.check(!harness.update({}, MouseCapturePolicy::WhileRightMouseDown).mouseCaptured,
                     "capture: switching from persistent to while-right-down releases the mouse");
    }
    {
        Harness harness(window, false);
        pushMouseButton(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT);
        checks.check(harness.update().mouseCaptured, "capture: a persistent click captures the mouse");
        checks.check(!harness.update({}, MouseCapturePolicy::Released).mouseCaptured,
                     "capture: switching to released frees a held mouse");

        pushMouseButton(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT);
        pushMouseButton(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_RIGHT);
        const InputState& clicked = harness.update({}, MouseCapturePolicy::Released);
        checks.check(!clicked.mouseCaptured && clicked.mouseButtonPressed(MouseButton::Left) &&
                         clicked.mouseButtonPressed(MouseButton::Right),
                     "capture: while released, no click captures and every click is reported");

        checks.check(harness.update().mouseCaptured,
                     "capture: switching from released back to persistent captures the mouse again");
    }
}

void testWindowRequestsAndObserver(Window& window) {
    Harness harness(window);
    pushType(SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED);
    int observed = 0;
    harness.input.update(
        window, [&](const SDL_Event& event) { observed += event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ? 1 : 0; });
    checks.check(harness.input.resizeRequested() && !harness.input.quitRequested(),
                 "window: a size change requests resize");
    checks.check(observed == 1, "window: the observer sees every polled event");

    pushType(SDL_EVENT_QUIT);
    harness.update();
    checks.check(harness.input.quitRequested() && !harness.input.resizeRequested(),
                 "window: quit is requested and last update's resize is cleared");
    harness.update();
    checks.check(!harness.input.quitRequested(), "window: requests last a single update");
}

// Fixed-step simulation may run zero or several steps per frame; a press must
// survive frames with no step and be consumed exactly once.
void testFixedStepEdges(Window& window) {
    Harness harness(window);
    pushKey(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE);
    harness.update();
    harness.update();
    checks.check(!harness.input.state().keyPressed(Key::Space) && harness.input.fixedState().keyPressed(Key::Space),
                 "fixed: a press is kept for the next fixed step after the frame that saw it");
    harness.input.consumeFixedPresses();
    checks.check(!harness.input.fixedState().keyPressed(Key::Space), "fixed: consuming presses clears them");

    pushKey(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_R);
    harness.update();
    harness.input.clearBufferedInput();
    checks.check(!harness.input.fixedState().keyPressed(Key::R),
                 "fixed: clearing buffered input drops pending presses");
}

SDL_JoystickID attachVirtualGamepad() {
    SDL_VirtualJoystickDesc description;
    SDL_INIT_INTERFACE(&description);
    description.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    description.naxes = SDL_GAMEPAD_AXIS_COUNT;
    description.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    // The masks tell SDL which gamepad controls the virtual device has, so it
    // can generate a gamepad mapping for it.
    description.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1u;
    description.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1u;
    description.name = "Zenith test gamepad";
    return SDL_AttachVirtualJoystick(&description);
}

void testGamepad(Window& window) {
    const SDL_JoystickID id = attachVirtualGamepad();
    if (id == 0) {
        std::cout << "Skipping gamepad checks: no virtual joystick (" << SDL_GetError() << ")\n";
        return;
    }
    SDL_Joystick* joystick = SDL_OpenJoystick(id);
    {
        Harness harness(window);
        checks.check(harness.update().gamepadConnected, "gamepad: an attached gamepad is opened at initialization");

        SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_SOUTH, true);
        SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MIN);
        SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTY, SDL_JOYSTICK_AXIS_MAX);
        SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MAX);
        SDL_UpdateJoysticks();
        const InputState& pressed = harness.update();
        checks.check(pressed.gamepadButtonPressed(GamepadButton::South) &&
                         pressed.gamepadButtonDown(GamepadButton::South),
                     "gamepad: a new button press reports pressed and down");
        checks.check(near(pressed.gamepadAxis(GamepadAxis::LeftX), -1.0f) &&
                         near(pressed.gamepadAxis(GamepadAxis::LeftY), 1.0f),
                     "gamepad: stick extremes normalize to exactly -1 and 1 despite the asymmetric int16 range");
        checks.check(near(pressed.gamepadAxis(GamepadAxis::RightTrigger), 1.0f) &&
                         near(pressed.gamepadAxis(GamepadAxis::LeftTrigger), 0.0f),
                     "gamepad: triggers normalize to [0, 1]");
        checks.check(pressed.activeDevice == InputDevice::Gamepad, "gamepad: gamepad input makes it the active device");

        const InputState& held = harness.update();
        checks.check(held.gamepadButtonDown(GamepadButton::South) && !held.gamepadButtonPressed(GamepadButton::South),
                     "gamepad: a held button is down without a new press");

        pushKey(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_W);
        checks.check(harness.update().activeDevice == InputDevice::KeyboardMouse,
                     "gamepad: keyboard input switches the active device back");

        SDL_CloseJoystick(joystick);
        joystick = nullptr;
        SDL_DetachVirtualJoystick(id);
        const InputState& removed = harness.update();
        checks.check(!removed.gamepadConnected && removed.gamepadButtonReleased(GamepadButton::South),
                     "gamepad: disconnecting releases held buttons");
    }
    if (joystick != nullptr) {
        SDL_CloseJoystick(joystick);
    }
}
} // namespace

int main() {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    // SDL drops joystick input while the application lacks keyboard focus;
    // allow it so the gamepad checks do not depend on window focus.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::cout << "Skipping input manager tests: SDL init failed (" << SDL_GetError() << ")\n";
        return testsupport::skipExitCode;
    }
    // One window for every check: Window::shutdown() quits all of SDL,
    // including the gamepad subsystem.
    Window window;
    if (!window.init()) {
        std::cout << "Skipping input manager tests: no window (" << SDL_GetError() << ")\n";
        window.shutdown();
        return testsupport::skipExitCode;
    }
    testKeyEdges(window);
    testMouseButtonsAndWheel(window);
    testMouseCapturePolicies(window);
    testWindowRequestsAndObserver(window);
    testFixedStepEdges(window);
    testGamepad(window);
    window.shutdown();

    return checks.exitCode("input manager tests");
}
