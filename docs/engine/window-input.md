# Window and input

Part of the [repository guide](../REPOSITORY_GUIDE.md).

## `src/engine/window/Window.*`

A small SDL window owner. `init` starts SDL video and creates a resizable Vulkan
window at 1280x720. `shutdown` destroys the window and quits SDL. Vulkan uses
surface capabilities and current pixel size when selecting its actual extent.

## `src/engine/input/InputManager.*` and `InputState.*`

`InputState.hpp` holds the device-independent state (`InputState`, `Key`,
`MouseButton`, gamepad buttons and axes, `InputCapture`, `InputEdgeBuffer`)
with no SDL types, so gameplay headers such as `GameUi.hpp` and
`InputActions.hpp` include it rather than `InputManager.hpp`, which is
host-only (see [Gameplay API](gameplay-api.md)). `InputManager`
translates SDL-specific state into device-independent held/pressed keys,
mouse movement, gamepad buttons and axes, active-device, and capture state.
Press and release edges are buffered until the first fixed update consumes
them, so short inputs are not lost between physics ticks. Controller connection
changes release held controls rather than leaving stuck state. It also reports
quit and resize requests. Escape releases relative mouse mode; clicking the
window captures it.

The host chooses the capture each frame with a `MouseCapturePolicy`
(`ApplicationSession::mouseCapturePolicy`: the application's, as the active
scene adjusts it). `Persistent` captures on any click until Escape;
`WhileRightMouseDown` only while the right button is held (the editor's
camera); `Released` frees a held capture and never captures, for a game's
menu (`OutsideServices::setPointerReleased`). Going from `Released` back to
`Persistent` captures the mouse again, so play resumes without an extra
click.

Files and folders dropped onto the window from the operating system
(`SDL_EVENT_DROP_FILE`) arrive in `InputState::droppedFiles` for one frame,
in drop order, each a `DroppedFile` with its path (decoded from SDL's UTF-8)
and the drop position in window coordinates. Fixed-step states never carry
them (`InputEdgeBuffer::apply` clears them), so a drop is seen once. The
engine gives drops no meaning: the editor imports them into the project
(`editor/application/FileDropQueue`), and a game may ignore them.

## `src/engine/input/InputActions.*`

Maps physical inputs to named button, one-dimensional, and two-dimensional
actions. Bindings support keyboard chords, mouse deltas and wheels, gamepad
dead zones, multiple contributing controls, and runtime rebinding. Gameplay
owns action names and default bindings; the engine does not assign meaning to
specific controls.

A game module declares its actions with `ModuleRegistrar::addInputActions`.
Declarations merge into one map (`InputActionMap::merge`), and an action
declared twice fails the registration. The host evaluates the map, and systems
read the result as `SystemContext::actions` instead of evaluating bindings:
- `GameplayRunner` evaluates it once per frame for Update, LateUpdate, and
  Ui;
- it evaluates it again for each fixed step, from that step's input, so each
  press reaches exactly one fixed step;
- the editor's `GameModuleHost` evaluates it from the input it gives systems,
  so actions are null whenever input is (Edit, Simulate).

## `src/engine/input/InputActionsJson.*`

A project's default actions as hand-editable JSON: version 1, an `actions`
array of `{name, type, clamp, bindings}` where a type is `button`, `axis1d`,
or `axis2d` and each binding is `{source, scale, deadZone, modifiers}`.
Sources are named `<kind>:<name>` (`key:W`, `mouseButton:Left`,
`mouseAxis:DeltaX`, `gamepadButton:South`, `gamepadAxis:LeftX`) by
`inputSourceName` and `parseInputSource`, so the file survives enum
reordering, unlike `InputProfiles`' numeric encoding. `InputActionMap`
exposes `actionType`, `clampsValue`, and `bindings` for it, `removeAction`,
and `overrideWith`, which replaces shared actions whole.

## `src/engine/input/InputProfiles.*`

Named binding profiles with conflict checks (`InputProfileSet`), saved to and
loaded from JSON. `applyInputProfile` overlays a profile on an action map: each
action the profile names gets the profile's bindings instead of the game's
defaults. A game rebinds the map it plays with (`GameplayRunner::actionMap`).
