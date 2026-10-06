# Runtime game UI

Part of the [repository guide](../REPOSITORY_GUIDE.md).

## `src/engine/ui/GameUi.*`

Provides a resolution-independent command API for runtime menus and HUDs.
Applications receive a `GameUiContext` through `ApplicationContext` and emit
panels, text, images, and buttons using a 1920x1080 design canvas. The context
owns anchor resolution, uniform scaling, clipping, pointer hit testing, focus,
and action-driven navigation. Game code exchanges only engine handles and
domain types; the renderer privately translates commands to its ImGui Vulkan
backend after tone mapping.

Widgets are immediate mode and built only from panel and text commands, so
the renderer and the editor's game-view overlay draw them unchanged:

- `button`: activated by a click or by `UiNavigationInput::activatePressed`
  while focused (hovering also focuses).
- `toggle(id, rect, label, bool&)`: flips its value the same way and draws a
  box that fills while on.
- `slider(id, rect, label, float&, minimum, maximum, step)`: a press on the
  widget starts a drag that sets the value from the pointer along its track
  until the button is released (even off the widget); `leftPressed` and
  `rightPressed` step the focused slider. The value is rounded to the step and
  clamped.
- `progressBar(rect, fraction)`: a background filled to the clamped fraction.
- `UiLayout::column`/`row` hand out consecutive rects with spacing, in design
  units before anchoring.
- `pixelText(position, text, size)`: text at a viewport pixel position and
  pixel size, outside the design canvas, for overlays that follow projected
  world points (the game's debug labels); the accessibility text scale still
  applies.

Font files are cached as CPU assets through `AssetManager::loadFont`, while UI
images reuse normal `TextureHandle` values and the renderer's texture cache.

### Frame flow and scaling

`Engine` owns one `GameUiContext` and exposes it to the active application
scene through `ApplicationContext`. At the start of each frame the context
captures the drawable pixel size and device-independent input snapshot. Game
code then records panels, text, texture-handle images, and buttons without
including ImGui or Vulkan headers; ImGui and its Vulkan backend remain private
renderer/editor implementation details.

The design space is 1920x1080. Uniform scaling preserves proportions at 16:9,
centers content with gutters on wider displays, and uses drawable pixels so
high-DPI windows retain stable logical sizing. Anchors, clip rectangles, draw
orders, pointer hit testing, and focused action navigation are resolved before
the renderer consumes the command list.

After the post-process pass writes the tone-mapped swapchain image, `UiPass`
translates the game commands to its private ImGui draw list and composes them
before presentation. Font bytes and texture handles cross the public boundary;
backend font atlases and Vulkan descriptors do not.

## `src/engine/ui/GameMenus.*`

Reusable screens built on the widgets, centred on the canvas, that carry no
game policy. `drawPauseMenu` returns `Resume`, `OpenSettings`, or `Quit`.
`drawSettingsMenu` draws one row per `SettingsEntry` the game supplies, each a
`SettingsToggle`, `SettingsChoice` (cycles its options), or `SettingsSlider`,
with get and set callbacks, then Back; it returns `Changed` after a setter ran
(so the game can persist its settings) or `Back`.

`Localization.*` owns stable message IDs, locale/plural selection, and UTF-8
fallback shaping. `Accessibility.*` owns timed caption tracks and
color-independent indicator semantics. `InputProfiles.*` stores named,
remappable keyboard, mouse, and gamepad profiles with explicit conflict and
hold/toggle policies.

These services stay below game policy. Stable message IDs select runtime
locales and plural forms; UTF-8 shaping emits fallback-font glyph runs. Game
UI applies text/UI scaling and high-contrast styles, captions use timed
message IDs, and indicators always include a shape and label. Versioned
control profiles preserve remaps and hold/toggle choices.
