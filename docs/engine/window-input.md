# Window and input

Part of the [repository guide](../REPOSITORY_GUIDE.md).

## `src/engine/window/Window.*`

A small SDL window owner. `init` starts SDL video and creates a resizable Vulkan
window at 1280x720. `shutdown` destroys the window and quits SDL. Vulkan uses
surface capabilities and current pixel size when selecting its actual extent.

## `src/engine/input/InputManager.*`

Translates SDL-specific state into device-independent held/pressed keys,
mouse movement, gamepad buttons and axes, active-device, and capture state.
Press and release edges are buffered until the first fixed update consumes
them, so short inputs are not lost between physics ticks. Controller connection
changes release held controls rather than leaving stuck state. It also reports
quit and resize requests. Escape releases relative mouse mode; clicking the
window captures it.

## `src/engine/input/InputActions.*`

Maps physical inputs to named button, one-dimensional, and two-dimensional
actions. Bindings support keyboard chords, mouse deltas and wheels, gamepad
dead zones, multiple contributing controls, and runtime rebinding. Gameplay
owns action names and default bindings; the engine does not assign meaning to
specific controls.
